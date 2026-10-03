/*
Copyright 2026

This program is free software: you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation, either version 2 of the License, or
(at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/

/* USB SOF-based HSI trim + disconnect watchdog for UniCore-F1 (STM32F103).
 *
 * This board has no HSE crystal: the 48 MHz USB clock is derived from the
 * internal HSI oscillator (mcuconf.h: HSI/2 * 12), whose +/-1..2% tolerance
 * is far outside the USB full-speed requirement of +/-0.25%. When the
 * drift crosses the edge, the host drops the device ("tears off").
 *
 * This module locks the HSI to the host clock instead: every USB Start Of
 * Frame (1 kHz, host-timed) is timestamped with TIM4 running at 48 MHz,
 * and the accumulated tick error over a window of frames drives a
 * deadband controller that nudges RCC_CR.HSITRIM, parking the HSI at the
 * trim step closest to exactly 48 MHz.
 *
 * A watchdog additionally detects a dead bus while the stack still reports
 * USB_ACTIVE (host silently dropped us) and forces a quick reconnect by
 * pulsing the D+ line (PA12) low, escalating to an MCU reset if the host
 * does not re-enumerate.
 *
 * Define USB_SOF_TRIM_DISABLE or USB_WATCHDOG_DISABLE in config.h to turn
 * the respective parts off. Define SOF_TRIM_DEBUG for periodic logging
 * through the QMK console (build with CONSOLE_ENABLE=yes).
 *
 * See USB_SOF_TRIM_PLAN.md in this directory for the full design notes.
 */

#include <ch.h>
#include <hal.h>
#include "quantum.h"
#include "timer.h"
#include "debug.h"
#include "wait.h"
#include "usb_sof_trim.h"

#if !defined(USB_SOF_TRIM_DISABLE) || !defined(USB_WATCHDOG_DISABLE)

/* ------------------------------------------------------------------------ */
/* configuration                                                             */
/* ------------------------------------------------------------------------ */

/* TIM4 is free on this board: ChibiOS uses TIM2 for the system tick
 * (STM32_ST_USE_TIMER == 2) and all GPT/PWM drivers are disabled in
 * mcuconf.h. Timer clock is 48 MHz (APB1 = HCLK/2, timer clock x2). */
#define SOF_TRIM_TICKS_PER_FRAME 48000

#ifndef SOF_TRIM_WINDOW_FRAMES
#    define SOF_TRIM_WINDOW_FRAMES 128 /* measurement window, in SOF frames (ms) */
#endif

/* HSITRIM bounds (register default is 16, legal range 0..31) */
#ifndef SOF_TRIM_MIN
#    define SOF_TRIM_MIN 4
#endif
#ifndef SOF_TRIM_MAX
#    define SOF_TRIM_MAX 28
#endif

/* Expected HSITRIM step size in ticks/frame (RM0008: ~0.4%/LSB => ~192);
 * refined at runtime from the observed error shift. */
#ifndef SOF_TRIM_STEP_TICKS
#    define SOF_TRIM_STEP_TICKS 192
#endif

/* Watchdog timings, milliseconds */
#ifndef SOF_WD_SILENCE_MS
#    define SOF_WD_SILENCE_MS 250 /* ACTIVE + no SOF for this long => dead bus */
#endif
#ifndef SOF_WD_BOOT_GRACE_MS
#    define SOF_WD_BOOT_GRACE_MS 2000
#endif
#ifndef SOF_WD_RECOVERY_MS
#    define SOF_WD_RECOVERY_MS 500 /* after the D+ pulse, before MCU reset */
#endif
#ifndef SOF_WD_COOLDOWN_MS
#    define SOF_WD_COOLDOWN_MS 5000
#endif

/* ------------------------------------------------------------------------ */
/* SOF capture (ISR context)                                                */
/* ------------------------------------------------------------------------ */

