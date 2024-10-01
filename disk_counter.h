#pragma once

#include "config.h"

#ifdef PIO_DISK_COUNTER

extern volatile uint32_t disk_counter;

void init_disk_counter();

#endif
