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
 * Copyright (C) 2025 Wojciech Mostowski <wojciech.mostowski@gmail.com>
 */

#pragma once

#include <stdint.h>
#include <hardware/gpio.h>

#define MOTOR_CHECK_INTERVAL_MS 10

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

void init_io();
void reinit_pio();
void pio_enqueue(uint8_t b, uint32_t d);
bool cas_motor_on();
void flush_pio();
