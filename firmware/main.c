/*
 * Project: Micronucleus -  v2.4
 *
 * Micronucleus V2.3             (c) 2016 Tim Bo"scke - cpldcpu@gmail.com
 *                               (c) 2014 Shay Green
 * Original Micronucleus         (c) 2012 Jenna Fox
 *
 * Based on USBaspLoader-tiny85  (c) 2012 Louis Beaudoin
 * Based on USBaspLoader         (c) 2007 by OBJECTIVE DEVELOPMENT Software GmbH
 *
 * License: GNU GPL v2 (see License.txt)
 */

#define MICRONUCLEUS_VERSION_MAJOR 2
#define MICRONUCLEUS_VERSION_MINOR 5

#include <avr/io.h>
#include <avr/pgmspace.h>
#include <avr/wdt.h>
#include <avr/boot.h>
#include <util/delay.h>

#include "bootloaderconfig.h"
#include "usbdrv/usbdrv.c"

// verify the bootloader address aligns with page size
#if (defined __AVR_ATtiny841__)||(defined __AVR_ATtiny441__)
#if BOOTLOADER_ADDRESS % ( SPM_PAGESIZE * 4 ) != 0
#error "BOOTLOADER_ADDRESS in makefile must be a multiple of chip's pagesize"
#endif
#else
#if BOOTLOADER_ADDRESS % SPM_PAGESIZE != 0
#error "BOOTLOADER_ADDRESS in makefile must be a multiple of chip's pagesize"
#endif
#endif

#if SPM_PAGESIZE>256
#error "Micronucleus only supports pagesizes up to 256 bytes"
#endif

#if ((AUTO_EXIT_MS>0) && (AUTO_EXIT_MS<1000))
#error "Do not set AUTO_EXIT_MS to below 1s to allow Micronucleus to function properly"
#endif

// Device configuration reply
// Length: 6 bytes
//   Byte 0:  User program memory size, high byte
//   Byte 1:  User program memory size, low byte
//   Byte 2:  Flash Pagesize in bytes
//   Byte 3:  Page write timing in ms.
//    Bit 7 '0': Page erase time equals page write time
//    Bit 7 '1': Page erase time equals page write time divided by 4
//   Byte 4:  SIGNATURE_1
//   Byte 5:  SIGNATURE_2

PROGMEM const uint8_t configurationReply[6] = { (((uint16_t) PROGMEM_SIZE) >> 8) & 0xff, ((uint16_t) PROGMEM_SIZE) & 0xff,
SPM_PAGESIZE,
MICRONUCLEUS_WRITE_SLEEP,
SIGNATURE_1,
SIGNATURE_2 };

typedef union {
    uint16_t w;
    uint8_t b[2];
} uint16_union_t;

#if OSCCAL_RESTORE_DEFAULT
  register uint8_t      osccal_default  asm("r2");
#endif

register uint16_union_t currentAddress asm("r4");  // r4/r5 current progmem address, used for erasing and writing
register uint16_union_t idlePolls asm("r6");  // r6/r7 idlecounter

// command system schedules functions to run in the main loop
enum {
    cmd_local_nop = 0,
    cmd_device_info = 0,
    cmd_transfer_page = 1,
    cmd_erase_application = 2,
    cmd_write_data = 3,
    cmd_exit = 4,
    cmd_write_page = 64  // internal commands start at 64
};
register uint8_t command asm("r3");  // bind command to r3

// Definition of sei and cli without memory barrier keyword to prevent reloading of memory variables
#define sei() asm volatile("sei")
#define cli() asm volatile("cli")
#define nop() asm volatile("nop")
#define wdr() asm volatile("wdr")

// Use the old delay routines without NOP padding. This saves memory.
#define __DELAY_BACKWARD_COMPATIBLE__

/* ------------------------------------------------------------------------ */
static inline void eraseApplication(void);
static void writeFlashPage(void);
static void writeWordToPageBuffer(uint16_t data);
static uint8_t usbFunctionSetup(uint8_t data[8]);
static inline void leaveBootloader(void);
void blinkLED(uint8_t aBlinkCount);

