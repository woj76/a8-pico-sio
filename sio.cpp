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

#include <algorithm>
#include <string.h>
#include "hardware/pio.h"
#include "hardware/pio_instructions.h"
#include "hardware/uart.h"

#include "sio.hpp"

#include "sd_card.h"
#include "options.hpp"
#include "led_indicator.hpp"
#include "mounts.hpp"
#include "file_load.hpp"
#include "io.hpp"
#include "atx.hpp"
#include "wav_decode.hpp"

#include "diskio.h"

#define serial_read_timeout 5000

const uint8_t hsio_opt_to_index[] = {0x28, 0x10, 6, 5, 4, 3, 2, 1, 0};

const uint8_t percom_table[] = {
	0x28,0x03,0x00,0x12,0x00,0x00,0x00,0x80, 0x80,0x16,0x80,0x00, 0,
	0x28,0x03,0x00,0x1A,0x00,0x04,0x00,0x80, 0x80,0x20,0x80,0x00, 0,
	0x28,0x03,0x00,0x12,0x00,0x04,0x01,0x00, 0xE8,0x2C,0x00,0x01, 0,
	0x28,0x03,0x00,0x12,0x01,0x04,0x01,0x00, 0xE8,0x59,0x00,0x01, 0
};
#define percom_table_size 4

// XEX boot loader data
#include "boot_loader.h"

const int8_t boot_reloc_delta[] = {-2, -1, 0, 1, 2, 3};

volatile int8_t high_speed = -1;
volatile bool fs_error_or_change = false;

typedef struct __attribute__((__packed__)) {
	uint8_t device_id;
	uint8_t command_id;
	uint16_t sector_number;
	uint8_t checksum;
} sio_command_type;

static sio_command_type sio_command;

static uint8_t locate_percom(int drive_number) {
	int i = 0;
	uint8_t r = 0x80;
	while(i < percom_table_size) {
		if(!memcmp(&disk_headers[drive_number-1].data[2], &percom_table[i*13+8], 5)) {
			r = i;
			break;
		}
		i++;
	}
	return r;
}

static bool compare_percom(int drive_number) {
	int i = disk_headers[drive_number-1].atr_header.temp3;
	if(i & 0x80)
		return false;
	i &= 0x3;
	if(percom_table[i*13] != sector_buffer[0] || memcmp(&percom_table[i*13+2], &sector_buffer[2], 6))
		return false;
	return true;
}


static uint8_t sio_checksum(uint8_t *data, size_t len) {
	uint8_t cksum = 0;
	uint16_t nck;

	for(int i=0; i<len; i++) {
		nck = cksum + data[i];
		cksum = (nck > 0x00ff) ? 1 : 0;
		cksum += (nck & 0x00ff);
	}
	return cksum;
}

static bool try_get_sio_command() {

	static uint16_t freshly_changed = 0;
	bool r = true;
	int i = 0;

	// Assumption - when casette motor is on the active command line
	// is much more likely due to turbo activation and cassette transfer
	// should not be interrupted
	if(gpio_get(command_line_pin) || gpio_get(normal_motor_pin) == MOTOR_ON_STATE)
		return false;

	memset(&sio_command, 0x01, 5);

	// HiassofT suggests that if bytes == 5 with wrong checksum only or only a single framing error
	// repeat once without changing the speed
	// wrong #of bytes, multiple framing errors
	// continue reading while command line pin is low? When to break this? Let's say after 64?!

	while(!gpio_get(command_line_pin) && i<5 && uart_is_readable_within_us(uart1, serial_read_timeout) && !uart_get_hw(uart1)->rsr)
		((uint8_t *)&sio_command)[i++] = (uint8_t)uart_get_hw(uart1)->dr;

	//while(!gpio_get(command_line_pin) && uart_is_readable(uart1))
	//	((uint8_t *)&sio_command)[i] = (uint8_t)uart_get_hw(uart1)->dr;
	//if(!gpio_get(command_line_pin) && uart_is_readable(uart1))
	//	((uint8_t *)&sio_command)[i] = (uint8_t)uart_get_hw(uart1)->dr;

	if(gpio_get(command_line_pin) || uart_get_hw(uart1)->rsr || i != 5 ||
			sio_checksum((uint8_t *)&sio_command, 4) != sio_command.checksum || sio_command.command_id < 0x21) {
		hw_clear_bits(&uart_get_hw(uart1)->rsr, UART_UARTRSR_BITS);
		r = false;
	}

	if(r) {
		absolute_time_t t = make_timeout_time_us(1250); // According to Avery's manual 950us
		while (!gpio_get(command_line_pin) && absolute_time_diff_us(get_absolute_time(), t) > 0)
			tight_loop_contents();
		if(!gpio_get(command_line_pin))
			r = false;
		else
			freshly_changed = 0;
	}else{
		if(current_options[hsio_option_index] && !freshly_changed && high_speed >= 0) {
			high_speed ^= 1;
			// freshly_changed = 2 - high_speed;
			freshly_changed = 2;
			uint8_t s = high_speed ? current_options[hsio_option_index] : 0;
			uart_set_baudrate(uart1, current_options[clock_option_index] ? hsio_opt_to_baud_ntsc[s] : hsio_opt_to_baud_pal[s]);
		}else if(freshly_changed)
			freshly_changed--;
	}

	return r;
}

