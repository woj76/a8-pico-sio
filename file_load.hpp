#pragma once

#include <ctype.h>
#include <stdint.h>

#define MAX_PATH_LEN 512

extern char curr_path[];
extern size_t num_files;
extern size_t num_files_page;

extern char temp_array[];
extern volatile int16_t create_new_file;

size_t get_filename_ext(char *filename);
