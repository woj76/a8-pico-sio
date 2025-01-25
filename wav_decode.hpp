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

#include "config.h"
#include "mounts.hpp"

extern wav_header_type wav_header;
extern volatile uint8_t wav_sample_size;
extern uint8_t wav_last_duration_bit;
extern uint32_t wav_scaled_sample_rate;
extern uint8_t wav_sample_div;
extern int16_t wav_prev_sample;
extern uint32_t wav_last_silence;
extern uint32_t wav_last_duration;
extern bool cas_last_block_marker;
extern uint32_t wav_filter_window_size;
extern uint32_t wav_last_count;
extern uint32_t wav_silence_threshold;

extern int16_t zcoeff1;
extern int16_t zcoeff2;

int32_t goertzel_int8(int8_t *x, int16_t zcoeff);
int32_t goertzel_int16(int16_t *x, int16_t zcoeff);
int16_t filter1(int16_t s);
int16_t filter2(int16_t s);
int32_t filter_avg(int32_t s);
void init_wav();
