/*-----------------------------------------------------------------------*/
/* Low level disk I/O module SKELETON for FatFs     (C)ChaN, 2019        */
/*-----------------------------------------------------------------------*/
/* If a working storage control module is available, it should be        */
/* attached to the FatFs via a glue function rather than modifying it.   */
/* This is an example of glue functions to attach various exsisting      */
/* storage control modules to the FatFs module with a defined API.       */
/*-----------------------------------------------------------------------*/

#include "ff.h"			/* Obtains integer types */
#include "diskio.h"		/* Declarations of disk functions */

#define DEV_FLASH		0
#define DEV_SD			1

#include "fatfs_disk.h"

#include "sd_card.h"

DSTATUS disk_status (BYTE pdrv) {
	switch(pdrv) {
		case DEV_FLASH:
			return 0;
		case DEV_SD: {
			sd_card_t *p_sd = sd_get_by_num(pdrv);
			if (!p_sd)
				return RES_PARERR;
			sd_card_detect(p_sd);
			return p_sd->m_Status;
		}
		default:
			return STA_NOINIT;
	}
}

DSTATUS disk_initialize (BYTE pdrv) {
	switch(pdrv) {
		case DEV_FLASH:
			return 0;
		case DEV_SD: {
			bool rc = sd_init_driver();
			if (!rc)
				return RES_NOTRDY;
			sd_card_t *p_sd = sd_get_by_num(pdrv);
			if (!p_sd)
				return RES_PARERR;
			return p_sd->init(p_sd);
		}
		default:
			return STA_NOINIT;
	}
}

static int sdrc2dresult(int sd_rc) {
	switch (sd_rc) {
		case SD_BLOCK_DEVICE_ERROR_NONE:
			return RES_OK;
		case SD_BLOCK_DEVICE_ERROR_UNUSABLE:
		case SD_BLOCK_DEVICE_ERROR_NO_RESPONSE:
		case SD_BLOCK_DEVICE_ERROR_NO_INIT:
		case SD_BLOCK_DEVICE_ERROR_NO_DEVICE:
			return RES_NOTRDY;
		case SD_BLOCK_DEVICE_ERROR_PARAMETER:
		case SD_BLOCK_DEVICE_ERROR_UNSUPPORTED:
			return RES_PARERR;
		case SD_BLOCK_DEVICE_ERROR_WRITE_PROTECTED:
			return RES_WRPRT;
		case SD_BLOCK_DEVICE_ERROR_CRC:
		case SD_BLOCK_DEVICE_ERROR_WOULD_BLOCK:
		case SD_BLOCK_DEVICE_ERROR_ERASE:
		case SD_BLOCK_DEVICE_ERROR_WRITE:
		default:
			return RES_ERROR;
	}
}

DRESULT disk_read (BYTE pdrv, BYTE *buff, LBA_t sector, UINT count) {
	switch(pdrv) {
		case DEV_FLASH:
			return fatfs_disk_read((uint8_t*)buff, sector, count);
		case DEV_SD: {
			sd_card_t *p_sd = sd_get_by_num(pdrv);
			if (!p_sd)
				return RES_PARERR;
			int rc = p_sd->read_blocks(p_sd, buff, sector, count);
			return sdrc2dresult(rc);
		}
		default:
			return RES_PARERR;
	}
}

#if FF_FS_READONLY == 0

DRESULT disk_write (BYTE pdrv, const BYTE *buff, LBA_t sector, UINT count) {
	switch(pdrv) {
		case DEV_FLASH:
			return fatfs_disk_write((const uint8_t*)buff, sector, count);
		case DEV_SD: {
			sd_card_t *p_sd = sd_get_by_num(pdrv);
			if (!p_sd)
				return RES_PARERR;
				int rc = p_sd->write_blocks(p_sd, buff, sector, count);
			return sdrc2dresult(rc);
		}
		default:
			return RES_PARERR;
	}
}

#endif

DRESULT disk_ioctl (BYTE pdrv, BYTE cmd, void *buff) {

	switch(pdrv) {
		case DEV_FLASH:
			switch(cmd) {
				case CTRL_SYNC:
					fatfs_disk_sync();
					return RES_OK;
				case GET_SECTOR_COUNT:
					*(LBA_t*) buff = SECTOR_NUM;
					return RES_OK;
				case GET_SECTOR_SIZE:
					*(WORD*) buff = SECTOR_SIZE;
					return RES_OK;
				case GET_BLOCK_SIZE:
					*(DWORD*) buff = 1;
					return RES_OK;
				case CTRL_TRIM:
					return RES_OK;
				default:
					return RES_PARERR;
		}
		case DEV_SD: {
			sd_card_t *p_sd = sd_get_by_num(pdrv);
			if (!p_sd)
				return RES_PARERR;
			switch (cmd) {
				case GET_SECTOR_COUNT: {
					static LBA_t n;
					n = sd_sectors(p_sd);
					*(LBA_t *)buff = n;
					if (!n)
						return RES_ERROR;
					return RES_OK;
				}
				case GET_SECTOR_SIZE:
					*(WORD*) buff = SECTOR_SIZE;
					return RES_OK;
				case GET_BLOCK_SIZE: {
					static DWORD bs = 1;
					*(DWORD *)buff = bs;
					return RES_OK;
				}
				case CTRL_SYNC:
					return RES_OK;
				default:
					return RES_PARERR;
			}
		}
		default:
			return RES_PARERR;
	}
}
