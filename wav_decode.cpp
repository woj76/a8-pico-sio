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

#include <math.h>

#include "wav_decode.hpp"
#include "io.hpp"
#include "options.hpp"

uint32_t wav_avg_reads;
uint wav_avg_offset;
int32_t wav_avg_sum;
uint32_t wav_silence_threshold;
wav_header_type wav_header;
volatile uint8_t wav_sample_size;
uint8_t wav_sample_div;
uint32_t wav_filter_window_size;
uint32_t wav_scaled_sample_rate;
uint32_t wav_last_silence;
uint32_t wav_last_count;
bool cas_last_block_marker;
uint32_t wav_last_duration;
uint8_t wav_last_duration_bit;
int16_t wav_prev_sample;
bool wav_filter1_started;
bool wav_filter2_started;

int16_t zcoeff1;
int16_t zcoeff2;

int32_t goertzel_int8(int8_t *x, int16_t zcoeff) {
	int32_t z;
	int32_t zprev = 0;
	int32_t zprev2 = 0;
	int8_t xn;
	for(int n=0; n<wav_filter_window_size; n += wav_sample_div) {
		z = (x[n*wav_header.num_channels]<<2) + ((zcoeff*zprev)>>14) - zprev2;
		zprev2 = zprev;
		zprev = z;
	}
	return (zprev2*zprev2 + zprev*zprev - ((zcoeff*zprev)>>14)*zprev2) >> 5;
}

int32_t goertzel_int16(int16_t *x, int16_t zcoeff) {
	int32_t z;
	int32_t zprev = 0;
	int32_t zprev2 = 0;
	for(int n=0; n<wav_filter_window_size; n += wav_sample_div) {
		z = (x[n*wav_header.num_channels]>>6) + ((zcoeff*zprev)>>14) - zprev2;
		zprev2 = zprev;
		zprev = z;
	}
	return (zprev2*zprev2 + zprev*zprev - ((zcoeff*zprev)>>14)*zprev2) >> 5;
}

int16_t filter1(int16_t s) {
	int16_t rs;
	static int16_t prs;
	static int16_t ps;
	if(!wav_filter1_started) {
		rs = s;
		ps = s;
		prs = rs;
	} else {
		rs = prs*4/10 + (s-ps)*4/10;
		ps = s;
		prs = rs;
	}
	wav_filter1_started = true;
	return rs;
}

int16_t filter2(int16_t s) {
	int16_t rs;
	static int16_t prs;
	static int16_t ps;

	if(!wav_filter2_started) {
		rs = s;
		prs = rs;
	} else {
		rs = s*1/12 + prs*11/12;
		prs = rs;
	}
	wav_filter2_started = true;
	return rs;
}

// Stepping average over the last 16 values
#define NUM_READS_SH 4
#define NUM_READS (1u << NUM_READS_SH)

int32_t filter_avg(int32_t s) {
	static int32_t values[NUM_READS];
	wav_avg_reads++;
	if(wav_avg_reads > NUM_READS)
		wav_avg_sum -= values[wav_avg_offset];
	wav_avg_sum += s;
	values[wav_avg_offset] = s;
	wav_avg_offset = (wav_avg_offset + 1) & (NUM_READS-1);
	if(wav_avg_reads <= NUM_READS)
		return wav_avg_sum / wav_avg_reads;
	return wav_avg_sum >> NUM_READS_SH;
}

void init_wav() {
	wav_avg_reads = 0;
	wav_avg_offset = 0;
	wav_avg_sum = 0;
	wav_prev_sample = 0;
	wav_filter1_started = false;
	wav_filter2_started = false;
	wav_last_silence = 0;
	wav_last_count = 0;
	wav_last_duration = 0;

	wav_sample_size = (wav_header.bits_per_sample == 16) ? 2 : 1;
	wav_sample_div = (wav_header.sample_rate > 48000) ? 2 : 1;
	wav_silence_threshold = wav_header.sample_rate / (wav_sample_div*20); // 32
	float coeff1 = 2.0*cosf(2.0*M_PI*(3995.0*wav_sample_div/(float)wav_header.sample_rate));
	float coeff2 = 2.0*cosf(2.0*M_PI*(5327.0*wav_sample_div/(float)wav_header.sample_rate));
	zcoeff1 = coeff1*(1<<14);
	zcoeff2 = coeff2*(1<<14);

	wav_filter_window_size = cas_block_turbo ? 0 : (wav_header.sample_rate < 44100 ? 12 : 20*wav_sample_div);
	if(cas_block_turbo)
		wav_scaled_sample_rate = wav_header.sample_rate / wav_sample_div;
	else
		// The ideal sampling frequency for PAL is 31668(.7), for NTSC 31960(.54)
		wav_scaled_sample_rate = wav_header.sample_rate < 44100 ? wav_header.sample_rate : (current_options[clock_option_index] ? 31960 : 31668);
	cas_sample_duration = (timing_base_clock+wav_scaled_sample_rate/2)/wav_scaled_sample_rate;
	pwm_bit = cas_block_turbo ? 0 : 1;
	cas_fsk_bit = pwm_bit;
	wav_last_duration_bit = cas_fsk_bit;
}
