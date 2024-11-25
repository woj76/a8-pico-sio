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

#include <string.h>
#include <stdio.h>

#include "pico/multicore.h"
#include "pico/time.h"
#include "hardware/sync.h"

#include "mounts.hpp"
#include "led_indicator.hpp"
#include "file_load.hpp"
#include "io.hpp"

char d1_mount[MAX_PATH_LEN] = {0};
char d2_mount[MAX_PATH_LEN] = {0};
char d3_mount[MAX_PATH_LEN] = {0};
char d4_mount[MAX_PATH_LEN] = {0};
char c_mount[MAX_PATH_LEN] = {0};

char str_d1[] = "D1:  <EMPTY>   ";
char str_d2[] = "D2:  <EMPTY>   ";
char str_d3[] = "D3:  <EMPTY>   ";
char str_d4[] = "D4:  <EMPTY>   ";
char str_cas[] = "C:  <EMPTY>   ";

mounts_type mounts[] = {
	{.str=str_cas, .mount_path=c_mount, .mounted=false, .status = 0},
	{.str=str_d1, .mount_path=d1_mount, .mounted=false, .status = 0},
	{.str=str_d2, .mount_path=d2_mount, .mounted=false, .status = 0},
	{.str=str_d3, .mount_path=d3_mount, .mounted=false, .status = 0},
	{.str=str_d4, .mount_path=d4_mount, .mounted=false, .status = 0}
};

disk_header_type disk_headers[4];

uint8_t sector_buffer[sector_buffer_size];

file_type ft = file_type::none;
cas_header_type cas_header;

uint8_t pwm_bit_order;
uint8_t pwm_bit;

uint32_t pwm_sample_duration; // in cycles dep the base timing value
uint32_t cas_sample_duration; // in cycles dep the base timing value
uint16_t silence_duration; // in ms
uint16_t cas_block_index;
uint16_t cas_block_multiple;
uint8_t cas_fsk_bit;

volatile FSIZE_t cas_size;

FATFS fatfs[2];

mutex_t fs_lock, mount_lock;

void init_locks() {
	mutex_init(&fs_lock);
	mutex_init(&mount_lock);
}

void mount_file(char *f, int drive_number, char *lfn) {
	int j;
	bool read_only = false;

	if(!drive_number)
		flush_pio();

	mutex_enter_blocking(&mount_lock);

	if(mounts[drive_number].mounted)
		f_close(&mounts[drive_number].fil);
	if(drive_number) {
		for(j=1; j<=4; j++) {
			if(j == drive_number)
				continue;
			if(!strcmp(mounts[j].mount_path, curr_path)) {
				if(!mounts[j].mounted) {
					mounts[j].mount_path[0] = 0;
					strcpy(&mounts[j].str[3], "  <EMPTY>   ");
				} else if(!(disk_headers[j-1].atr_header.flags & 0x01))
					read_only = true;
			}
		}
	}
	if(read_only)
		disk_headers[drive_number-1].atr_header.flags |= 0x01;
	mounts[drive_number].mounted = true;
	mounts[drive_number].status = 0;
	strcpy((char *)mounts[drive_number].mount_path, curr_path);
	j = 0;
	int si = (ft == file_type::disk) ? 3 : 2;
	size_t size_lfn = strlen(lfn)-4;
	strcpy(&mounts[drive_number].str[si+8], &lfn[size_lfn]);
	while(j<8) {
		mounts[drive_number].str[si+j] = (j < size_lfn ? lfn[j] : ' ');
		j++;
	}
	if(size_lfn > 8)
		mounts[drive_number].str[si+7] = '~';

	mutex_exit(&mount_lock);
}


FRESULT mounted_file_transfer(int drive_number, FSIZE_t offset, FSIZE_t to_transfer, bool op_write, size_t t_offset, FSIZE_t brpt) {
	FIL* fil = &mounts[drive_number].fil;
	FRESULT f_op_stat;
	uint bytes_transferred;
	uint8_t *data = &sector_buffer[t_offset];

	mutex_enter_blocking(&fs_lock);
	do {
		//if((f_op_stat = f_mount(&fatfs[0], (const char *)mounts[drive_number].mount_path, 1)) != FR_OK)
		//	break;
//		if((f_op_stat = f_open(fil, (const char *)mounts[drive_number].mount_path, op_write ? FA_WRITE : FA_READ)) != FR_OK)
//			break;
		if((f_op_stat = f_lseek(fil, offset)) != FR_OK)
			break;
		uint vol_num = mounts[drive_number].mount_path[0] - '0';
		if(op_write) {
			uint32_t ints;
			if(!vol_num) {
				ints = save_and_disable_interrupts();
				multicore_lockout_start_blocking();
			}
			for(uint i=0; i<brpt; i++)
				f_op_stat = f_write(fil, data, to_transfer, &bytes_transferred);
			f_op_stat = f_sync(fil);
			if(!vol_num){
				multicore_lockout_end_blocking();
				restore_interrupts(ints);
			}
		} else
			f_op_stat = f_read(fil, data, to_transfer, &bytes_transferred);
		if(f_op_stat != FR_OK)
			break;
		if(bytes_transferred != to_transfer)
			f_op_stat = FR_INT_ERR;
	}while(false);
//	f_close(fil);
	//f_mount(0, (const char *)mounts[drive_number].mount_path, 1);
	mutex_exit(&fs_lock);
	return f_op_stat;
}

