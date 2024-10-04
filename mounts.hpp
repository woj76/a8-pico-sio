#pragma once

#include "pico/multicore.h"

#include "config.h"

#include "ff.h"

typedef struct {
	char *str;
	char *mount_path;
	bool mounted;
	FSIZE_t status;
} mounts_type;

typedef struct __attribute__((__packed__)) {
	uint16_t magic;
	uint16_t pars;
	uint16_t sec_size;
	uint8_t pars_high;
	uint32_t crc;
	uint8_t temp1, temp2, temp3, temp4;
	uint8_t flags;
} atr_header_type;

typedef struct __attribute__((__packed__)) {
	uint32_t signature;
	uint16_t chunk_length;
	union {
		uint8_t aux_b[2];
		uint16_t aux_w;
	} aux;
} cas_header_type;

typedef union {
	atr_header_type atr_header;
	uint8_t data[sizeof(atr_header_type)];
} disk_header_type;

enum file_type {none, disk, casette};
extern file_type ft;

extern char str_d1[];
extern char str_d2[];
extern char str_d3[];
extern char str_d4[];
extern char str_cas[];

extern mounts_type mounts[];
extern disk_header_type disk_headers[];

#define disk_type_atr 1
#define disk_type_xex 2
#define disk_type_atx 3

#define sector_buffer_size 768
extern uint8_t sector_buffer[];

extern cas_header_type cas_header;

extern uint8_t pwm_bit_order;
extern uint8_t pwm_bit;

extern uint32_t pwm_sample_duration;
extern uint32_t cas_sample_duration;
extern uint16_t silence_duration;
extern uint16_t cas_block_index;
extern uint16_t cas_block_multiple;
extern uint8_t cas_fsk_bit;

extern volatile FSIZE_t cas_size;

extern FATFS fatfs[];

extern mutex_t fs_lock;
extern mutex_t mount_lock;

void init_locks();

#define cas_header_FUJI 0x494A5546
#define cas_header_baud 0x64756162
#define cas_header_data 0x61746164
#define cas_header_fsk  0x206B7366
#define cas_header_pwms 0x736D7770
#define cas_header_pwmc 0x636D7770
#define cas_header_pwmd 0x646D7770
#define cas_header_pwml 0x6C6D7770

void get_drive_label(int i);

uint8_t try_mount_sd();

void mount_file(char *f, int drive_number, char *lfn);

FRESULT mounted_file_transfer(int drive_number, FSIZE_t offset, FSIZE_t to_transfer, bool op_write, size_t t_offset=0, FSIZE_t brpt=1);

FSIZE_t cas_read_forward(FIL *fil, FSIZE_t offset);

extern volatile uint8_t sd_card_present;
extern const char * const volume_names[];

extern const char * const str_int_flash;
extern const char * const str_sd_card;

extern char volume_labels[][14];

extern volatile int last_drive;
extern volatile uint32_t last_drive_access;

void update_last_drive(uint drive_number);

extern int last_access_error_drive;
extern bool last_access_error[];

void set_last_access_error(int drive_number);
