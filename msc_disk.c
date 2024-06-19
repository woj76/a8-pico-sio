#include "tusb.h"
#include "fatfs_disk.h"

static bool ejected = false;

void tud_msc_inquiry_cb(uint8_t lun, uint8_t vendor_id[8], uint8_t product_id[16], uint8_t product_rev[4]) {
	(void) lun;

	const char vid[] = "PicoSIO";
	const char pid[] = "Mass Storage";
	const char rev[] = "1.0";

	memcpy(vendor_id  , vid, strlen(vid));
	memcpy(product_id , pid, strlen(pid));
	memcpy(product_rev, rev, strlen(rev));
}

bool tud_msc_test_unit_ready_cb(uint8_t lun) {
	(void) lun;
	if (ejected) {
		tud_msc_set_sense(lun, SCSI_SENSE_NOT_READY, 0x3a, 0x00);
		return false;
	}
	return true;
}

void tud_msc_capacity_cb(uint8_t lun, uint32_t* block_count, uint16_t* block_size) {
	(void) lun;
	*block_count = SECTOR_NUM;
	*block_size  = SECTOR_SIZE;
}

bool tud_msc_start_stop_cb(uint8_t lun, uint8_t power_condition, bool start, bool load_eject) {
	(void) lun;
	(void) power_condition;
	if ( load_eject ) {
		if (start) {
		} else {
			ejected = true;
		}
	}
	return true;
}

int32_t tud_msc_read10_cb(uint8_t lun, uint32_t lba, uint32_t offset, void* buffer, uint32_t bufsize) {
	(void) lun;

	if(offset != 0) return -1;
	if(bufsize != SECTOR_SIZE) return -1;

	uint32_t status = fatfs_disk_read(buffer, lba, 1);
	if(status != 0) return -1;
	return (int32_t) bufsize;
}

bool tud_msc_is_writable_cb (uint8_t lun) {
	(void) lun;
	return true;
}

alarm_id_t alarm_id = -1;

int64_t sync_callback(alarm_id_t id, void *user_data) {
	fatfs_disk_sync();
}

int32_t tud_msc_write10_cb(uint8_t lun, uint32_t lba, uint32_t offset, uint8_t* buffer, uint32_t bufsize) {
	(void) lun;

	if(offset != 0) return -1;
	if(bufsize != SECTOR_SIZE) return -1;

	uint32_t status = fatfs_disk_write(buffer, lba, 1);

	if(alarm_id >= 0)
		cancel_alarm(alarm_id);
	alarm_id = add_alarm_in_ms(250, sync_callback, NULL, false);

	if(status != 0) return -1;
	return (int32_t) bufsize;
}

int32_t tud_msc_scsi_cb (uint8_t lun, uint8_t const scsi_cmd[16], void* buffer, uint16_t bufsize) {

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
		if(in_xfer) {
			memcpy(buffer, response, (size_t) resplen);
		} else {
		}
	}

	return (int32_t) resplen;
}
