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

#include "pico/time.h"
#include "hardware/sync.h"
#include "libraries/pico_display_2/pico_display_2.hpp"
#include "drivers/rgbled/rgbled.hpp"

#include "led_indicator.hpp"

volatile int8_t red_blinks = 0;
volatile int8_t green_blinks = 0;
volatile int8_t blue_blinks = 0;

pimoroni::RGBLED led(pimoroni::PicoDisplay2::LED_R, pimoroni::PicoDisplay2::LED_G, pimoroni::PicoDisplay2::LED_B, pimoroni::Polarity::ACTIVE_LOW, 64);

void update_rgb_led(bool from_interrupt) {
	uint32_t ints;
	if(!from_interrupt)
		ints = save_and_disable_interrupts();
	uint8_t r = (red_blinks & 1) ? 128 : 0;
	uint8_t g = (green_blinks & 1) ? 128 : 0;
	uint8_t b = (blue_blinks & 1) ? 128 : 0;
	led.set_rgb(r, g, b);
	if(!from_interrupt)
		restore_interrupts(ints);
}

static bool repeating_timer_led(struct repeating_timer *t) {
	if(red_blinks > 0) red_blinks--;
	if(green_blinks > 0) green_blinks--;
	if(blue_blinks > 0) blue_blinks--;
	update_rgb_led(true);
	return true;
}

void init_rgb_led() {
	update_rgb_led(true);
	static struct repeating_timer tmr;
	add_repeating_timer_ms(250, repeating_timer_led, NULL, &tmr);
}