static uint8_t check_drive_and_sector_status(int drive_number, FSIZE_t *offset, FSIZE_t *to_read, bool op_write=false) {

	if(op_write && (disk_headers[drive_number-1].atr_header.flags & 0x1)) {
		disk_headers[drive_number-1].atr_header.temp2 &= 0xBF;
		return 'N';
	}
	*offset = sio_command.sector_number-1;
	if(*offset < 3) {
		*offset <<= 7;
		*to_read = 128;
	} else {
		*to_read = disk_headers[drive_number-1].atr_header.sec_size;
		*offset = 384+(*offset-3)*(*to_read);
	}
	if(sio_command.sector_number == 0 || *offset + *to_read > mounts[drive_number].status) {
		disk_headers[drive_number-1].atr_header.temp2 &= 0xEF;
		return 'N';
	}
	return 'A';
}

static uint8_t try_receive_data(int drive_number, FSIZE_t to_read) {
	int i=0;
	uint8_t r = 'A';
	uint8_t status = 0;
	while(i<to_read && uart_is_readable_within_us(uart1, serial_read_timeout))
		sector_buffer[i++] = (uint8_t)uart_get_hw(uart1)->dr;
	if(i != to_read || !uart_is_readable_within_us(uart1, serial_read_timeout) || (uint8_t)uart_get_hw(uart1)->dr != sio_checksum(sector_buffer, to_read)) {
		status |= 0x02;
		r = 'N';
	}
	disk_headers[drive_number-1].atr_header.temp1 = status;
	return r;
}

