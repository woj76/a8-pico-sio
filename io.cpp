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

 #include "config.h"

#include "hardware/clocks.h"
#include "hardware/pio.h"
#include "hardware/dma.h"
#include "hardware/irq.h"
#include "hardware/uart.h"
#include "pico/util/queue.h"

#include "io.hpp"

#include "options.hpp"
#include "led_indicator.hpp"
#include "pin_io.pio.h"

// const uint led_pin = 25;

const uint sio_tx_pin = 4;
const uint sio_rx_pin = 5;
const uint normal_motor_pin = 10;
const uint command_line_pin = 11;
const uint proceed_pin = 21;
const uint interrupt_pin = 22;
const uint joy2_p1_pin = 26;
const uint joy2_p2_pin = 27;
const uint joy2_p4_pin = 28;

// Conventional SIO, but also Turbo D & Turbo 6000
const uint32_t normal_motor_pin_mask = (1u << normal_motor_pin);
const uint32_t normal_motor_value_on = (MOTOR_ON_STATE << normal_motor_pin);

// Turbo 2000 KSO - Joy 2 port
const uint32_t kso_motor_pin_mask = (1u << joy2_p2_pin);
const uint32_t kso_motor_value_on = (0u << joy2_p2_pin);

// Turbo 2001 / 2000F and sorts over SIO data
const uint32_t comm_motor_pin_mask = (1u << command_line_pin) | (1u << normal_motor_pin);
const uint32_t comm_motor_value_on = (0u << command_line_pin) | (MOTOR_ON_STATE << normal_motor_pin);

// Turbo Blizzard - SIO Data Out
const uint32_t sio_motor_pin_mask = (1u << sio_rx_pin) | (1u << normal_motor_pin);
const uint32_t sio_motor_value_on = (0u << sio_rx_pin) | (MOTOR_ON_STATE << normal_motor_pin);

const uint opt_to_turbo_data_pin[] = {sio_tx_pin, joy2_p4_pin, proceed_pin, interrupt_pin, joy2_p1_pin};
const uint32_t opt_to_turbo_motor_pin_mask[] = {comm_motor_pin_mask, kso_motor_pin_mask, sio_motor_pin_mask, normal_motor_pin_mask};
const uint32_t opt_to_turbo_motor_pin_val[] = {comm_motor_value_on, kso_motor_value_on, sio_motor_value_on, normal_motor_value_on};

const uint hsio_opt_to_baud_ntsc[] = {19040, 38908, 68838, 74575, 81354, 89490, 99433, 111862, 127842};
const uint hsio_opt_to_baud_pal[] = {18866, 38553, 68210, 73894, 80611, 88672, 98525, 110840, 126675};
// PAL/NTSC average
// const uint hsio_opt_to_baud[] = {18953, 38731, 68524, 74234, 80983, 89081, 98979, 111351, 127258};


uint32_t turbo_motor_pin_mask;
uint32_t turbo_motor_value_on;
uint turbo_data_pin;

volatile bool cas_block_turbo;
volatile bool dma_block_turbo;
uint sm;
uint sm_turbo;

// Turbo 2000 KSO
// const uint turbo_data_pin = joy2_p4_pin; // Joy 2 port, pin 4
// Turbo D
//const uint turbo_data_pin = joy2_p1_pin; // Joy 2 port, pin 1
// Turbo 2001 / 2000F SIO / Turbo Blizzard
//const uint turbo_data_pin = sio_tx_pin;
// Turbo 6000, this needs pwm_bit invertion
// const uint turbo_data_pin = proceed_pin;
// Rambit, also needs bit inversion
//const uint turbo_data_pin = interrupt_pin;

uint32_t timing_base_clock;
uint32_t max_clock_ms;

pio_sm_config sm_config_turbo;
uint pio_offset;

int8_t turbo_conf[] = {-1, -1, -1};

queue_t pio_queue;
// 10*8 is not enough for Turbo D 9000, but going wild here costs memory, each item is 4 bytes
// 16*8 also fails sometimes with the 1MHz base clock
// The WAV decoding now seems to work with 64, but increasing it might be a good idea if some WAV file is not working
// WAV files with 96000 sample rate also prefer this to be more than 64
const int pio_queue_size = 96*8;

uint32_t pio_e;

