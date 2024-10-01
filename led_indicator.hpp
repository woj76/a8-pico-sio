#pragma once

#include "config.h"

extern volatile int8_t red_blinks;
extern volatile int8_t green_blinks;
extern volatile int8_t blue_blinks;

void update_rgb_led(bool from_interrupt);
void init_rgb_led();
