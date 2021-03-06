#ifndef TWIMaster_h
#define TWIMaster_h

#include <stdint.h>
#include <stddef.h>
#include <avr/io.h>
#include <avr/interrupt.h>

// For now the timer is hardwired to Timer5; changing that would require at least #ifdefs
// for the ISR, as there is no way to parameterize it after the preprocessor has run
// (remember that ISR(...) is a macro). Either all of the relevant timer registers would
// need to be passed as arguments and stored (requiring pointer access), #ifdefed into
// place (ugly), or done via template (which would require #pragma push_macro and 
// redefinitions of the _SFR_{MEM|IO}{8|16} macros). See ./metaprogramming.txt for an
// example which compiles.
#define USINGTIMER

/* hardware-specific config
*/
#define TWI_TWBR 0x0C // 400 KHz
#define TWI_READ_BIT  0       // Bit position for R/W bit in "address byte".
#define TWI_ADR_BITS  1       // Bit position for LSB of the slave address bits in the init byte.
#define TWSR_STATUS_MASK 0xF8 // 3 LSB are baud rate prescalar
#define STATE_SUCCESS_BIT 0   // this bit is set in sate_s.state on callback if no error
#define STATE_TIMEOUT_BIT 1   // this bit is set in sate_s.state on callback if no error
#define TIMEOUT_TWI_CLOCKS 32 // the absolute minimum is 13, but that assumes zero delay in the slave


/* debugging
*/
#define PIN_iFree (1<<PA5)
#define PIN_iCmd  (1<<PA6)
#define PIN_iCallback (1<<PA7)

#define INC_INDEX(idx) //PINA |= idx


/* TWI Queue
*/
void kick_isr();

// size is 2^NQBits, with max useful size of 2^NQBits - 1
// (ring buffer needs >= 1 empty spot to avoid more complicated management)
#define NQBits 4
struct state_s;

typedef void (*callback_fp)(struct state_s*);
typedef void (*state_fp)();
typedef int qindex;

typedef struct state_s {
  char *buff;
  char addr;
  char state;
  char len;
  callback_fp donefunc;
} state_t;

#define NOSTATE ((state_t*)0)


class twiQueue {
private:
  enum { qSize = (1<<NQBits),
         qMask = (1<<NQBits)-1 };

  state_t queue[qSize];
  qindex iCmd, iCallback, iFree;

public:
  twiQueue() : iCmd(0), iCallback(0), iFree(0)
  {}

  inline qindex nextIndex(qindex index) {
    return (index + 1) & qMask;
  }

  inline bool validIndex(qindex index) {
    return 0 <= index && index < qSize;
  }

  // returns
  //   state_t* if a slot is free
  //   NOSTATE otherwise
  state_t* allocFree() {
    qindex old = iFree;
    qindex i = nextIndex(iFree);

    if (i == iCallback)
      return NOSTATE;

    INC_INDEX(PIN_iFree);
    iFree = i;

    return &queue[old];
  }

  // only valid if hasCmd()
  state_t& currCmd() {
    //assert( validIndex(iCmd) && hasCmd() );
    return queue[iCmd];
  };

  void     doneCmd() {
    //assert( validIndex(iCmd) && hasCmd() );
    INC_INDEX(PIN_iCmd);
    iCmd = nextIndex(iCmd);
  };

  bool     hasCmd() {
    return iCmd != iFree;
  };

  state_t& currCallback() {
    //assert( validIndex(iCallback) && hasCallback() );
    return queue[iCallback];
  };

  void     doneCallback() {
    //assert( validIndex(iCallback) && hasCallback() );
    INC_INDEX(PIN_iCallback);
    iCallback = nextIndex(iCallback);
  }

  bool     hasCallback() {
    return iCallback != iCmd;
  }

private:
  // must be called with interrupts disabled
  inline bool enqueue_rw_crit(char addr_rw, char *data, uint8_t len, callback_fp donefunc) {
    state_t *p = allocFree();

    if (p == NOSTATE)
      return false;

    p->buff = data;
    p->addr = addr_rw;
    p->len = len;
    p->donefunc = donefunc;

    kick_isr();

    return true;
  }

  inline bool enqueue_rw(char addr_rw, char *data, uint8_t len, callback_fp donefunc) {
    uint8_t sreg = SREG;
    cli();

    bool ret = enqueue_rw_crit(addr_rw, data, len, donefunc);

    SREG = sreg;

    return ret;
  }

  static volatile bool _blocking_callback_called;
  static volatile char _blocking_state;
  static callback_fp _blocking_donefunc;
  static void blocking_callback(state_t *s);