void reinit_pio() {
	if(turbo_conf[0] != current_options[turbo1_option_index]) {
		if(turbo_conf[0] >= 0) {
			pio_sm_set_enabled(cas_pio, sm_turbo, false);
			//gpio_set_function(turbo_data_pin, turbo_data_pin == sio_rx_pin ? GPIO_FUNC_UART : GPIO_FUNC_NULL);
			gpio_set_function(turbo_data_pin, GPIO_FUNC_NULL);
		}
		turbo_conf[0] = current_options[turbo1_option_index];
		turbo_data_pin = opt_to_turbo_data_pin[turbo_conf[0]];
		pio_gpio_init(cas_pio, turbo_data_pin);
		sm_config_set_out_pins(&sm_config_turbo, turbo_data_pin, 1);
		pio_sm_set_consecutive_pindirs(cas_pio, sm_turbo, turbo_data_pin, 1, true);
		pio_sm_init(cas_pio, sm_turbo, pio_offset, &sm_config_turbo);
		pio_sm_set_enabled(cas_pio, sm_turbo, true);
	}
	turbo_conf[1] = current_options[turbo2_option_index];
	turbo_motor_pin_mask = opt_to_turbo_motor_pin_mask[turbo_conf[1]];
	turbo_motor_value_on = opt_to_turbo_motor_pin_val[turbo_conf[1]];
	turbo_conf[2] = current_options[turbo3_option_index];
	//while(!queue_is_empty(&pio_queue))
	//		tight_loop_contents();
	pio_sm_restart(cas_pio, sm);
	pio_sm_restart(cas_pio, sm_turbo);
}

volatile bool dma_going = false;
int dma_channel, dma_channel_turbo;


static void dma_handler() {
	int dc = dma_block_turbo ? dma_channel_turbo : dma_channel;
	if(dma_going)
		dma_hw->ints1 = 1u << dc;
	else
		dma_going = true;
	if(queue_try_remove(&pio_queue, &pio_e))
		dma_channel_start(dc);
	else
		dma_going = false;
}

bool cas_motor_on() {
	return (cas_block_turbo ? turbo_motor_value_on : normal_motor_value_on) ==
		(gpio_get_all() & (cas_block_turbo ? turbo_motor_pin_mask : normal_motor_pin_mask));
}

void flush_pio() {
	wav_sample_size = 0;
	pio_sm_set_enabled(cas_pio, cas_block_turbo ? sm_turbo : sm, true);
	blue_blinks = 0;
	green_blinks = 0;
	update_rgb_led(false);
	//while(!queue_is_empty(&pio_queue))
	//		tight_loop_contents();
	//pio_interrupt_clear(cas_pio, 7);
}

static bool repeating_timer_motor(struct repeating_timer *t) {
#if MOTOR_OFF_DELAY > 0
	static uint motor_off_delay = MOTOR_OFF_DELAY;
#endif
#if MOTOR_ON_DELAY > 0
	static uint motor_on_delay = MOTOR_ON_DELAY;
#endif
	if(wav_sample_size) {
		if(!cas_motor_on()) {
#if MOTOR_ON_DELAY > 0
			motor_on_delay = MOTOR_ON_DELAY;
#endif
#if MOTOR_OFF_DELAY > 0
			if(motor_off_delay)
				motor_off_delay--;
			else
#endif
			{
				//pio_sm_exec(cas_pio, cas_block_turbo ? sm_turbo : sm, pio_encode_irq_set(false, 7));
				////sleep_ms(1);
				//pio_sm_exec(cas_pio, cas_block_turbo ? sm_turbo : sm, pio_encode_wait_irq(0, false, 7));
				pio_sm_set_enabled(cas_pio, cas_block_turbo ? sm_turbo : sm, false);
				blue_blinks = 0;
				green_blinks = 0;
				update_rgb_led(false);
			}
		} else {
#if MOTOR_OFF_DELAY > 0
			motor_off_delay = MOTOR_OFF_DELAY;
#endif
#if MOTOR_ON_DELAY > 0
			if(motor_on_delay)
				motor_on_delay--;
			else
#endif
			{
				//pio_interrupt_clear(cas_pio, 7);
				pio_sm_set_enabled(cas_pio, cas_block_turbo ? sm_turbo : sm, true);
				if(cas_block_turbo)
					blue_blinks = -1;
				green_blinks = -1;
				update_rgb_led(false);
			}

		}
	}
	return true;
}

void pio_enqueue(uint8_t b, uint32_t d) {
	uint32_t e = (b^(cas_block_turbo ? turbo_conf[2] : 0) | ((d - pio_prog_cycle_corr) << 1));
//	queue_try_add(&pio_queue, &e);
//	absolute_time_t t = make_timeout_time_ms(1250);
//	while (!gpio_get(command_line_pin) && absolute_time_diff_us(get_absolute_time(), t) > 0)
//		tight_loop_contents();
	queue_add_blocking(&pio_queue, &e);
	if(!dma_going) {
		dma_block_turbo = cas_block_turbo;
		dma_handler();
	}
}