// This function is never called, it is just here to suppress a compiler warning.
USB_PUBLIC usbMsgLen_t usbFunctionDescriptor(struct usbRequest *rq) {
    return 0;
}

// erase all pages until bootloader, in reverse order (so our vectors stay in place for as long as possible)
// to minimise the chance of leaving the device in a state where the bootloader wont run, if there's power failure
// during upload
static inline void eraseApplication(void) {
    uint16_t ptr = BOOTLOADER_ADDRESS;

    while (ptr) {
#if (defined __AVR_ATtiny841__)||(defined __AVR_ATtiny441__)
    ptr -= SPM_PAGESIZE * 4;
#else
        ptr -= SPM_PAGESIZE;
#endif
        boot_page_erase(ptr);
#if (defined __AVR_ATmega328P__)||(defined __AVR_ATmega168P__)||(defined __AVR_ATmega88P__)
    // the ATmegaATmega328p/168p/88p don't halt the CPU when writing to RWW flash, so we need to wait here
    boot_spm_busy_wait();
#endif
    }

    // Reset address to ensure the reset vector is written first.
    currentAddress.w = 0;
}

// simply write currently stored page in to already erased flash memory
static inline void writeFlashPage(void) {
    if (currentAddress.w - 2 < BOOTLOADER_ADDRESS) {
        boot_page_write(currentAddress.w - 2);   // will halt CPU, no waiting required
#if (defined __AVR_ATmega328P__)||(defined __AVR_ATmega168P__)||(defined __AVR_ATmega88P__)
    // the ATmega328p/168p/88p don't halt the CPU when writing to RWW flash
    boot_spm_busy_wait();
#endif
    }
}

// Write a word into the page buffer.
// Will patch the bootloader reset vector into the main vectortable to ensure
// the device can not be bricked. Saving user-reset-vector is done in the host
// tool, starting with firmware V2
static void writeWordToPageBuffer(uint16_t data) {

#ifndef ENABLE_UNSAFE_OPTIMIZATIONS
#if BOOTLOADER_ADDRESS < 8192
    // rjmp
    if (currentAddress.w == RESET_VECTOR_OFFSET * 2) {
        data = 0xC000 + (BOOTLOADER_ADDRESS / 2) - 1;
    }
#else
  // far jmp
  if (currentAddress.w == RESET_VECTOR_OFFSET * 2) {
    data = 0x940c;
  } else if (currentAddress.w == (RESET_VECTOR_OFFSET +1 ) * 2) {
    data = (BOOTLOADER_ADDRESS/2);
  }
  #endif
#endif

#if OSCCAL_SAVE_CALIB
    if (currentAddress.w == BOOTLOADER_ADDRESS - TINYVECTOR_OSCCAL_OFFSET) {
        data = OSCCAL;
    }
#endif

    boot_page_fill(currentAddress.w, data);
    currentAddress.w += 2;
}

/* ------------------------------------------------------------------------ */
static uint8_t usbFunctionSetup(uint8_t data[8]) {
    usbRequest_t *rq = (void *) data;

    idlePolls.b[1] = 0; // reset idle polls when we get usb traffic
    if (rq->bRequest == cmd_device_info) { // get device info
        usbMsgPtr = (usbMsgPtr_t) configurationReply;
        return sizeof(configurationReply);
    } else if (rq->bRequest == cmd_transfer_page) {
        // Set page address. Address zero always has to be written first to ensure reset vector patching.
        // Mask to page boundary to prevent vulnerability to partial page write "attacks"
        if (currentAddress.w != 0) {
            currentAddress.b[0] = rq->wIndex.bytes[0] & (~(SPM_PAGESIZE - 1));
            currentAddress.b[1] = rq->wIndex.bytes[1];

            // clear page buffer as a precaution before filling the buffer in case
            // a previous write operation failed and there is still something in the buffer.
#ifdef CTPB
            __SPM_REG = (_BV(CTPB) | _BV(__SPM_ENABLE));
#else
  #ifdef RWWSRE
            __SPM_REG=(_BV(RWWSRE)|_BV(__SPM_ENABLE));
  #else
            __SPM_REG=_BV(__SPM_ENABLE);
  #endif
#endif
            asm volatile("spm");

        }
    } else if (rq->bRequest == cmd_write_data) { // Write data
        writeWordToPageBuffer(rq->wValue.word);
        writeWordToPageBuffer(rq->wIndex.word);
        if ((currentAddress.b[0] % SPM_PAGESIZE) == 0)
            command = cmd_write_page; // ask runloop to write our page
    } else {
        // Handle cmd_erase_application and cmd_exit
        command = rq->bRequest & 0x3f;
    }
    return 0;
}

