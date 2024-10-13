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

#pragma once

#include "flash_fs.h"
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

#define SECTOR_NUM NUM_FAT_SECTORS
#define SECTOR_SIZE 512

void create_fatfs_disk();
bool mount_fatfs_disk();
bool fatfs_is_mounted();
uint32_t fatfs_disk_read(uint8_t* buff, uint32_t sector, uint32_t count);
uint32_t fatfs_disk_write(const uint8_t* buff, uint32_t sector, uint32_t count);
void fatfs_disk_sync();

#ifdef __cplusplus
}
#endif
