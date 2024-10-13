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
#include "config.h"

#define option_count 8

extern uint8_t current_options[];

#define mount_option_index 0
#define clock_option_index 1
#define hsio_option_index 2
#define atx_option_index 3
#define xex_option_index 4
#define turbo1_option_index 5
#define turbo2_option_index 6
#define turbo3_option_index 7

extern const uint8_t *flash_config_pointer;
extern volatile bool save_config_flag;
extern volatile bool save_path_flag;

void check_and_load_config(bool reset_config);
void check_and_save_config();