void main_sio_loop() {
	FILINFO fil_info;
	FSIZE_t offset, to_read;
	FRESULT f_op_stat;
	int i;
	uint bytes_read;
	absolute_time_t last_sd_check = get_absolute_time();
	sd_card_t *p_sd = sd_get_by_num(1);
	while(true) {
		uint8_t cd_temp = (gpio_get(p_sd->card_detect_gpio) == p_sd->card_detected_true);
		// Debounce 500ms - can it be smaller?
		if(cd_temp != sd_card_present && absolute_time_diff_us(last_sd_check, get_absolute_time()) > 500000) {
			last_sd_check = get_absolute_time();
			mutex_enter_blocking(&mount_lock);
			if(cd_temp)
				try_mount_sd();
			else {
				red_blinks = 4;
				if(!curr_path[0] || curr_path[0] == '1')
					last_file_name[0] = 0;
				sd_card_present = 0;
				//sd_card_t *p_sd = sd_get_by_num(1);
				//p_sd->sd_test_com(p_sd);
				// This one is enough and way quicker, we know by now that the card is gone
				p_sd->m_Status |= STA_NOINIT;
			}
			cd_temp = sd_card_present ^ 1;
			mutex_exit(&mount_lock);
		} else
			cd_temp = 0;
		if(cd_temp || last_access_error_drive >= 0) {
			mutex_enter_blocking(&mount_lock);
			for(i=0; i<5; i++) {
				if(last_access_error[i] || (cd_temp && mounts[i].mount_path[0] == '1')) {
					f_close(&mounts[i].fil);
					mounts[i].status = 0;
					mounts[i].mounted = false;
					mounts[i].mount_path[0] = 0;
					strcpy(&mounts[i].str[i ? 3 : 2],"  <EMPTY>   ");
					//red_blinks = 0;
					//green_blinks = 0;
					fs_error_or_change = true;
					blue_blinks = 0;
					update_rgb_led(false);
				}
				last_access_error[i] = false;
			}
			last_access_error_drive = -1;
			mutex_exit(&mount_lock);
		}
		for(i=0; i<5; i++) {
			mutex_enter_blocking(&mount_lock);
			if(!mounts[i].mounted || mounts[i].status) {
				mutex_exit(&mount_lock);
				continue;
			}
			mutex_enter_blocking(&fs_lock);
			if(/*f_mount(&fatfs[0], (const char *)mounts[i].mount_path, 1) == FR_OK && */ f_stat((const char *)mounts[i].mount_path, &fil_info) == FR_OK && f_open(&mounts[i].fil, (const char *)mounts[i].mount_path, FA_READ) == FR_OK) {
				if(!i) {
					reinit_pio();
					if(!strcasecmp(&mounts[i].mount_path[strlen(mounts[i].mount_path)-3], "CAS")) {
						wav_sample_size = 0;
						cas_sample_duration = (timing_base_clock+300)/600;
						cas_size = f_size(&mounts[0].fil);
						wav_filter_window_size = 0;
						if(f_read(&mounts[i].fil, &cas_header, sizeof(cas_header_type), &bytes_read) == FR_OK &&
							bytes_read == sizeof(cas_header_type) &&
							cas_header.signature == cas_header_FUJI)
								mounts[i].status = cas_read_forward(cas_header.chunk_length + sizeof(cas_header_type));
					} else {
						mounts[i].status = 0;
						if(f_read(&mounts[i].fil, &wav_header, sizeof(wav_header_type), &bytes_read) == FR_OK && bytes_read == sizeof(wav_header_type)) {
							mounts[i].status = bytes_read;
							while(wav_header.subchunk2_id != WAV_DATA) {
								mounts[i].status += wav_header.subchunk2_size;
								f_lseek(&mounts[i].fil, mounts[i].status);
								if(f_read(&mounts[i].fil, &wav_header.subchunk2_id, 8, &bytes_read) != FR_OK || bytes_read != 8) {
									mounts[i].status = 0;
									break;
								}
								mounts[i].status += 8;
							}
							if(!mounts[i].status || wav_header.chunk_id != WAV_RIFF || wav_header.format != WAV_WAVE || wav_header.subchunk1_id != WAV_FMT ||
								wav_header.subchunk1_size != 16 || wav_header.audio_format != 1 || wav_header.byte_rate != wav_header.sample_rate * wav_header.block_align ||
								wav_header.block_align != (wav_header.bits_per_sample / 8) * wav_header.num_channels) {
									mounts[i].status = 0;
							} else {
								silence_duration = 500; // Extra .5s of silence at the beginning
								cas_size = mounts[i].status + wav_header.subchunk2_size;
								cas_block_turbo = current_options[wav_option_index];
								init_wav();
							}
						}
					}
					cas_last_block_marker = true;
					if(!mounts[i].status)
						set_last_access_error(i);
					else if(last_drive == 0)
							last_drive = -1;
				} else {
					uint8_t disk_type = 0;
					if(f_read(&mounts[i].fil, sector_buffer, 4, &bytes_read) == FR_OK && bytes_read == 4) {
						if(*(uint16_t *)sector_buffer == 0x0296) // ATR magic
							disk_type = disk_type_atr;
						else if(*(uint16_t *)sector_buffer == 0xFFFF)
							disk_type = disk_type_xex;
						else if(*(uint32_t *)sector_buffer == 0x58385441) // AT8X
							disk_type = disk_type_atx;
						f_lseek(&mounts[i].fil, 0);
					}
					switch(disk_type) {
						case disk_type_atr:
							if(f_read(&mounts[i].fil, &disk_headers[i-1].atr_header, sizeof(atr_header_type), &bytes_read) == FR_OK && bytes_read == sizeof(atr_header_type)) {
								mounts[i].status = (disk_headers[i-1].atr_header.pars | ((disk_headers[i-1].atr_header.pars_high << 16) & 0xFF0000)) << 4;
								disk_headers[i-1].atr_header.temp2 = 0xFF;
								if(!current_options[mount_option_index] || (fil_info.fattrib & AM_RDO))
									disk_headers[i-1].atr_header.flags |= 0x1;
								disk_headers[i-1].atr_header.temp3 = locate_percom(i);
							} else
								disk_type = 0;
							break;
						case disk_type_xex:
							// Sectors occupied by the file itself
							offset = f_size(&mounts[i].fil);
							disk_headers[i-1].atr_header.pars = (offset+124)/125;
							disk_headers[i-1].atr_header.pars_high = offset % 125;
							if(!disk_headers[i-1].atr_header.pars_high)
								disk_headers[i-1].atr_header.pars_high = 125;
							// Incidentally this can be a proper SD or ED disk,
							// but this is perfectly OK
							mounts[i].status = (disk_headers[i-1].atr_header.pars+3+0x170)*128;
							disk_headers[i-1].atr_header.sec_size = 128;
							disk_headers[i-1].atr_header.flags = 0x1;
							disk_headers[i-1].atr_header.temp2 = 0xFF;
							disk_headers[i-1].atr_header.temp3 = 0x80;
							break;
						case disk_type_atx:
							if(loadAtxFile(&mounts[i].fil, i-1)) {
								mounts[i].status = 40*atx_track_size[i-1]*disk_headers[i-1].atr_header.sec_size-(disk_headers[i-1].atr_header.sec_size == 256 ? 384 : 0);
								disk_headers[i-1].atr_header.pars = mounts[i].status >> 4;
								disk_headers[i-1].atr_header.pars_high = 0x00;
								disk_headers[i-1].atr_header.flags = 0;
								if(!current_options[mount_option_index] || (fil_info.fattrib & AM_RDO))
									disk_headers[i-1].atr_header.flags |= 0x1;
								disk_headers[i-1].atr_header.temp2 = 0xFF;
								disk_headers[i-1].atr_header.temp3 = locate_percom(i) | (current_options[atx_option_index] ? 0x40 : 0x00);
							}else
								disk_type = 0;
							break;
						default:
							break;
					}
					if(!disk_type)
						set_last_access_error(i);
					else {
						disk_headers[i-1].atr_header.temp4 = disk_type;
						if(last_drive == i)
							last_drive = -1;
						// If the disk is not read-only re-open in r/w mode.
						if(!(disk_headers[i-1].atr_header.flags & 0x1)) {
							f_close(&mounts[i].fil);
							f_open(&mounts[i].fil, (const char *)mounts[i].mount_path, FA_WRITE | FA_READ);
						}
					}
				}
				//f_close(&fil);
			}else
				set_last_access_error(i);
			//f_mount(0, (const char *)mounts[i].mount_path, 1);
			mutex_exit(&fs_lock);
			mutex_exit(&mount_lock);
		}

		if(try_get_sio_command()) {
			int drive_number = sio_command.device_id-0x30;
			mutex_enter_blocking(&mount_lock);
			uint8_t r = 'A';
			int8_t atx_res;
			uint64_t us_pre_ce = 300;
			if(drive_number < 1 || drive_number > 4 || !mounts[drive_number].mounted || last_access_error[drive_number])
				goto ignore_sio_command_frame;
			sleep_us(100); // Needed for BiboDos according to atari_drive_emulator.c
			gpio_set_function(sio_tx_pin, GPIO_FUNC_UART);
			memset(sector_buffer, 0, sector_buffer_size);
			blue_blinks = (high_speed == 1) ? -1 : 0;
			update_rgb_led(false);
			update_last_drive(drive_number);
			disk_headers[drive_number-1].atr_header.temp1 = 0x0;
			f_op_stat = FR_OK;
			to_read = 0;
			switch(sio_command.command_id) {
				case 'S': // get status
					uart_putc_raw(uart1, r);
					to_read = 4;
					// TODO motor on only after first actual disk operation?
					// for not mounted drives report motor on but no disk (drive always present,
					// not only when the disk is in), or???
					sector_buffer[0] = disk_headers[drive_number-1].atr_header.temp1;
					sector_buffer[1] = disk_headers[drive_number-1].atr_header.temp2;
					//if(mounts[drive_number].mounted) {
					sector_buffer[0] |= 0x10; // motor on
					if(disk_headers[drive_number-1].atr_header.flags & 0x1)
						sector_buffer[0] |= 0x08; // write protect
					if(disk_headers[drive_number-1].atr_header.sec_size == 256) {
						sector_buffer[0] |= 0x20;
						if(disk_headers[drive_number-1].atr_header.temp3 & 0x03 == 0x03)
							sector_buffer[0] |= 0x40;
					} else if(mounts[drive_number].status == 0x20800) // 1040*128
						sector_buffer[0] |= 0x80; // medium density
					//} else {
					//	sector_buffer[1] &= 0x7F; // disk removed
					//}
					sector_buffer[2] = 0xe0;
					sector_buffer[3] = 0x0;
					break;
				case 'N': // read percom
					uart_putc_raw(uart1, r);
					if(disk_headers[drive_number-1].atr_header.temp3 & 0x80) {
						sector_buffer[0] = 1; // # tracks
						sector_buffer[1] = 3; // step rate
						offset = disk_headers[i-1].atr_header.sec_size;
						sector_buffer[6] = (offset >> 8) & 0xFF; // bytes / sec high
						sector_buffer[7] = offset & 0xFF; // bytes / sec low
						offset = 3+(mounts[i].status-384)/offset;
						sector_buffer[2] = (offset >> 8) & 0xFF; // # sectors high
						sector_buffer[3] = (offset & 0xFF); // # sectors low
						sector_buffer[4] = 0; // # sides
						sector_buffer[5] = 4; // record method
					} else {
						memcpy(sector_buffer, &percom_table[(disk_headers[drive_number-1].atr_header.temp3 & 0x3)*13], 8);
					}
					sector_buffer[8] = 0xFF;
					memset(&sector_buffer[9], 0x00, 3);
					to_read = 12;
					break;
				case 'R': // read sector
					switch(disk_headers[drive_number-1].atr_header.temp4) {
						case disk_type_atr:
							r = check_drive_and_sector_status(drive_number, &offset, &to_read);
							uart_putc_raw(uart1, r);
							if(r == 'N') break;
							green_blinks = -1;
							update_rgb_led(false);
							if((f_op_stat = mounted_file_transfer(drive_number, sizeof(atr_header_type)+offset, to_read , false)) != FR_OK) {
								set_last_access_error(drive_number);
								disk_headers[drive_number-1].atr_header.temp2 &= 0xEF;
							}
							else
								disk_headers[drive_number-1].atr_header.temp2 = 0xFF;
							break;
						case disk_type_xex:
							r = check_drive_and_sector_status(drive_number, &offset, &to_read);
							uart_putc_raw(uart1, r);
							if(r == 'N') break;
							green_blinks = -1;
							update_rgb_led(false);
							if(sio_command.sector_number >= 0x171) {
									offset = (sio_command.sector_number-0x171);
									if(offset == disk_headers[drive_number-1].atr_header.pars - 1) {
										to_read = disk_headers[drive_number-1].atr_header.pars_high;
									} else {
										sector_buffer[125]=((sio_command.sector_number+1)>>8) & 0xFF;
										sector_buffer[126]=((sio_command.sector_number+1) & 0xFF);
										to_read = 125;
									}
									sector_buffer[127] = to_read;
									offset *= 125;
									if((f_op_stat = mounted_file_transfer(drive_number, offset, to_read , false)) != FR_OK) {
										set_last_access_error(drive_number);
										disk_headers[drive_number-1].atr_header.temp2 &= 0xEF;
									}else
										disk_headers[drive_number-1].atr_header.temp2 = 0xFF;
									to_read = 128;
							} else {
								if(sio_command.sector_number <= 2) {
									offset = 128*(sio_command.sector_number-1);
									memcpy(sector_buffer, &boot_loader[offset], 128);
									if(boot_reloc_delta[current_options[xex_option_index]])
										for(i=0; i<boot_reloc_locs_size; i++)
											if(boot_reloc_locs[i] >= offset && boot_reloc_locs[i] < offset + 128)
												sector_buffer[boot_reloc_locs[i]-offset] += boot_reloc_delta[current_options[xex_option_index]];
								} else {
									if(sio_command.sector_number == 0x168) { // VTOC
										uint total_sectors = mounts[drive_number].status >> 7;
										uint vtoc_sectors = total_sectors >> 10;
										uint rem = total_sectors - (vtoc_sectors << 10);
										if(rem > 943) vtoc_sectors += 2;
										else if(rem) vtoc_sectors++;
										if(!(vtoc_sectors % 2)) vtoc_sectors++;
										total_sectors -= (vtoc_sectors + 12);
										sector_buffer[0] = (uint8_t)((vtoc_sectors + 3)/2);
										sector_buffer[1] = (total_sectors & 0xFF);
										sector_buffer[2] = ((total_sectors >> 8) & 0xFF);
									}else if(sio_command.sector_number == 0x169) { // Directory
										uint file_sectors = disk_headers[drive_number-1].atr_header.pars;
										sector_buffer[0] = (file_sectors > 0x28F) ? 0x46 : 0x42;
										sector_buffer[1] = (file_sectors & 0xFF);
										sector_buffer[2] = ((file_sectors >> 8) & 0xFF);
										sector_buffer[3] = 0x71;
										sector_buffer[4] = 0x01;
										// The file name is guaranteed to have an extension that is 3 characters long
										memset(&sector_buffer[5], ' ', 8);
										offset = 0;
										for(i=0; i<11; i++) {
											uint8_t c = mounts[drive_number].str[3+offset];
											if(c == '.')
												i = 7;
											else {
												// 'a'-1 or > 'z' -> '@'
												// To uppercase: 'a'..'z' -> -32 Needed?
												if(c == 'a'-1 || c > 'z') c = '@';
												sector_buffer[5+i] = c;
											}
											offset++;
										}
									}
								}
							}
							break;
						case disk_type_atx:
							// delay for the time the drive takes to process the request
							sleep_us(us_drive_request_delay);
							if(sio_command.sector_number == 0 || sio_command.sector_number > 40*atx_track_size[drive_number-1]) {
								disk_headers[drive_number-1].atr_header.temp2 &= 0xEF;
								r = 'N';
							}
							uart_putc_raw(uart1, r);
							if (r == 'N')
								break;
							green_blinks = -1;
							update_rgb_led(false);
							to_read = disk_headers[drive_number-1].atr_header.sec_size;
							us_pre_ce = 0; // Handled in transferAtxSector
							atx_res = transferAtxSector(drive_number-1, sio_command.sector_number, &disk_headers[drive_number-1].atr_header.temp2);
							if(atx_res) {
								f_op_stat = FR_INT_ERR;
								if(atx_res < 0)
									set_last_access_error(drive_number);
							}
							// break;
						default:
							break;
					}
					green_blinks = 0;
					update_rgb_led(false);
					break;
				case 'O': // write percom
					// Only writable disks react to PERCOM write command frame
					if((disk_headers[drive_number-1].atr_header.flags & 0x1) || (disk_headers[drive_number-1].atr_header.temp3 & 0x80))
						r = 'N';
					uart_putc_raw(uart1, r);
					if(r == 'N') break;
					r = try_receive_data(drive_number, 12);
					if(r == 'A' && !compare_percom(drive_number))
						r = 'N';
					sleep_us(850);
					uart_putc_raw(uart1, r);
					if(r == 'N') break;
					 // Mark that PERCOM has been written successfully
					disk_headers[drive_number-1].atr_header.temp3 |= 0x04;
					break;
				case 'P': // put sector
				case 'W': // write (+verify) sector
					switch(disk_headers[drive_number-1].atr_header.temp4) {
						case disk_type_atr:
							r = check_drive_and_sector_status(drive_number, &offset, &to_read, true);
							uart_putc_raw(uart1, r);
							if(r == 'N')
								break;
							r = try_receive_data(drive_number, to_read);
							sleep_us(850);
							uart_putc_raw(uart1, r);
							if(r == 'N')
								break;
							red_blinks = -1;
							update_rgb_led(false);
							if((f_op_stat = mounted_file_transfer(drive_number, sizeof(atr_header_type)+offset, to_read, true)) != FR_OK) {
								disk_headers[drive_number-1].atr_header.temp1 |= 0x4; // write error
								set_last_access_error(drive_number);
							} else if (sio_command.command_id == 'W') {
								if((f_op_stat = mounted_file_transfer(drive_number, sizeof(atr_header_type)+offset, to_read, false, to_read)) != FR_OK)
									set_last_access_error(drive_number);
								else if(memcmp(sector_buffer, &sector_buffer[to_read], to_read)) {
									f_op_stat = FR_INT_ERR;
									disk_headers[drive_number-1].atr_header.temp1 |= 0x4; // write error
								}
							}
							to_read = 0;
							break;
						case disk_type_atx:
							sleep_us(us_drive_request_delay);
							if(sio_command.sector_number == 0 || sio_command.sector_number > 40*atx_track_size[drive_number-1]) {
								disk_headers[drive_number-1].atr_header.temp2 &= 0xEF;
								r = 'N';
							}
							if(disk_headers[drive_number-1].atr_header.flags & 0x1) {
								disk_headers[drive_number-1].atr_header.temp2 &= 0xBF;
								r = 'N';
							}
							uart_putc_raw(uart1, r);
							if (r == 'N')
								break;
							to_read = disk_headers[drive_number-1].atr_header.sec_size;
							r = try_receive_data(drive_number, to_read);
							sleep_us(850);
							uart_putc_raw(uart1, r);
							if(r == 'N')
								break;
							red_blinks = -1;
							update_rgb_led(false);
							atx_res = transferAtxSector(drive_number-1, sio_command.sector_number, &disk_headers[drive_number-1].atr_header.temp2, true, sio_command.command_id == 'W');
							if(atx_res) {
								f_op_stat = FR_INT_ERR;
								disk_headers[drive_number-1].atr_header.temp1 |= 0x4; // write error
								if(atx_res < 0)
									set_last_access_error(drive_number);
							}
							to_read = 0;
							// break;
						case disk_type_xex:
						default:
							break;
					}
					red_blinks = 0;
					update_rgb_led(false);
					break;
				case '!': // Format SD / PERCOM
				case '"': // Format ED
					switch(disk_headers[drive_number-1].atr_header.temp4) {
					case disk_type_atr:
						i = sio_command.command_id - 0x21;
						if( /* !mounts[drive_number].mounted || */
							(disk_headers[drive_number-1].atr_header.flags & 0x1) ||
							(i != (disk_headers[drive_number-1].atr_header.temp3 & 0x3)
								&& !(disk_headers[drive_number-1].atr_header.temp3 & 0x4)))
							r = 'N';
						uart_putc_raw(uart1, r);
						if(r == 'N') break;
						red_blinks = -1;
						update_rgb_led(false);
						to_read = disk_headers[drive_number-1].atr_header.sec_size;
						memset(sector_buffer, 0, to_read);
						if((f_op_stat = mounted_file_transfer(drive_number, sizeof(atr_header_type)+i*128, 128, true, 0, mounts[drive_number].status >> 7)) != FR_OK)
							set_last_access_error(drive_number);
						sector_buffer[0] = sector_buffer[1] = 0xFF;
						break;
					case disk_type_xex:
					case disk_type_atx:
						r = 'N';
						uart_putc_raw(uart1, r);
						// break;
					default:
						break;
					}
					red_blinks = 0;
					update_rgb_led(false);
					break;
				case '?': // get speed index
					if(!current_options[hsio_option_index] || disk_headers[drive_number-1].atr_header.temp4 == disk_type_atx)
						r = 'N';
					uart_putc_raw(uart1, r);
					if(r == 'N') break;
					sector_buffer[0] = hsio_opt_to_index[current_options[hsio_option_index]];
					to_read = 1;
					break;
				default:
					r = 'N';
					uart_putc_raw(uart1, r);
					break;
			}
			if(r == 'A') {
				uart_tx_wait_blocking(uart1);
				sleep_us(us_pre_ce);
				uart_putc_raw(uart1, f_op_stat == FR_OK ? 'C' : 'E');
				if(to_read) {
					uart_tx_wait_blocking(uart1);
					sleep_us(150); // Another one from atari_drive_emulator.c
					uart_write_blocking(uart1, sector_buffer, to_read);
					uart_putc_raw(uart1, sio_checksum(sector_buffer, to_read));
				}
				uart_tx_wait_blocking(uart1);
				if(sio_command.command_id == '?') {
					high_speed = 1;
					r = current_options[hsio_option_index];
					uart_set_baudrate(uart1, current_options[clock_option_index] ? hsio_opt_to_baud_ntsc[r] : hsio_opt_to_baud_pal[r]);
				}
			}
ignore_sio_command_frame:
			blue_blinks = 0;
			update_rgb_led(false);
			mutex_exit(&mount_lock);
		} else if(mounts[0].mounted && (offset = mounts[0].status) && offset + wav_filter_window_size*wav_header.block_align < cas_size) {
			// Including wav_sample_size allows for better pre-loading of the decoded WAV data
			// into the PIO queue, however, it is then more likely to block the whole SIO loop
			if(/*wav_sample_size || */ cas_motor_on()) {
				if(wav_sample_size)
					to_read = std::min((uint32_t)sector_buffer_size, cas_size - offset);
				else
					to_read = std::min(cas_header.chunk_length-cas_block_index, (cas_block_turbo ? 128 : 256)*cas_block_multiple);
				mutex_enter_blocking(&mount_lock);
				if(!wav_sample_size) {
					green_blinks = -1;
					blue_blinks = cas_block_turbo ? -1 : 0;
					update_rgb_led(false);
				}
				if(mounted_file_transfer(0, offset, to_read, false) != FR_OK) {
					set_last_access_error(0);
					mutex_exit(&mount_lock);
					if(!wav_sample_size) {
						green_blinks = 0;
						blue_blinks = 0;
						update_rgb_led(false);
					}
					continue;
				}
				update_last_drive(0);
				offset += to_read;
				mounts[0].status = offset - wav_filter_window_size*wav_header.block_align;
				gpio_set_function(sio_tx_pin, GPIO_FUNC_PIOX);
				uint8_t silence_bit = (cas_block_turbo ? 0 : 1);
				while(silence_duration > 0) {
					uint16_t silence_block_len = silence_duration;
					if(silence_block_len >= max_clock_ms)
						silence_block_len = max_clock_ms;
					pio_enqueue(silence_bit, (timing_base_clock/1000)*silence_block_len);
					silence_duration -= silence_block_len;
				}
				if(wav_sample_size) {
					// WAV file
					// The first alternative seems to work better - push the data to PIO if there was no action (silence block)
					// when decoding the last WAV sample block
					if(!cas_last_block_marker) {
					//if(wav_last_count > wav_silence_threshold) {
						uint32_t wav_scaled_bit_duration = wav_sample_div*wav_last_count*wav_scaled_sample_rate/wav_header.sample_rate;
						wav_last_count = 0;
						pio_enqueue(cas_fsk_bit, wav_scaled_bit_duration*cas_sample_duration);
						if(cas_fsk_bit == wav_last_duration_bit)
							wav_last_duration += wav_scaled_bit_duration;
						else {
							wav_last_duration_bit = cas_fsk_bit;
							wav_last_duration = 0;
						}
					}
					cas_last_block_marker = false;
					offset = 0;
					while(offset + wav_filter_window_size*wav_header.block_align < to_read) {
						int32_t g1, g2;
						int16_t wav_last_sample = 0;
						if(wav_sample_size == 2) {
							int16_t *v = (int16_t *)&sector_buffer[offset+2*(wav_header.num_channels-1)];
							if(!cas_block_turbo) {
								g1 = goertzel_int16(v, zcoeff1);
								g2 = goertzel_int16(v, zcoeff2);
							}
							wav_last_sample = *v;
						}else{
							int8_t *v = (int8_t *)&sector_buffer[offset+(wav_header.num_channels-1)];
							if(!cas_block_turbo) {
								g1 = goertzel_int8(v, zcoeff1);
								g2 = goertzel_int8(v, zcoeff2);
							}
							wav_last_sample = *v * 256;
						}
						if(cas_block_turbo) {
							int16_t ns = 20*filter1(filter2(wav_last_sample));

							if (pwm_bit)
								pwm_bit = ns >= wav_prev_sample - 200;
							else
								pwm_bit = ns > wav_prev_sample + 200;
							wav_prev_sample = ns;
						} else {
							//if(wav_last_sample >= -1000 && wav_last_sample <= 1000)
							if(wav_last_sample >= -3200 && wav_last_sample <= 3200)
								wav_last_silence++;
							else
								wav_last_silence = 0;
							pwm_bit = (wav_last_silence > wav_silence_threshold) ? 1 : (filter_avg(g2-g1) > 0);
						}

						offset += wav_sample_div*wav_header.block_align;

						if(pwm_bit == cas_fsk_bit)
							wav_last_count++;
						else {
							uint32_t wav_scaled_bit_duration = wav_sample_div*wav_last_count*wav_scaled_sample_rate/wav_header.sample_rate;
							// The first alternative filters stray signal flips in the long steady signal blocks
							if((cas_block_turbo || wav_last_duration < 1500 || wav_scaled_bit_duration > 10 || wav_last_duration_bit) && wav_scaled_bit_duration) {
							//if(wav_scaled_bit_duration) {
								pio_enqueue(cas_fsk_bit, wav_scaled_bit_duration*cas_sample_duration);
								if(cas_fsk_bit == wav_last_duration_bit)
									wav_last_duration += wav_scaled_bit_duration;
								else {
									wav_last_duration_bit = cas_fsk_bit;
									wav_last_duration = 0;
								}
							}
							cas_last_block_marker = true;
							cas_fsk_bit = pwm_bit;
							wav_last_count = 1;
						}
					}
				} else {
					// CAS file
					cas_block_index += to_read;
					int bs, be, bd;
					uint8_t b;
					uint16_t ld;
					for(i=0; i < to_read; i += cas_block_multiple) {
						switch(cas_header.signature) {
							case cas_header_data:
								pio_enqueue(0, cas_sample_duration);
								b = sector_buffer[i];
								for(int j=0; j!=8; j++) {
									pio_enqueue(b & 0x1, cas_sample_duration);
									b >>= 1;
								}
								pio_enqueue(1, cas_sample_duration);
								break;
							case cas_header_fsk:
							case cas_header_pwml:
								ld = *(uint16_t *)&sector_buffer[i];
								// ld = (sector_buffer[i] & 0xFF) | ((sector_buffer[i+1] << 8) & 0xFF00);
								if(ld != 0)
									pio_enqueue(cas_fsk_bit, (cas_block_turbo ? pwm_sample_duration : (timing_base_clock/10000))*ld);
								cas_fsk_bit ^= 1;
								break;
							case cas_header_pwmc:
								ld = (sector_buffer[i+1] & 0xFF) | ((sector_buffer[i+2] << 8) & 0xFF00);
								for(uint16_t j=0; j<ld; j++) {
									pio_enqueue(pwm_bit, sector_buffer[i]*pwm_sample_duration/2);
									pio_enqueue(pwm_bit^1, sector_buffer[i]*pwm_sample_duration/2);
								}
								break;
							case cas_header_pwmd:
								b = sector_buffer[i];
								if (pwm_bit_order) {
									bs=7; be=-1; bd=-1;
								} else {
									bs=0; be=8; bd=1;
								}
								for(int j=bs; j!=be; j += bd) {
									uint8_t d = cas_header.aux.aux_b[(b >> j) & 0x1];
									pio_enqueue(pwm_bit, d*pwm_sample_duration/2);
									pio_enqueue(pwm_bit^1, d*pwm_sample_duration/2);
								}
							default:
								break;
						}
					}
					if(cas_block_index == cas_header.chunk_length && offset < cas_size && mounts[0].mounted) {
						mutex_enter_blocking(&fs_lock);
						offset = cas_read_forward(offset);
						mutex_exit(&fs_lock);
						mounts[0].status = offset;
						if(!offset)
							set_last_access_error(0);
						else if(cas_header.signature == cas_header_pwmc || cas_header.signature == cas_header_data || (cas_header.signature == cas_header_fsk && silence_duration) || dma_block_turbo^cas_block_turbo) {
							while(!pio_sm_is_tx_fifo_empty(cas_pio, dma_block_turbo ? sm_turbo : sm))
								tight_loop_contents();
							// This is to account for the possible motor off switching lag
							sleep_ms(MOTOR_CHECK_INTERVAL_MS);
						}
					}
					blue_blinks = 0;
					green_blinks = 0;
					update_rgb_led(false);
				}
			}
			mutex_exit(&mount_lock);
		}else if(create_new_file > 0 && last_drive == -1)
			create_new_file = create_new_disk_image();
		else if(last_drive == -1)
			check_and_save_config();
	}
}
