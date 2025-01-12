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

#include "pico/multicore.h"
#include "hardware/sync.h"
#include "hardware/flash.h"
#include "string.h"

#include "options.hpp"

#include "file_load.hpp"
#include "mounts.hpp"
#include "flash_fs.h"

uint8_t current_options[option_count] = {
	0, // disk write off
	0, // PAL
	0, // HSIO off
	0, // 1050
	2, // loader at $700
	0, // SIO
	0, // Command
	0, // no invert
	0 // FSK
};

volatile bool save_config_flag = false;
volatile bool save_path_flag = false;

#define flash_save_offset (HW_FLASH_STORAGE_BASE-FLASH_SECTOR_SIZE)

const uint8_t *flash_config_pointer = (uint8_t *)(XIP_BASE+flash_save_offset);
#define flash_config_offset MAX_PATH_LEN
#define flash_check_sig_offset (flash_config_offset+64)
#define config_magic 0xDEADBEEF

void check_and_load_config(bool reset_config) {
	if(!reset_config && *(uint32_t *)&flash_config_pointer[flash_check_sig_offset] == config_magic) {
		memcpy(curr_path, &flash_config_pointer[0], MAX_PATH_LEN);
		memcpy(current_options, &flash_config_pointer[flash_config_offset], option_count);
	} else {
		save_path_flag = true;
		save_config_flag = true;
	}
}

void check_and_save_config() {
	if(!save_path_flag && !save_config_flag)
		return;
	memset(sector_buffer, 0, sector_buffer_size);
	memcpy(sector_buffer, save_path_flag ? (uint8_t *)curr_path : &flash_config_pointer[0], MAX_PATH_LEN);
	memcpy(&sector_buffer[flash_config_offset], save_config_flag ? current_options : (uint8_t *)&flash_config_pointer[flash_config_offset], option_count);
	*(uint32_t *)&sector_buffer[flash_check_sig_offset] = config_magic;
	uint32_t ints = save_and_disable_interrupts();
	multicore_lockout_start_blocking();
	flash_range_erase(flash_save_offset, FLASH_SECTOR_SIZE);
	flash_range_program(flash_save_offset, (uint8_t *)sector_buffer, sector_buffer_size);
	multicore_lockout_end_blocking();
	restore_interrupts(ints);
	save_path_flag = false;
	save_config_flag = false;
}
