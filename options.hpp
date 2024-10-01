#pragma once

#include <stdint.h>
#include "config.h"

#define option_count 8

extern uint8_t current_options[];

#define mount_option_index 0
#define clock_option_index 1
#define hsio_option_index 2
#define atx_option_index 3
#define xex_option_index 4
#define turbo1_option_index 5
#define turbo2_option_index 6
#define turbo3_option_index 7

extern const uint8_t *flash_config_pointer;
extern volatile bool save_config_flag;
extern volatile bool save_path_flag;

void check_and_load_config(bool reset_config);
void check_and_save_config();