static volatile uint32_t sof_frames;      /* total SOF frames seen           */
static volatile uint32_t sof_ticks_total; /* sum of per-frame TIM4 deltas   */
static uint16_t           sof_last_cnt;   /* ISR-private                     */

/* RAM copy of QMK's USBConfig with our SOF callback installed. The driver
 * dereferences usbp->config on every event, so swapping the pointer swaps
 * the hook; QMK's own sof_cb (kbd_sof_cb + qmkusbSOFHookI) is chained. */
static USBConfig   sof_cfg_ram;
static usbcallback_t sof_orig_cb;

static bool inited = false;

static void sof_trim_sof_cb(USBDriver *usbp) {
    uint16_t cnt = (uint16_t)TIM4->CNT;
    sof_ticks_total += (uint16_t)(cnt - sof_last_cnt);
    sof_last_cnt = cnt;
    sof_frames++;
    if (sof_orig_cb) {
        sof_orig_cb(usbp);
    }
}

/* ------------------------------------------------------------------------ */
/* trim control loop (thread context, called from matrix_scan)              */
/* ------------------------------------------------------------------------ */

static bool     trim_resync      = true;
static uint32_t win_start_frames = 0;
static uint32_t win_start_ticks  = 0;
static int32_t  step_window      = (int32_t)SOF_TRIM_WINDOW_FRAMES * SOF_TRIM_STEP_TICKS;
static int32_t  err_prev         = 0;
static bool     err_valid        = false;
static uint32_t last_cntr_check  = 0;

static uint32_t hsi_trim_read(void) {
    return (RCC->CR & RCC_CR_HSITRIM) >> RCC_CR_HSITRIM_Pos;
}

static void hsi_trim_write(uint32_t trim) {
    uint32_t cr = RCC->CR & ~RCC_CR_HSITRIM;
    RCC->CR     = cr | (trim << RCC_CR_HSITRIM_Pos);
}

/* ------------------------------------------------------------------------ */
/* watchdog (thread context)                                                */
/* ------------------------------------------------------------------------ */

static uint32_t wd_boot_at        = 0;
static uint32_t wd_last_frames    = 0;
static uint32_t wd_last_sof_at    = 0;
static uint32_t wd_last_action_at = 0;
static bool     wd_pulsed         = false;
static uint32_t wd_pulse_at       = 0;

#define WD_SYSTEM_RESET()                                 \
    do {                                                  \
        SCB->AIRCR = ((0x5FAU << SCB_AIRCR_VECTKEY_Pos) & \
                      SCB_AIRCR_VECTKEY_Msk) |            \
                     SCB_AIRCR_SYSRESETREQ_Msk;           \
        for (;;) {                                        \
        }                                                 \
    } while (0)

/* Force the host to re-detect the device: drive D+ (PA12) low briefly.
 * Same trick the firmware already uses at boot in early_hardware_init_pre();
 * releasing the pad back to floating input lets the USB transceiver and the
 * board's D+ pull-up take over again. */
static void wd_pulse_dp(void) {
    palSetPadMode(GPIOA, 12, PAL_MODE_OUTPUT_PUSHPULL);
    palClearPad(GPIOA, 12);
    wait_ms(20);
    palSetPadMode(GPIOA, 12, PAL_MODE_INPUT);
}

/* ------------------------------------------------------------------------ */
/* public API                                                               */
/* ------------------------------------------------------------------------ */

