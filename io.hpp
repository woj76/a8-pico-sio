/*
 * This file is part of the a8-pico-sio project --
 * An Atari 8-bit SIO drive and (turbo) tape emulator for
 * Raspberry Pi Pico, see
 *
 *         https://github.com/woj76/a8-pico-sio
 *
 * For information on what / whose work it is based on, check the corresponding
 * source files and the README file. This file is licensed under GNU General
 * Public License 3.0 or later.
 *
 * Copyright (C) 2024 Wojciech Mostowski <wojciech.mostowski@gmail.com>
 */

#pragma once

#include <stdint.h>
#include <hardware/gpio.h>
#include "config.h"

// Somewhere in the process the PIO queue was increased to 96 elements

// Retest these two with 96 queue size
// Pico 1 + 1000000 clock with half scaling on silence and bits -> no load
// Pico 1 + 1000000 clock with half scaling on silence-> garbled

// Pico 1 + 1000000 clock with no half scaling of sample rate -> OK
// Pico 1 - full clock with "half scaling" on silence -> OK

// Pico 1 - full clock with "half scaling" on silence and bits -> OK
// Pico 1 - full clock and no "half scaling" -> OK debounce

// Pico 2 - full clock and no "half scaling" -> no load

// Pico 2 - full clock with "half scaling" on silence and bits -> OK once, no load
// Pico 2 - full clock with "half scaling" on silence -> OK

// Pico 2 + 1000000 clock with half scaling on silence and bits -> no load (weird loading sound)
// Pico 2 + 1000000 clock with half scaling on silence-> no load

// Pico 2 - 1000000 clock and no "half scaling" -> OK (really nice sound!)

// Pico 2, 1000000, no /2 - good sound, SF OK, Draconus Not OK, SF CAS OK
// Pico 2, full, no /2 - little burping sound, SF OK, Draconus Not OK, SF CAS OK

// Pico 1, full, no /2 - little uneven sound, SF failed twice, Draconus , SF CAS
// Pico 1, 1000000, no/2 - good sound?, SF OK, Draconus , SF CAS


// Best candidate: half scaling on silence and bits (most mathematically accurate?)
// After this check again with pio queue size == 64
// Test Draconus
// Then test everything else on both Picos! Especially Boulder Dash, Spy vs. Spy
// Move the motor delay to config.h, or remove altogether (if Super Fortuna works there is no point)
// Why is the led blinking upon mounting

// Pico 1
// Overclock and full clock PIO
// 0, 0 loaded the version without debounce
// 10, 0 did not work
// 0, 10 did not work

// Overclock and 1000 PIO clock
// 0, 0 loaded the version without debounce
// 10, 0 loaded garbled
// 0, 10 loaded the version without debounce + garbled
// 0, 60 loaded garbled

#define MOTOR_CHECK_INTERVAL_MS 10
#define MOTOR_OFF_DELAY 0 // 500 ms
#define MOTOR_ON_DELAY 0 // 500 ms

#define cas_pio pio0
#define GPIO_FUNC_PIOX GPIO_FUNC_PIO0

// extern const uint led_pin;

extern uint turbo_data_pin;

extern const uint sio_tx_pin;
extern const uint sio_rx_pin;
extern const uint normal_motor_pin;
extern const uint command_line_pin;
extern const uint proceed_pin;
extern const uint interrupt_pin;
extern const uint joy2_p1_pin;
extern const uint joy2_p2_pin;
extern const uint joy2_p4_pin;

extern const uint32_t normal_motor_pin_mask;
extern const uint32_t normal_motor_value_on;

extern const uint32_t kso_motor_pin_mask;
extern const uint32_t kso_motor_value_on;

extern const uint32_t comm_motor_pin_mask;
extern const uint32_t comm_motor_value_on;

extern const uint32_t sio_motor_pin_mask;
extern const uint32_t sio_motor_value_on;

extern uint32_t timing_base_clock;
extern uint32_t max_clock_ms;

extern uint32_t turbo_motor_pin_mask;
extern uint32_t turbo_motor_value_on;

extern int8_t turbo_conf[];
extern const uint hsio_opt_to_baud_ntsc[];
extern const uint hsio_opt_to_baud_pal[];


extern volatile bool cas_block_turbo;
extern volatile bool dma_block_turbo;
extern uint sm;
extern uint sm_turbo;
extern volatile uint8_t wav_sample_size;

void init_io();
void reinit_pio();
void pio_enqueue(uint8_t b, uint32_t d);
bool cas_motor_on();
void flush_pio();
