#pragma once

// Relocate the Pimorioni Display A/B/X/Y button pins to GPIO
// 0, 1, 2, 3. This is to free SPI1 pins for the SD card and the
// carrier board is needed for that (or an equivalent Pico to display pack
// rewiring)
#define RELOCATE_BUTTON_PINS

// State of the pin to detect motor on, should be 0 or 1 depending if
// connected through a NOT gate transistor (needed for Pico 2 to function properly)
// 0 for the NOT gate present
#define MOTOR_ON_STATE 0u

// Use the PIO based emulated disk rotational counter for the ATX support
// (This is more of a PIO programming exercise rather than anything else)
//#define PIO_DISK_COUNTER

// Give core1 more computing priority (not really needed)
//#define CORE1_PRIORITY
