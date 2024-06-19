#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include <stdbool.h>

#define HW_FLASH_STORAGE_BASE  (1024 * 1024)
#define BOARD_SIZE 2 // in MB
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