void usb_sof_trim_init(void) {
    /* TIM4: free-running 16-bit counter at 48 MHz. */
    rccEnableTIM4(true);
    TIM4->CR1 = 0;
    TIM4->PSC = 0;
    TIM4->ARR = 0xFFFF;
    TIM4->EGR = TIM_EGR_UG;
    TIM4->CR1 = TIM_CR1_CEN;
    sof_last_cnt = (uint16_t)TIM4->CNT;

    /* Intercept SOF through a RAM copy of the USB driver configuration.
     * The copy/swap is race free: the ISR either sees the old or the new
     * complete config, and the pointer store is atomic. */
    chSysLock();
    sof_orig_cb = USBD1.config->sof_cb;
    sof_cfg_ram = *USBD1.config;
    sof_cfg_ram.sof_cb = sof_trim_sof_cb;
    USBD1.config = &sof_cfg_ram;
    chSysUnlock();

    /* SOF interrupts are normally already enabled (usb_lld_reset enables
     * CNTR_SOFM whenever config->sof_cb != NULL, and QMK registers one);
     * assert it in case the last bus reset predates the config swap. */
    STM32_USB->CNTR |= CNTR_SOFM;

    wd_boot_at        = timer_read32();
    wd_last_frames    = sof_frames;
    wd_last_sof_at    = wd_boot_at;
    wd_last_action_at = wd_boot_at;

    inited = true;
}

