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
