#pragma once

#include <ctype.h>
#include <stdint.h>
#include "config.h"

#define MAX_PATH_LEN 512

extern char curr_path[];
extern size_t num_files;
extern size_t num_files_page;

extern char temp_array[];
extern volatile int16_t create_new_file;

typedef struct  __attribute__((__packed__)) {
	bool dir;
	char short_name[15];
	char long_name[258];
	uint16_t dir_index;
	bool last_file;
} file_entry_type;

#define max_files_per_page 12

extern file_entry_type file_entries[];

extern char last_file_name[];

size_t get_filename_ext(char *filename);

int32_t read_directory(int32_t page_index, int page_size);
int16_t create_new_disk_image();