void init_io() {

#ifdef FULL_SPEED_PIO
	timing_base_clock = clock_get_hz(clk_sys);
#else
	timing_base_clock = 1000000;
#endif
	// How much "silence" can the PIO produce in one step:
	max_clock_ms = 0x7FFFFFFF/(timing_base_clock/1000)/1000*1000;

	gpio_init(joy2_p2_pin); gpio_set_dir(joy2_p2_pin, GPIO_IN); gpio_pull_up(joy2_p2_pin);

	gpio_init(normal_motor_pin); gpio_set_dir(normal_motor_pin, GPIO_IN);

	// Pico2 bug! This pull down will lock the pin state high after the first read, instead
	// one has to use an external pull down resistor of not too high value, for example 4.7kohm
	// (reports sugguest that it has to be below 9kohm)
#ifndef RASPBERRYPI_PICO2
#if MOTOR_ON_STATE == 1
	gpio_pull_down(normal_motor_pin);
#endif
#endif
	gpio_init(command_line_pin); gpio_set_dir(command_line_pin, GPIO_IN); gpio_pull_up(command_line_pin);

	queue_init(&pio_queue, sizeof(uint32_t), pio_queue_size);

	pio_offset = pio_add_program(cas_pio, &pin_io_program);
 	int clk_divider = clock_get_hz(clk_sys)/timing_base_clock;

	sm = pio_claim_unused_sm(cas_pio, true);
	pio_gpio_init(cas_pio, sio_tx_pin);
	pio_sm_set_consecutive_pindirs(cas_pio, sm, sio_tx_pin, 1, true);
	pio_sm_config c = pin_io_program_get_default_config(pio_offset);
	sm_config_set_fifo_join(&c, PIO_FIFO_JOIN_TX);
	sm_config_set_clkdiv_int_frac(&c, clk_divider, 0);

	sm_config_set_out_pins(&c, sio_tx_pin, 1);
	sm_config_set_out_shift(&c, true, true, 32);
	pio_sm_init(cas_pio, sm, pio_offset, &c);
	pio_sm_set_enabled(cas_pio, sm, true);

	sm_turbo = pio_claim_unused_sm(cas_pio, true);

	sm_config_turbo = pin_io_program_get_default_config(pio_offset);
	sm_config_set_fifo_join(&sm_config_turbo, PIO_FIFO_JOIN_TX);

	sm_config_set_clkdiv_int_frac(&sm_config_turbo, clk_divider, 0);

	sm_config_set_out_shift(&sm_config_turbo, true, true, 32);

	reinit_pio();

	dma_channel = dma_claim_unused_channel(true);
	dma_channel_config dma_c = dma_channel_get_default_config(dma_channel);
	channel_config_set_transfer_data_size(&dma_c, DMA_SIZE_32);
	channel_config_set_read_increment(&dma_c, false);
	channel_config_set_dreq(&dma_c, pio_get_dreq(cas_pio, sm, true));
	dma_channel_configure(dma_channel, &dma_c, &cas_pio->txf[sm], &pio_e, 1, false);
	dma_channel_set_irq1_enabled(dma_channel, true);

	dma_channel_turbo = dma_claim_unused_channel(true);
	dma_channel_config dma_c1 = dma_channel_get_default_config(dma_channel_turbo);
	channel_config_set_transfer_data_size(&dma_c1, DMA_SIZE_32);
	channel_config_set_read_increment(&dma_c1, false);
	channel_config_set_dreq(&dma_c1, pio_get_dreq(cas_pio, sm_turbo, true));
	dma_channel_configure(dma_channel_turbo, &dma_c1, &cas_pio->txf[sm_turbo], &pio_e, 1, false);
	dma_channel_set_irq1_enabled(dma_channel_turbo, true);

	irq_set_exclusive_handler(DMA_IRQ_1, dma_handler);

	irq_set_enabled(DMA_IRQ_1, true);

	uart_init(uart1, current_options[clock_option_index] ? hsio_opt_to_baud_ntsc[0] : hsio_opt_to_baud_pal[0]);
	gpio_set_function(sio_tx_pin, GPIO_FUNC_UART);
	gpio_set_function(sio_rx_pin, GPIO_FUNC_UART);
	uart_set_hw_flow(uart1, false, false);
	uart_set_fifo_enabled(uart1, false);
	uart_set_format(uart1, 8, 1, UART_PARITY_NONE);

	static struct repeating_timer tmr;
	add_repeating_timer_ms(MOTOR_CHECK_INTERVAL_MS, repeating_timer_motor, NULL, &tmr);
}
