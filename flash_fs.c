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

#include "hardware/flash.h"
#include "hardware/sync.h"

#include <string.h>

#include "flash_fs.h"

#define MAGIC_8_BYTES "RHE!FS30"

typedef struct {
	uint8_t header[8];
	uint16_t sectors[NUM_FAT_SECTORS];  // map FAT sectors -> flash sectors
} sector_map;

sector_map fs_map;

bool fs_map_needs_written[MAP_ENTRIES];

uint8_t used_bitmap[NUM_FLASH_SECTORS]; // we will use 256 flash sectors for 2048 fat sectors

uint16_t write_sector = 0;   // which flash sector we are writing to
uint8_t write_sector_bitmap = 0;   // 1 for each free 512 byte page on the sector

uint16_t get_map_sector(uint16_t mapEntry) {
	return (mapEntry & 0xFFF8) >> 3;
}

uint8_t get_map_offset(uint16_t mapEntry) {
	return mapEntry & 0x7;
}

uint16_t make_map_entry(uint16_t sector, uint8_t offset) {
	return (sector << 3) | offset;
}

void flash_read_sector(uint16_t sector, uint8_t offset, void *buffer, uint16_t size);
void flash_erase_sector(uint16_t sector);
void flash_write_sector(uint16_t sector, uint8_t offset, const void *buffer, uint16_t size);
void flash_erase_with_copy_sector(uint16_t sector, uint8_t preserve_bitmap);

void write_fs_map() {
	for (int i=0; i<MAP_ENTRIES; i++) {
		if (fs_map_needs_written[i]) {
			flash_erase_sector(i);
			flash_write_sector(i, 0, (uint8_t*)&fs_map+(4096*i), 4096);
			fs_map_needs_written[i] = false;
		}
	}
}

uint16_t get_next_write_sector() {
	static uint16_t search_start_pos = 0;
	int i;
	if (write_sector == 0 || write_sector_bitmap == 0) {
		for (i=0; i<NUM_FLASH_SECTORS; i++)
			if (used_bitmap[(i + search_start_pos) % NUM_FLASH_SECTORS] == 0)
				break;
		if (i < NUM_FLASH_SECTORS) {
			write_sector = (i + search_start_pos) % NUM_FLASH_SECTORS;
			write_sector_bitmap = 0xFF;
			flash_erase_sector(write_sector);
		} else {
			for (i=0; i<NUM_FLASH_SECTORS; i++)
				if (used_bitmap[(i + search_start_pos) % NUM_FLASH_SECTORS] != 0xFF)
					break;
			write_sector = (i + search_start_pos) % NUM_FLASH_SECTORS;
			write_sector_bitmap = ~used_bitmap[write_sector];
			flash_erase_with_copy_sector(write_sector, used_bitmap[write_sector]);
		}
		search_start_pos = (i + search_start_pos) % NUM_FLASH_SECTORS;
	}

	for (i=0; i<8; i++)
		if (write_sector_bitmap & (1 << i))
			break;
	write_sector_bitmap &= ~(1 << i);
	return make_map_entry(write_sector, i);
}

void init_used_bitmap() {
	memset(used_bitmap, 0, NUM_FLASH_SECTORS);
	for (int i=0; i<MAP_ENTRIES; i++)
		used_bitmap[i] = 0xFF;

	for (int i=0; i<NUM_FAT_SECTORS; i++) {
		uint16_t mapEntry = fs_map.sectors[i];
		if (mapEntry)
			used_bitmap[get_map_sector(mapEntry)] |= (1 << get_map_offset(mapEntry));
	}
	write_sector = 0;
}

int flash_fs_mount() {
	for (int i=0; i<MAP_ENTRIES; i++)
		fs_map_needs_written[i] = false;

	flash_read_sector(0, 0, &fs_map, 4096);
	if (memcmp(fs_map.header, MAGIC_8_BYTES, 8) != 0)
		return 1;

	for (int i=1; i<MAP_ENTRIES; i++)
		flash_read_sector(i, 0, (uint8_t*)&fs_map+(4096*i), 4096);

	init_used_bitmap();
	return 0;
}

void flash_fs_create() {
	memset(&fs_map, 0, sizeof(fs_map));
	strcpy(fs_map.header, MAGIC_8_BYTES);
	for (int i=0; i<MAP_ENTRIES; i++)
		fs_map_needs_written[i] = true;
	write_fs_map();
	init_used_bitmap();
}

void flash_fs_sync() {
	write_fs_map();
}

void flash_fs_read_FAT_sector(uint16_t fat_sector, void *buffer) {
	int mapEntry = fs_map.sectors[fat_sector];
	if (mapEntry)
		flash_read_sector(get_map_sector(mapEntry), get_map_offset(mapEntry), buffer, 512);
	else
		memset(buffer, 0, 512);
}

void flash_fs_write_FAT_sector(uint16_t fat_sector, const void *buffer) {
	uint16_t mapEntry = fs_map.sectors[fat_sector];
	if (mapEntry)
		used_bitmap[get_map_sector(mapEntry)] &= ~(1 << get_map_offset(mapEntry));
	mapEntry = get_next_write_sector();
	fs_map.sectors[fat_sector] = mapEntry;
	if (fat_sector < 2044)
		fs_map_needs_written[0] = true;
	else
		fs_map_needs_written[1+((fat_sector-2044)/2048)] = true;
	used_bitmap[get_map_sector(mapEntry)] |= (1 << get_map_offset(mapEntry));
	flash_write_sector(get_map_sector(mapEntry), get_map_offset(mapEntry), buffer, 512);
}

bool flash_fs_verify_FAT_sector(uint16_t fat_sector, const void *buffer) {
	uint8_t read_buf[512];
	flash_fs_read_FAT_sector(fat_sector, read_buf);
	if (memcmp(buffer, read_buf, 512) == 0)
		return true;
	return false;
}

void flash_read_sector(uint16_t sector, uint8_t offset, void *buffer, uint16_t size) {
	uint32_t fs_start = XIP_BASE + HW_FLASH_STORAGE_BASE;
	uint32_t addr = fs_start + (sector * FLASH_SECTOR_SIZE) + (offset * 512);
	memcpy(buffer, (unsigned char *)addr, size);
}

void flash_erase_sector(uint16_t sector) {
	uint32_t fs_start = HW_FLASH_STORAGE_BASE;
	uint32_t offset = fs_start + (sector * FLASH_SECTOR_SIZE);
	uint32_t ints = save_and_disable_interrupts();
	flash_range_erase(offset, FLASH_SECTOR_SIZE);
	restore_interrupts(ints);
}

void flash_write_sector(uint16_t sector, uint8_t offset, const void *buffer, uint16_t size) {
	uint32_t fs_start = HW_FLASH_STORAGE_BASE;
	uint32_t addr = fs_start + (sector * FLASH_SECTOR_SIZE) + (offset * 512);
	uint32_t ints = save_and_disable_interrupts();
	flash_range_program(addr, (const uint8_t *)buffer, size);
	restore_interrupts(ints);
}

void flash_erase_with_copy_sector(uint16_t sector, uint8_t preserve_bitmap) {
	uint8_t buf[FLASH_SECTOR_SIZE];
	flash_read_sector(sector, 0, buf, FLASH_SECTOR_SIZE);
	flash_erase_sector(sector);
	for (int i=0; i<8; i++)
		if (preserve_bitmap & (1 << i))
			flash_write_sector(sector, i, buf + (i * 512), 512);
}
