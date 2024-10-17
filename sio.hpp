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

#include "config.h"

#include <stdint.h>

extern volatile int8_t high_speed;
extern volatile bool fs_error_or_change;

void main_sio_loop();
