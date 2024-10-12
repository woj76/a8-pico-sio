/* This file or its parts come originally from the A8PicoCart
 * project, see https://github.com/robinhedwards/A8PicoCart for additional
 * information, license, and credits (see also below). The major modification is
 * the ability to handle various sizes of the Pico FLASH memory that stores the
 * FAT file system, not only the 15MB drive on a 16MB Pico clone.
 */

/**
  *    _   ___ ___ _       ___          _
  *   /_\ ( _ ) _ (_)__ _ / __|__ _ _ _| |_
  *  / _ \/ _ \  _/ / _/_\ (__/ _` | '_|  _|
  * /_/ \_\___/_| |_\__\_/\___\__,_|_|  \__|
  *
  *
  * Atari 8-bit cartridge for Raspberry Pi Pico
  *
  * Robin Edwards 2023
  */

#include "ff.h"
#include "diskio.h"
#include "fatfs_disk.h"

// #include <stdio.h>

// #include "pico/stdlib.h"
#include "hardware/flash.h"

bool flashfs_is_mounted = false;

bool mount_fatfs_disk() {
	int err = flash_fs_mount();
	if (err)
		return false;
	flashfs_is_mounted = true;
	return true;
}

bool fatfs_is_mounted() {
	return flashfs_is_mounted;
}

void create_fatfs_disk() {
	flash_fs_create();
	flashfs_is_mounted = true;

	FATFS fs;
	FIL fil;
	FRESULT res;
	BYTE work[FF_MAX_SS];

	res = f_mkfs("", 0, work, sizeof(work));
	f_mount(&fs, "0:", 0);
	f_setlabel("A8-Pico-SIO");
	res = f_open(&fil, "INFO.TXT", FA_CREATE_NEW | FA_WRITE);
	f_puts("Atari 8-bit Pico SIO USB device\r\n(c) 2024 woj@AtariAge, USB device inspired and based on A8PicoCart code by Electrotrains (c) 2023\r\nDrag your ATR, ATX, XEX, or CAS files in here!\r\n", &fil);
	f_close(&fil);
	f_mount(0, "0:", 0);
}

uint32_t fatfs_disk_read(uint8_t* buff, uint32_t sector, uint32_t count) {
	if (!flashfs_is_mounted)
		return RES_ERROR;
	if (sector < 0 || sector >= SECTOR_NUM)
		return RES_PARERR;

	for (int i=0; i<count; i++)
		flash_fs_read_FAT_sector(sector + i, buff + (i*SECTOR_SIZE));
	return RES_OK;
}

uint32_t fatfs_disk_write(const uint8_t* buff, uint32_t sector, uint32_t count) {
	if (!flashfs_is_mounted)
		return RES_ERROR;
	if (sector < 0 || sector >= SECTOR_NUM)
		return RES_PARERR;

	for (int i=0; i<count; i++) {
		flash_fs_write_FAT_sector(sector + i, buff + (i*SECTOR_SIZE));
		if (!flash_fs_verify_FAT_sector(sector + i, buff + (i*SECTOR_SIZE)))
			return RES_ERROR;
	}
	return RES_OK;
}

void fatfs_disk_sync() {
	flash_fs_sync();
}
