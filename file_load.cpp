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

#include <string.h>

#include "file_load.hpp"
#include "ff.h"
#include "mounts.hpp"

char curr_path[MAX_PATH_LEN];
size_t num_files, num_files_page;
DIR dir = {0};

char temp_array[MAX_PATH_LEN];
volatile int16_t create_new_file = 0;

file_entry_type file_entries[max_files_per_page+2];
char last_file_name[15] = {0};

size_t get_filename_ext(char *filename) {
	size_t l = strlen(filename);
	size_t i = l;
	while(i > 0 && filename[--i] != '.');
	return (i == 0 || l-i > 4) ? l : i+1;
}

static int file_entry_cmp(int i, int j) {
	file_entry_type e1 = file_entries[i];
	file_entry_type e2 = file_entries[j];
	if (e1.dir && !e2.dir) return -1;
	else if (!e1.dir && e2.dir) return 1;
	// Short names are a bit worse to use for comparison, but with the current
	// setup they are the only ones guaranteed not to have duplicates in the list
	else return strcasecmp(e1.short_name, e2.short_name);
}

static bool is_valid_file(char *filename) {
	size_t i = get_filename_ext(filename);
	switch(ft) {
		case file_type::casette:
			return !strcasecmp(&filename[i], "CAS") || !strcasecmp(&filename[i], "WAV");
		case file_type::disk:
			return !strcasecmp(&filename[i], "ATR") || !strcasecmp(&filename[i], "ATX") || !strcasecmp(&filename[i], "XEX") || !strcasecmp(&filename[i], "COM") || !strcasecmp(&filename[i], "EXE");
		default:
			return false;
	}
}

static void mark_directory(int i) {
	int l = strlen(file_entries[i].long_name);
	file_entries[i].long_name[l] = '/';
	file_entries[i].long_name[l+1] = 0;
	l = strlen(file_entries[i].short_name);
	file_entries[i].short_name[l] = '/';
	file_entries[i].short_name[l+1] = 0;
}

uint16_t next_page_references[5464]; // This is 65536 / 12 (max_files_per_page) with a tiny slack

/**
  Read (the portion of) the directory, page_index is to control what is the current alphabetically smallest
  file name on the current page, page_size specifies how many files are supposed to appear on the current display page,
  use -1 to only pre-count files in the directory. Returns either the total number of files (when page_size is -1)
  or the total number of files for the next display (it should be assertable that result == page_size in this case).
*/
int32_t read_directory(int32_t page_index, int page_size) {
	FILINFO fno;
	int32_t ret = -1;

	if(!curr_path[0]) {
		int i;
		for(i=0; i < sd_card_present+1; i++) {
			strcpy(file_entries[i].short_name, volume_names[i]);
			strcpy(file_entries[i].long_name, volume_names[i]);
			file_entries[i].dir = true;
			file_entries[i].dir_index = i;
			file_entries[i].last_file = false;
			if(last_file_name[0] && !strcmp(file_entries[i].short_name, last_file_name))
				file_entries[i].last_file = true;
		}
		return i;
	}

	mutex_enter_blocking(&fs_lock);
	//f (f_mount(&fatfs[0], curr_path, 1) == FR_OK) {
		if (f_rewinddir(&dir) == FR_OK) {
		//if (f_opendir(&dir, curr_path) == FR_OK) {
			ret = 0;
			uint16_t dir_index = -1;
			int32_t look_for_index = page_index >= 0 ? next_page_references[page_index] : -1;
			while (true) {
				dir_index++;
				if (f_readdir(&dir, &fno) != FR_OK || fno.fname[0] == 0) break;
				if (fno.fattrib & (AM_HID | AM_SYS)) continue;
				bool is_dir = (fno.fattrib & AM_DIR);
				if (!is_dir && !is_valid_file(fno.fname)) continue;
				// I did not document in time this alphabetical file sorting in display page increments
				// logic, and now I forgot how it was working... :/, but the general idea is to get the
				// next n files that are just after the last portion of files displayed previously
				// and remember the new smallest file for the next page. For previous pages, the data is
				// cached in the next_page_references array.
				if(page_index < 0) {
					file_entries[page_size].dir = is_dir;
					file_entries[page_size].dir_index = dir_index;
					strcpy(file_entries[page_size].long_name, fno.fname);
					strcpy(file_entries[page_size].short_name, fno.altname[0] ? fno.altname : fno.fname);
					if(is_dir)
						mark_directory(page_size);
					if(!ret || file_entry_cmp(page_size, page_size+1) < 0)
						file_entries[page_size+1] = file_entries[page_size];
					ret++;
				} else {
					if(look_for_index >= 0 && dir_index != look_for_index)
						continue;
					file_entries[ret].dir = is_dir;
					file_entries[ret].dir_index = dir_index;
					strcpy(file_entries[ret].long_name, fno.fname);
					strcpy(file_entries[ret].short_name, fno.altname[0] ? fno.altname : fno.fname);
					file_entries[ret].last_file = false;
					if(is_dir) mark_directory(ret);
					if(last_file_name[0] && !strcmp(file_entries[ret].short_name, last_file_name))
						file_entries[ret].last_file = true;
					if(!ret) {
						ret++;
						look_for_index = -1;
						dir_index = -1;
						f_rewinddir(&dir);
						continue;
					}
					if(file_entry_cmp(ret, 0) <= 0)
						continue;
					int32_t ri = ret;
					while(ri > 0 && file_entry_cmp(ri, ri-1) < 0) {
						file_entry_type fet = file_entries[ri];
						file_entries[ri] = file_entries[ri-1];
						file_entries[ri-1] = fet;
						ri--;
					}
					if(ret < page_size) {
						ret++;
					}else if(look_for_index == -1 || file_entry_cmp(page_size, page_size+1) <= 0) {
						look_for_index = -2;
						file_entries[page_size+1] = file_entries[page_size];
					}
				}
				next_page_references[page_index+1] = file_entries[page_size+1].dir_index;
			}
			// f_closedir(&dir);
		}
		//f_mount(0, curr_path, 1);
	//}
	mutex_exit(&fs_lock);
	// sleep_ms(3000); // For testing to emulate long directory reading
	return ret;
}