static void initHardware(void) {
    MCUSR = ~_BV(WDRF); // write 0x07 to MCUSR - this should reset only bit 3/Watchdog System Reset Flag and keep the other flags

    // Disable watchdog and set timeout to maximum in case the WDT is fused on
#ifdef CCP
    // New ATtinies841/441 use a different unlock sequence and renamed registers
    CCP = 0xD8;
    WDTCSR = 1<<WDP2 | 1<<WDP1 | 1<<WDP0;
#elif defined(WDTCR)
    WDTCR = 1 << WDCE | 1 << WDE;
    WDTCR = 1 << WDP2 | 1 << WDP1 | 1 << WDP0;
#else
    wdt_reset();
    WDTCSR|=_BV(WDCE) | _BV(WDE);
    WDTCSR=0;
#endif

    usbDeviceDisconnect(); /* do this while interrupts are disabled */
    _delay_ms(300);
    usbDeviceConnect();

    usbInit();    // Initialize INT settings after reconnect
}

/* ------------------------------------------------------------------------ */
// reset system to a normal state and launch user program
static void leaveBootloader(void) __attribute__((__noreturn__));
static inline void leaveBootloader(void) {

    // bootLoaderExit() is a Macro defined in bootloaderconfig.h and mainly empty except for ENTRY_JUMPER, where it resets the pullup.
    bootLoaderExit();

#if OSCCAL_RESTORE_DEFAULT
  OSCCAL=osccal_default;
  nop(); // NOP to avoid CPU hickup during oscillator stabilization
#endif

#if (defined __AVR_ATmega328P__)||(defined __AVR_ATmega168P__)||(defined __AVR_ATmega88P__)
  // Tell the system that we want to read from the RWW memory again.
  boot_rww_enable();
#endif

    asm volatile ("rjmp __vectors - 4");
    // jump to application reset vector at end of flash

    for (;;) { // Make sure function does not return to help compiler optimize
        ;
    }
}

void USB_INTR_VECTOR(void);

