#pragma once

#include "pico/time.h"
#include "ff.h"

extern const uint au_full_rotation;
extern const uint us_drive_request_delay;
extern uint8_t atx_track_size[];

bool loadAtxFile(FIL *fil, int atx_drive_number);
int8_t transferAtxSector(int atx_drive_number, uint16_t num, uint8_t *status, bool op_write = false, bool op_verify = false);