#include "disk_images_data.h"

int16_t create_new_disk_image() {
	FIL fil;
	FRESULT f_op_stat;

	uint8_t new_file_size_index = (create_new_file & 0xF)-1; // SD ED DD QD
	uint8_t new_file_format_index = ((create_new_file >> 4 ) & 0xF)-1; // None DOS MyDOS Sparta
	uint32_t image_size = image_sizes[new_file_size_index];
	bool new_file_boot = ((create_new_file >> 8 ) & 0x1);
	f_op_stat = FR_OK;
	uint disk_image_size = disk_images_size[new_file_size_index][new_file_format_index];
	memcpy(sector_buffer, (uint8_t *)(disk_images_data[new_file_size_index][new_file_format_index]), disk_image_size);
	if(new_file_boot)
		memcpy(&sector_buffer[256], dummy_boot, dummy_boot_len);
	uint32_t bs;
	uint32_t ints;
	uint vol_num = temp_array[0]-'0';
	if(!vol_num) {
		ints = save_and_disable_interrupts();
		multicore_lockout_start_blocking();
	}
	do {
		//if((f_op_stat = f_mount(&fatfs[0], temp_array, 1)) != FR_OK)
		//	break;
#if FF_MAX_SS != FF_MIN_SS
		bs = fatfs[vol_num].csize * fatfs[vol_num].ssize;
#else
		bs = fatfs[vol_num].csize * FF_MAX_SS;
#endif
		image_size = (image_size + bs - 1) / bs;
		FATFS *ff;
		if((f_op_stat = f_getfree(temp_array, &bs, &ff)) != FR_OK)
			break;
		if(bs < image_size) {
			f_op_stat = FR_INT_ERR;
			break;
		}
		if((f_op_stat = f_open(&fil, temp_array, FA_CREATE_NEW | FA_WRITE)) != FR_OK)
			break;
		uint ind=0;
		while(ind < disk_image_size) {
			memcpy(&bs, &sector_buffer[ind], 4);
			ind += 4;
			bool nrb = (bs & 0x80000000) ? true : false;
			bs &= 0x7FFFFFFF;
			UINT wrt;
			while(bs > 0) {
//				if(f_putc(sector_buffer[ind], &fil) == -1) {
				if(f_write(&fil, &sector_buffer[ind], 1, &wrt) != FR_OK || wrt != 1) {
					f_op_stat = FR_INT_ERR;
					break;
				}
				if(nrb) ind++;
				bs--;
			}
			if(f_op_stat != FR_OK)
				break;
			if(!nrb) ind++;
		}
		if(f_op_stat != FR_OK || !new_file_boot)
			break;
		if((f_op_stat = f_lseek(&fil, 16)) != FR_OK)
			break;
		f_op_stat = f_write(&fil, &sector_buffer[256], dummy_boot_len, &ind);
	} while(false);
	f_close(&fil);
	//f_mount(0, temp_array, 1);
	if(!vol_num) {
		multicore_lockout_end_blocking();
		restore_interrupts(ints);
	}
	return (f_op_stat != FR_OK) ? -1 : 0;
	/*
	if(f_op_stat != FR_OK) {
		create_new_file = -1;
		// Access error was not Atari drive specific
		last_access_error_drive = 5;
	}else
		create_new_file = 0;
	*/

}