void usb_sof_trim_task(void) {
    if (!inited) {
        return;
    }

    /* Coherent snapshot of the ISR-maintained counters. */
    chSysLock();
    uint32_t frames = sof_frames;
    uint32_t ticks  = sof_ticks_total;
    chSysUnlock();
    uint32_t now = timer_read32();

    if (frames != wd_last_frames) {
        wd_last_frames = frames;
        wd_last_sof_at = now;
    }
    bool sof_stream_armed = (frames > 64);

/* ---------------------------------------------------------------- watchdog */

#if !defined(USB_WATCHDOG_DISABLE)
    if (!wd_pulsed) {
        if (sof_stream_armed && USBD1.state == USB_ACTIVE && (now - wd_last_sof_at > SOF_WD_SILENCE_MS) && (now - wd_boot_at > SOF_WD_BOOT_GRACE_MS) && (now - wd_last_action_at > SOF_WD_COOLDOWN_MS)) {
            /* Host went silent on a bus we still consider active: the
             * classic "torn off" signature. Force a re-detect. */
            wd_pulse_dp();
            wd_pulsed         = true;
            wd_pulse_at       = now;
            wd_last_action_at = now;
        }
    } else {
        if (frames != wd_last_frames) {
            /* SOF flowing again: recovered. */
            wd_pulsed = false;
        } else if (now - wd_pulse_at > SOF_WD_RECOVERY_MS) {
            /* Still dead: reset for a clean re-enumeration. WAIT_FOR_USB
             * makes the firmware block instead of looping if the cable is
             * actually gone. */
            WD_SYSTEM_RESET();
        }
    }
#endif /* !USB_WATCHDOG_DISABLE */

/* -------------------------------------------------------------- trim loop */

#if !defined(USB_SOF_TRIM_DISABLE)
    /* Keep SOF interrupts armed even if something rewrote CNTR. */
    if (now - last_cntr_check > 1000) {
        last_cntr_check = now;
        if ((STM32_USB->CNTR & CNTR_SOFM) == 0) {
            STM32_USB->CNTR |= CNTR_SOFM;
        }
    }

    if (USBD1.state != USB_ACTIVE) {
        /* Suspended / not configured: no host-timed SOF to measure. */
        trim_resync = true;
        return;
    }

    if (trim_resync) {
        win_start_frames = frames;
        win_start_ticks  = ticks;
        trim_resync      = false;
        return;
    }

    uint32_t df = frames - win_start_frames;
    if (df < SOF_TRIM_WINDOW_FRAMES) {
        return;
    }

    int32_t dt       = (int32_t)(ticks - win_start_ticks);
    int32_t expected = (int32_t)(df * SOF_TRIM_TICKS_PER_FRAME);
    int32_t err      = dt - expected; /* clock fast => positive */

    /* Track the observed HSITRIM step effect (window-to-window error shift)
     * so the deadband adapts to the real silicon. */
    if (err_valid) {
        int32_t d = err - err_prev;
        if (d < 0) d = -d;
        if (d > 256 && d < (int32_t)(df * 2000)) {
            step_window = (step_window * 3 + d) / 4;
        }
    }
    err_prev  = err;
    err_valid = true;

    uint32_t trim = hsi_trim_read();
    int32_t  half = step_window / 2;

#    ifdef SOF_TRIM_DEBUG
    int32_t ppm = (int32_t)(((int64_t)err * 1000000) / ((int64_t)df * SOF_TRIM_TICKS_PER_FRAME));
#    endif

    if (err > half && trim > SOF_TRIM_MIN) {
        /* Clock too fast: slow the HSI down one step. */
        hsi_trim_write(trim - 1);
#    ifdef SOF_TRIM_DEBUG
        xprintf("softrim: +%d ppm, trim %u -> %u, step %d\n", (int)ppm, (unsigned)trim, (unsigned)(trim - 1), (int)step_window);
#    endif
    } else if (err < -half && trim < SOF_TRIM_MAX) {
        /* Clock too slow: speed the HSI up one step. */
        hsi_trim_write(trim + 1);
#    ifdef SOF_TRIM_DEBUG
        xprintf("softrim: %d ppm, trim %u -> %u, step %d\n", (int)ppm, (unsigned)trim, (unsigned)(trim + 1), (int)step_window);
#    endif
    }
#    ifdef SOF_TRIM_DEBUG
    else {
        static uint32_t last_log = 0;
        if (now - last_log > 2000) {
            last_log = now;
            /* Endpoint numbers are NOT the usb_descriptor.h enum on this
             * build -- the two disagree, and only the built descriptor is
             * authoritative. Read straight from the compiled artifact
             * (obj_ydkb_unicore_f1_dusk67_via/usb_descriptor.o), matching each
             * interface by its HID report length
             * (68=Keyboard, 34=RawHID, 182=Shared):
             *
             *   iface 0  report  68  0x81 IN  ep1   KEYBOARD
             *   iface 1  report  34  0x82 IN  ep2   RAW IN
             *                          0x03 OUT ep3   RAW OUT
             *   iface 2  report 182  0x84 IN  ep4   SHARED / CONSOLE
             *
             * The enum instead yields IN=3 / OUT=4, because it counts the
             * shared IN endpoint in the sequence; on this build the raw
             * interface is emitted before the shared one. Using the enum here
             * reads the wrong register, so the literals below are deliberate.
             *
             * STAT_RX occupies bits 13:12 (EPR_STAT_RX_MASK == 0x3000) and
             * says whether the host has a receive buffer armed on the raw-HID
             * OUT endpoint: 00=DISABLED 01=STALL 10=NAK 11=VALID. NAK is the
             * normal armed state between transfers; DISABLED while USBD1.state
             * is USB_ACTIVE is the "host dropped us but the stack still thinks
             * we are up" signature this module exists to catch.
             */
            uint32_t epr_raw_out = STM32_USB->EPR[3];
            uint32_t epr_raw_in  = STM32_USB->EPR[2];
            unsigned stat_rx     = (unsigned)(epr_raw_out & EPR_STAT_RX_MASK) >> 12;
            xprintf("softrim: %d ppm, trim %u, step %d, state %d, CNTR %04x, rawout(ep3) %04x rx %u, rawin(ep2) %04x, sof %u\n",
                    (int)ppm,
                    (unsigned)hsi_trim_read(),
                    (int)step_window,
                    (int)USBD1.state,
                    (unsigned)STM32_USB->CNTR,
                    (unsigned)epr_raw_out,
                    stat_rx,
                    (unsigned)epr_raw_in,
                    (unsigned)sof_frames);
        }
    }
#    endif

    /* Slide the window. */
    win_start_frames = frames;
    win_start_ticks  = ticks;
#endif /* !USB_SOF_TRIM_DISABLE */
}

#else /* both disabled: stubs */

void usb_sof_trim_init(void) {}

void usb_sof_trim_task(void) {}

#endif
