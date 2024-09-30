#include <string.h>

#include "file_load.hpp"
#include "ff.h"

char curr_path[MAX_PATH_LEN];
size_t num_files, num_files_page;

char temp_array[MAX_PATH_LEN];
volatile int16_t create_new_file = 0;

size_t get_filename_ext(char *filename) {
	size_t l = strlen(filename);
	size_t i = l;
	while(i > 0 && filename[--i] != '.');
	return (i == 0 || l-i > 4) ? l : i+1;
}
