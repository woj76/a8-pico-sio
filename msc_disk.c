/*
 * This file is part of the a8-pico-sio project --
 * An Atari 8-bit SIO drive and (turbo) tape emulator for
 * Raspberry Pi Pico, see
 *
 *         https://github.com/woj76/a8-pico-sio
 *
 * For information on what / whose work it is based on, check below and the
 * corresponding project repository.
 */

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

#include "tusb.h"
#include "fatfs_disk.h"
#include "ff.h"
#include "diskio.h"

#define MAX_LUN 2

static bool ejected[MAX_LUN] =
#if MAX_LUN == 1
	{true};
#else
	{true, true};
#endif

static void try_disk_init(uint8_t lun) {
	if(!(disk_initialize(lun) & (lun ? (STA_NOINIT | STA_NODISK): 0xFF)))
		ejected[lun] = false;
}

void tud_mount_cb() {
	if (!mount_fatfs_disk())
		create_fatfs_disk();
	for(uint8_t lun = 0; lun < MAX_LUN; lun++)
		try_disk_init(lun);
}

void tud_umount_cb() {}

uint8_t tud_msc_get_maxlun_cb(void) {
	return MAX_LUN;
}

void tud_msc_inquiry_cb(uint8_t lun, uint8_t vendor_id[8], uint8_t product_id[16], uint8_t product_rev[4]) {

	const char vid[] = "PicoSIO";
	const char pid[] = "Mass Storage I";
	const char rev[] = "1.0";

	int pid_len = strlen(pid);
	memcpy(vendor_id, vid, strlen(vid));
	memcpy(product_id, pid, pid_len);
	if(lun)
		product_id[pid_len-1] = 'E';
	memcpy(product_rev, rev, strlen(rev));
}

bool tud_msc_test_unit_ready_cb(uint8_t lun) {

	if(disk_status(lun) & STA_NOINIT)
		return false;

	if (ejected[lun]) {
		tud_msc_set_sense(lun, SCSI_SENSE_NOT_READY, 0x3a, 0x00);
		return false;
	}

	return true;
}

void tud_msc_capacity_cb(uint8_t lun, uint32_t* block_count, uint16_t* block_size) {
	disk_ioctl(lun, GET_SECTOR_COUNT, block_count);
	disk_ioctl(lun, GET_SECTOR_SIZE, block_size);
}

bool tud_msc_start_stop_cb(uint8_t lun, uint8_t power_condition, bool start, bool load_eject) {
	(void) power_condition;
	if (load_eject) {
		if (start)
			return !ejected[lun];
		else {
			if (disk_ioctl(lun, CTRL_SYNC, NULL) != RES_OK)
				return false;
			else
				ejected[lun] = true;
		}
	} else if (!start && disk_ioctl(lun, CTRL_SYNC, NULL) != RES_OK)
		return false;
	return true;
}

int32_t tud_msc_read10_cb(uint8_t lun, uint32_t lba, uint32_t offset, void* buffer, uint32_t bufsize) {

	if(offset)
		return -1;

	uint16_t sec_size;
	disk_ioctl(lun, GET_SECTOR_SIZE, &sec_size);
	if(bufsize != sec_size)
		return -1;

	return disk_read(lun, buffer, lba, 1) == RES_OK ? (int32_t) bufsize : -1;
}

bool tud_msc_is_writable_cb (uint8_t lun) {
	return lun ? !(disk_status(lun) & STA_PROTECT) : true;
}

alarm_id_t alarm_id = -1;

int64_t sync_callback(alarm_id_t id, void *user_data) {
	for(uint8_t lun = 0; lun < MAX_LUN; lun++)
		// For lun == 1 this seems to be a no-op
		disk_ioctl(lun, CTRL_SYNC, NULL);
}

int32_t tud_msc_write10_cb(uint8_t lun, uint32_t lba, uint32_t offset, uint8_t* buffer, uint32_t bufsize) {

	if(offset)
		return -1;

	uint16_t sec_size;
	disk_ioctl(lun, GET_SECTOR_SIZE, &sec_size);
	if(bufsize != sec_size)
		return -1;

	DSTATUS status = disk_write(lun, buffer, lba, 1);

	if(alarm_id >= 0)
		cancel_alarm(alarm_id);
	alarm_id = add_alarm_in_ms(250, sync_callback, NULL, false);

	return status == RES_OK ? (int32_t) bufsize : -1;
}

int32_t tud_msc_scsi_cb (uint8_t lun, uint8_t const scsi_cmd[16], void* buffer, uint16_t bufsize) {
/*
	void const* response = NULL;
	int32_t resplen = 0;

	bool in_xfer = true;

	switch (scsi_cmd[0]) {
		default:
			tud_msc_set_sense(lun, SCSI_SENSE_ILLEGAL_REQUEST, 0x20, 0x00);
			resplen = -1;
			break;
	}

	if ( resplen > bufsize )
		resplen = bufsize;

	if ( response && (resplen > 0) ) {
		if(in_xfer)
			memcpy(buffer, response, (size_t) resplen);
	}

	return (int32_t) resplen;
*/
	tud_msc_set_sense(lun, SCSI_SENSE_ILLEGAL_REQUEST, 0x20, 0x00);
	return -1;
}
