#pragma once

// Relocate the Pimorioni Display A/B/X/Y button pins to GPIO
// 0, 1, 2, 3. This is to free SPI1 pins for the SD card and the
// carrier board is needed for that to work (or an equivalent Pico
// to display pack rewiring)

#define RELOCATE_BUTTON_PINS

// State of the pin to detect motor on, should be 1, so do not change it.
// (The 0 option is for an earlier design that incorporated a NOT gate
// on the carries board, the option is kept in case that ever needs to
// be revived.)

#define MOTOR_ON_STATE 1u

// Use this (default) to support loading of WAV files with 96000 sample rate.
// This will overclock Pico1 boards (Pico2 seems to handle them fine with
// the stock clock) and increase the file reading buffer size.

#define WAV_96K

// Use the PIO based emulated disk rotational counter for the ATX support
// (This is more of a PIO programming exercise rather than anything else)
//#define PIO_DISK_COUNTER

// Give core1 more computing priority (not really needed)
//#define CORE1_PRIORITY

// This will make the PIO machine clock at the full board speed, this should (?)
// provide more accurate (real) SIO timing for tape emulation, however, at
// a slower speed it also works well, if not better according to many tests
// I have done.

//#define FULL_SPEED_PIO

// Additional delay for emulating the motor stopping and starting lag (applies
// to WAV files only). This was introduced in the attempt to make some "senstive"
// WAV files to load better, but in the end it does not seem to have much effect,
// left here for possible future use. Value 0 means no delay, otherwise both
// are in 10ms units of time (defined by MOTOR_CHECK_INTERVAL_MS in io.hpp) to
// define the delay.

#define MOTOR_OFF_DELAY 0 // 500 ms
#define MOTOR_ON_DELAY 0 // 500 ms