FSIZE_t cas_read_forward(FSIZE_t offset) {
	FIL* fil = &mounts[0].fil;
	uint bytes_read;
	if(f_lseek(fil, offset) != FR_OK) {
		offset = 0;
		goto cas_read_forward_exit;
	}
	while(true) {
		if(f_read(fil, &cas_header, sizeof(cas_header_type), &bytes_read) != FR_OK || bytes_read != sizeof(cas_header_type)) {
			offset = 0;
			goto cas_read_forward_exit;
		}
		offset += bytes_read;
		cas_block_index = 0;
		switch(cas_header.signature) {
			case cas_header_FUJI:
				offset += cas_header.chunk_length;
				if(f_lseek(fil, offset) != FR_OK) {
					offset = 0;
					goto cas_read_forward_exit;
				}
				break;
			case cas_header_baud:
				if(cas_header.chunk_length) {
					offset = 0;
					goto cas_read_forward_exit;
				}
				cas_sample_duration = (timing_base_clock+cas_header.aux.aux_w/2)/cas_header.aux.aux_w;
				break;
			case cas_header_data:
				cas_block_turbo = false;
				silence_duration = cas_header.aux.aux_w;
				cas_block_multiple = 1;
				goto cas_read_forward_exit;
			case cas_header_fsk:
				cas_block_turbo = false;
				silence_duration = cas_header.aux.aux_w;
				cas_block_multiple = 2;
				cas_fsk_bit = 0;
				goto cas_read_forward_exit;
			case cas_header_pwms:
				if(cas_header.chunk_length != 2) {
					offset = 0;
					goto cas_read_forward_exit;
				}
				pwm_bit_order = (cas_header.aux.aux_b[0] >> 2) & 0x1;
				cas_header.aux.aux_b[0] &= 0x3;
				if(cas_header.aux.aux_b[0] == 0b01)
					pwm_bit = 0; // 0
				else if(cas_header.aux.aux_b[0] == 0b10)
					pwm_bit = 1; // 1
				else {
					offset = 0;
					goto cas_read_forward_exit;
				}
				pwm_sample_duration = 0;
				if(f_read(fil, &pwm_sample_duration, sizeof(uint16_t), &bytes_read) != FR_OK || bytes_read != sizeof(uint16_t)) {
					offset = 0;
					goto cas_read_forward_exit;
				}
				offset += bytes_read;
				pwm_sample_duration = (timing_base_clock+pwm_sample_duration/2)/pwm_sample_duration;
				break;
			case cas_header_pwmc:
				cas_block_turbo = true;
				silence_duration = cas_header.aux.aux_w;
				cas_block_multiple = 3;
				goto cas_read_forward_exit;
			case cas_header_pwmd:
				cas_block_turbo = true;
				cas_block_multiple = 1;
				goto cas_read_forward_exit;
			case cas_header_pwml:
				cas_block_turbo = true;
				silence_duration = cas_header.aux.aux_w;
				cas_block_multiple = 2;
				cas_fsk_bit = pwm_bit;
				goto cas_read_forward_exit;
			default:
				break;
		}
	}
cas_read_forward_exit:
	return offset;
}

volatile uint8_t sd_card_present = 0;
const char * const volume_names[] = {"0:", "1:"};
const char * const str_int_flash = "Pico FLASH";
const char * const str_sd_card = "SD/MMC Card";
char volume_labels[2][17] = {"Int:            ", "Ext:            "};

void get_drive_label(int i) {
	DWORD sn;
	if(f_getlabel(volume_names[i], &volume_labels[i][5], &sn) == FR_OK) {
		if(!volume_labels[i][5])
			sprintf(&volume_labels[i][5], "%04X-%04X", (sn >> 16) & 0xFFFF, sn & 0xFFFF);
		else
			for(int j=0; j<strlen(volume_labels[j]); j++)
				if(volume_labels[i][j] >= 0x80) volume_labels[i][j] = '?';
	} else
		strcpy(&volume_labels[i][5], i ? str_sd_card : str_int_flash);
}

uint8_t try_mount_sd() {
	if(f_mount(&fatfs[1], volume_names[1], 1) == FR_OK) {
		get_drive_label(1);
		green_blinks = 4;
		sd_card_present = 1;
	}else
	sd_card_present = 0;
	return sd_card_present;
}

volatile int last_drive = -1;
volatile uint32_t last_drive_access = 0;

void update_last_drive(uint drive_number) {
	last_drive = drive_number;
	last_drive_access = to_ms_since_boot(get_absolute_time());
}

int last_access_error_drive = -1;
bool last_access_error[5] = {false};

void set_last_access_error(int drive_number) {
	last_access_error_drive = drive_number;
	last_access_error[last_access_error_drive] = true;
}
