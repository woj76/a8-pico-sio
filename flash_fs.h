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

#ifdef __cplusplus
extern "C" {
#endif

#include <stdbool.h>

#define HW_FLASH_STORAGE_BASE  (1024*1024)
#define BOARD_SIZE (PICO_FLASH_SIZE_BYTES/(1024*1024)) // in MB
#define NUM_FLASH_SECTORS ((BOARD_SIZE - 1) * 256)
#define NUM_FAT_SECTORS (NUM_FLASH_SECTORS * 8 - 4)
#define MAP_ENTRIES (BOARD_SIZE-1)

int flash_fs_mount();
void flash_fs_create();
void flash_fs_sync();
void flash_fs_read_FAT_sector(uint16_t fat_sector, void *buffer);
void flash_fs_write_FAT_sector(uint16_t fat_sector, const void *buffer);
bool flash_fs_verify_FAT_sector(uint16_t fat_sector, const void *buffer);

#ifdef __cplusplus
}
#endif