  // note that this may enable interrupts during execution
  bool enqueue_rwb(char addr_rw, char *data, uint8_t len, callback_fp donefunc) {
    // we could initialize this to true, and assert on it being false to prevent re-entry
    _blocking_callback_called = false;
    _blocking_donefunc = donefunc;
    uint8_t sreg = SREG;
    cli();

    // wait until the enqueue succeeds
    while (!enqueue_rw_crit(addr_rw, data, len, blocking_callback)) {
      // let TWI interrupts fire
      sei();
      // one instruction is always executed after sei(), so we cannot cli() immediately after
      asm volatile ("nop");
      cli();
    }

    sei();
    while (!_blocking_callback_called)
    {}
    SREG = sreg;

    return _blocking_state & (1<<STATE_SUCCESS_BIT);
  }

public:
  // returns true if enqueue was successful, otherwise false for full TWI command buffer
  bool enqueue_r(char addr, char *data, uint8_t len, callback_fp donefunc) {
    return enqueue_rw((addr<<TWI_ADR_BITS) | (1<<TWI_READ_BIT), data, len, donefunc);
  }

  // returns true on TWI success, otherwise false (use donefunc for full status info)
  bool enqueue_rb(char addr, char *data, uint8_t len, callback_fp donefunc) {
    return enqueue_rwb((addr<<TWI_ADR_BITS) | (1<<TWI_READ_BIT), data, len, donefunc);
  }

  // returns true if enqueue was successful, otherwise false for full TWI command buffer
  bool enqueue_w(char addr, char *data, uint8_t len, callback_fp donefunc) {
    return enqueue_rw((addr<<TWI_ADR_BITS) | (0<<TWI_READ_BIT), data, len, donefunc);
  }

  // returns true on TWI success, otherwise false (use donefunc for full status info)
  bool enqueue_wb(char addr, char *data, uint8_t len, callback_fp donefunc) {
    return enqueue_rwb((addr<<TWI_ADR_BITS) | (0<<TWI_READ_BIT), data, len, donefunc);
  }

  // returns true if enqueue was successful, otherwise false for full TWI command buffer
  bool enqueue_nop(callback_fp donefunc) {
    uint8_t sreg = SREG;
    cli();

    state_t *p = allocFree();

    if (p == NOSTATE) {
      SREG = sreg;
      return false;
    }

    p->len = 0;
    p->donefunc = donefunc;

    // need to do something like kick_isr() here, except that initiates a TWI START condition

    sei();
    while (!_blocking_callback_called)
    {}
    SREG = sreg;

    return true;
  }

  // while you might think this could be called wait_for_empty_queue(), it is actually possible
  // for more TWI commands to be enqueued from callbacks while this is processing
  void enqueue_nop_b() {
    // we could initialize this to true, and assert on it being false to prevent re-entry
    _blocking_callback_called = false;
    _blocking_donefunc = NULL;

    // wait until the enqueue succeeds
    while (!enqueue_nop(blocking_callback))
    {}

    uint8_t sreg = SREG;
    sei();
    while (!_blocking_callback_called)
    {}
    SREG = sreg;
  }
};

extern twiQueue twiQ;

static void i2c_master_initialize(void) {
  TWBR = TWI_TWBR;                        // baud rate
  TWSR &= ~((1<<TWPS1) | (1<<TWPS0));     // ensure there is no baud rate prescalar
  //TWDR = 0xFF;                            // default content = SDA released
  TWCR = (1<<TWEN)|                       // enable TWI interface and release TWI pins
         (0<<TWIE)|(0<<TWINT)|            // disable interupt
         (0<<TWEA)|(0<<TWSTA)|(0<<TWSTO)| // don't actually start anything
         (0<<TWWC);
  
  // from datasheet: SCL frequency = F_CPU / (16+2(TWBR)*4**(TWPS))
  // max freq is 400kHz, and we should probably allow for 16 clocks to be safe
  // (noting that this limits how long TWISlaveMem14's TWIUserSignal(...) can take)
  // 16MHz / 40 == 400kHz, which means we need a prescaler of 40 * 16 = 640 for the timer
  // to be 1 clock before timeout. But that's actually bad, as outlined in the "Prescalar 
  // Reset" section of the datasheet. Instead, we want to max out how high the timer will
  // count. If using a 16-bit timer, we'll want to allow up to 640 * 4 (allowing for 
  // 100kHz I2C freq), which means we can run with no prescaler. If using an 8-bit timer,
  // we'd have to at least use the /8 option.
  
  #ifdef USINGTIMER
  DDRL |= (1<<PL3);     // for debugging
  TCCR5A = (1<<COM5A0); // for debugging
  TCCR5B = (1<<WGM52) | (1<<CS50);
  TIMSK5 = 0;
  TIFR5 = 0xFF;
  // the + 1 is to avoid any possible problem if a nonzero prescaler is used above
  //                           this formula is from the datasheet, with TWPS = 0
  //       vvvvvvvvvvvvvvvvvvv (F_CPU is omitted because it is a common factor)
  OCR5A = ((16 + 2 * TWI_TWBR) * TIMEOUT_TWI_CLOCKS) + 1;
  #endif
}


#endif // #ifndef TWIMaster_h
