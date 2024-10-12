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

// Use the PIO based emulated disk rotational counter for the ATX support
// (This is more of a PIO programming exercise rather than anything else)
//#define PIO_DISK_COUNTER

// Give core1 more computing priority (not really needed)
//#define CORE1_PRIORITY