int main(void) {

    // bootLoaderInit() is a Macro defined in bootloaderconfig.h and mainly empty except for ENTRY_JUMPER, where it sets the pullup and waits 1 ms.
    bootLoaderInit();

    /* save default OSCCAL calibration  */
#if OSCCAL_RESTORE_DEFAULT
  osccal_default = OSCCAL;
#endif

#if OSCCAL_SAVE_CALIB
    // adjust clock to previous calibration value, so bootloader starts with proper clock calibration
    unsigned char stored_osc_calibration = pgm_read_byte(BOOTLOADER_ADDRESS - TINYVECTOR_OSCCAL_OFFSET);
    if (stored_osc_calibration != 0xFF) {
        OSCCAL = stored_osc_calibration;
        nop();
    }
#endif
    // bootLoaderStartCondition() is a Macro defined in bootloaderconfig.h and mainly is set to true or checks a bit in MCUSR
    if (bootLoaderStartCondition() || (pgm_read_byte(BOOTLOADER_ADDRESS - TINYVECTOR_RESET_OFFSET + 1) == 0xff)) {
        /*
         * Here boot condition matches or vector table is empty / no program loaded
         */
        initHardware();
        // Set LED pin to output, if LED exists
        LED_INIT();

        // At default AUTO_EXIT_NO_USB_MS is defined 0
        if (AUTO_EXIT_NO_USB_MS > 0) {
            // At default AUTO_EXIT_MS is set to 6000
            // bias idle counter to wait for AUTO_EXIT_NO_USB_MS if no reset was detected
            idlePolls.b[1] = ((AUTO_EXIT_MS - AUTO_EXIT_NO_USB_MS) / 5) >> 8;
        } else {
            idlePolls.b[1] = 0;
        }

        command = cmd_local_nop;
        currentAddress.w = 0;

#if (OSCCAL_HAVE_XTAL == 0) && defined(START_WITHOUT_PULLUP) // Adds 14 bytes
        uint8_t resetDetected = 0; // Flag to call calibrateOscillatorASM() directly after host reset ends.
#endif

        /*
         * 1. Wait for 5 ms or USB transmission (and detect reset)
         * 2. Interpret and execute USB command
         * 3. Parse data packet and construct response (usbpoll)
         * 4. Check for timeout and exit to user program
         * 5. Blink LED
         * 6. Resynchronize USB
         */
        do {

            // 15 clockcycles per loop.
            // adjust fastctr for 5ms timeout
            uint16_t fastctr = (uint16_t) (F_CPU / (1000.0f * 15.0f / 5.0f));
            uint8_t resetctr = 100; // start value to detecting reset timing
            /*
             * Now wait for 5 ms or USB transmission
             * If reset from host via USB was detected, then calibrate OSCCAL
             * If USB transmission was detected, reset idle / USB exit counter
             */
            do {
                /*
                 * If host resets us, both lines are driven to low
                 */
                if ((USBIN & USBMASK) != 0) {
                    // No host reset here, we are unconnected with pullup or receive host data -> wait again for reset
                    resetctr = 100;

#if (OSCCAL_HAVE_XTAL == 0) && defined(START_WITHOUT_PULLUP)
            /*
             * Call calibrateOscillatorASM() only if USB is attached and after a reset, otherwise just skip calibrateOscillatorASM and wait for timeout.
             * If USB has no pullup at VCC, we will end up here if pullup was powered by USB and host reset has ended.
             */
            if (resetDetected) {
                resetDetected = 0; // do it only once after reset
                calibrateOscillatorASM();
            }
#endif

                }
                if (--resetctr == 0) {
                    // reset timing counter expired -> reset encountered here, both lines were low for around 100 us.
                    // init 2 V-USB variables as done before in reset handling of usbpoll()
                    usbNewDeviceAddr = 0;
                    usbDeviceAddr = 0;

#if (OSCCAL_HAVE_XTAL == 0)
#ifdef START_WITHOUT_PULLUP
                    resetDetected = 1;  // Call calibrateOscillatorASM() directly after host reset ends and host drives the lines.
#else
                    /*
                     * Called if we received an host reset. This waits for the D- line to toggle.
                     * It will wait forever, if no host is connected and the pullup at D- was detached.
                     * In this case we recognize a (dummy) host reset but no toggling at D- will occur.
                     */
                    calibrateOscillatorASM();
#endif
#endif  // OSCCAL_HAVE_XTAL

                }

                if (USB_INTR_PENDING & (1 << USB_INTR_PENDING_BIT)) {
                    USB_INTR_VECTOR(); // call V-USB driver for USB receiving
                    USB_INTR_PENDING = 1 << USB_INTR_PENDING_BIT; // Clear int pending, in case timeout occurred during SYNC
                    /*
                     * Commented out - see readme for 2.04.
                     * Idlepolls is now only reset when traffic to the current endpoint is detected.
                     * This will let micronucleus timeout also when traffic from other USB devices is present on the bus.
                     */
//                    idlePolls.b[1] = 0; // reset idle polls when we get usb traffic
                    break;
                }

            } while (--fastctr);

            wdr();

#if OSCCAL_SLOW_PROGRAMMING
      uint8_t osccal_tmp  = OSCCAL;
      OSCCAL      = osccal_default;
 #endif
            // commands are only evaluated after next USB transmission or after 5 ms passed
            if (command == cmd_erase_application)
                eraseApplication();
            if (command == cmd_write_page)
                writeFlashPage();
#if OSCCAL_SLOW_PROGRAMMING
      OSCCAL      = osccal_tmp;
 #endif

            if (command == cmd_exit) {
                if (!fastctr)
                    break;  // Only exit after 5 ms timeout
            } else {
                command = cmd_local_nop;
            }

            {
                // This is the old usbpoll() minus reset logic (which is done above) and double buffering (not needed anymore)
                // Parse data packet and construct response
                int8_t len;
                len = usbRxLen - 3;

                if (len >= 0) {
                    usbProcessRx(usbRxBuf + 1, len); // only single buffer due to in-order processing
                    usbRxLen = 0; /* mark rx buffer as available */
                }

                if (usbTxLen & 0x10) { /* transmit system idle */
                    if (usbMsgLen != USB_NO_MSG) { /* transmit data pending? */
                        usbBuildTxBlock();
                    }
                }
            }

            // Increment idle counter
            idlePolls.w++;

            // Try to execute program when bootloader times out
            if (AUTO_EXIT_MS && (idlePolls.w == (AUTO_EXIT_MS / 5))) {
                if (pgm_read_byte(BOOTLOADER_ADDRESS - TINYVECTOR_RESET_OFFSET + 1) != 0xff)
                    // Only exit to user program, if program exists
                    break;
            }

            // Switch LED on for 4 Idle loops every 64th idle loop - implemented by masking LSByte of idle counter with 0x4C
            LED_MACRO( idlePolls.b[0] );

            // Test whether another interrupt occurred during the processing of USBpoll and commands.
            // If yes, we missed a data packet on the bus. Wait until the bus was idle for 8.8 �s to
            // allow synchronizing to the next incoming packet.

            if (USB_INTR_PENDING & (1 << USB_INTR_PENDING_BIT))  // Usbpoll() collided with data packet
                    {
                uint8_t ctr;

                // loop takes 5 cycles
                asm volatile(
                        "         ldi  %0,%1 \n\t"
                        "loop%=:  sbis %2,%3  \n\t"
                        "         ldi  %0,%1  \n\t"
                        "         subi %0,1   \n\t"
                        "         brne loop%= \n\t"
                        : "=&d" (ctr)
                        : "M" ((uint8_t)(8.8f*(F_CPU/1.0e6f)/5.0f+0.5)), "I" (_SFR_IO_ADDR(USBIN)), "M" (USB_CFG_DMINUS_BIT)
                );
                USB_INTR_PENDING = 1 << USB_INTR_PENDING_BIT;
            }
        } while (1);

        /*
         * USB transmission timeout -> cleanup and call user program
         */
        // Set LED pin to input if LED exists
        LED_EXIT();

#ifdef USB_CFG_PULLUP_IOPORTNAME
        usbDeviceDisconnect(); // if using initHardware() instead, the port will still be active powering the pullup after disconnecting micronucleus.
#else
        initHardware(); /* Disconnect micronucleus - this set the D- to output and after 300ms to input again. */
#endif

        USB_INTR_ENABLE = 0;
        USB_INTR_CFG = 0; /* also reset config bits */

    }

    leaveBootloader();
}

/*
 * For debugging purposes
 */
void blinkLED(uint8_t aBlinkCount) {
#if LED_MODE!=NONE
    LED_INIT();
    for (uint8_t i = 0; i < MCUSR; ++i) {
#if LED_MODE==ACTIVE_HIGH
        LED_PORT |= _BV(LED_PIN);
#else
        LED_DDR|=_BV(LED_PIN);
#endif
        _delay_ms(300);
#if LED_MODE==ACTIVE_HIGH
        LED_PORT &= ~_BV(LED_PIN);
#else
        LED_DDR&=~_BV(LED_PIN);
#endif
        _delay_ms(300);
    }
    LED_EXIT();
#endif
}
/* ------------------------------------------------------------------------ */
