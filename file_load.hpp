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

#include <ctype.h>
#include <stdint.h>
#include "config.h"
#include "ff.h"

#define MAX_PATH_LEN 512

extern char curr_path[];
extern size_t num_files;
extern size_t num_files_page;
extern DIR dir;

extern char temp_array[];
extern volatile int16_t create_new_file;

typedef struct  __attribute__((__packed__)) {
	bool dir;
	char short_name[15];
	char long_name[258];
	uint16_t dir_index;
	bool last_file;
} file_entry_type;

#define max_files_per_page 12

extern file_entry_type file_entries[];

extern char last_file_name[];

size_t get_filename_ext(char *filename);

int32_t read_directory(int32_t page_index, int page_size);
int16_t create_new_disk_image();
