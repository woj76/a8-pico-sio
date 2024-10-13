/*
 * This file is part of the a8-pico-sio project --
 * An Atari 8-bit SIO drive and (turbo) tape emulator for
 * Raspberry Pi Pico, see
 *
 *         https://github.com/woj76/a8-pico-sio
 *
 * For information on what / whose work it is based on, check below and the
 * corresponding project repository.
 */

/* See atx.cpp for credits and other info. */

#pragma once

#include "config.h"

#include "ff.h"

extern const uint au_full_rotation;
extern const uint us_drive_request_delay;
extern uint8_t atx_track_size[];

bool loadAtxFile(FIL *fil, int atx_drive_number);
int8_t transferAtxSector(int atx_drive_number, uint16_t num, uint8_t *status, bool op_write = false, bool op_verify = false);
