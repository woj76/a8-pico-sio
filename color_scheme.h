#pragma once

#include "config.h"

#ifdef ZAXON_DEVICE

    #define INITIAL_COLORS_BG 0xFF, 0xFF, 0xFF
    #define INITIAL_COLORS_WHITE 0x5B, 0x5B, 0x5B

    #define MENU_COLORS_NTSC_BG 0xFF, 0xBD, 0x4F
    #define MENU_COLORS_NTSC_WHITE 0xA2, 0x53, 0x00
    #define MENU_COLORS_NTSC_GREEN 0xD7, 0x4A, 0xDF
    #define MENU_COLORS_NTSC_RED 0x6C, 0xEC, 0xFD

    #define MENU_COLORS_PAL_BG 0xFF, 0x99, 0x89
    #define MENU_COLORS_PAL_WHITE 0xA7, 0x37, 0x27
    #define MENU_COLORS_PAL_GREEN 0x9C, 0x50, 0xFF
    #define MENU_COLORS_PAL_RED 0x69, 0xD8, 0xE9

#else

    #define INITIAL_COLORS_BG 0x00, 0x00, 0x00
    #define INITIAL_COLORS_WHITE 0xA4, 0xA4, 0xA4

    #define MENU_COLORS_NTSC_BG 0x00, 0x42, 0xB0 // Atari $94
    #define MENU_COLORS_NTSC_WHITE 0x5D, 0xAC, 0xFF // Atari $9A
    #define MENU_COLORS_NTSC_GREEN 0x28, 0xB5, 0x20 // Atari $C8
    #define MENU_COLORS_NTSC_RED 0x93, 0x13, 0x02// Atari $34

    #define MENU_COLORS_PAL_BG 0x00, 0x66, 0x76 // Atari $94
    #define MENU_COLORS_PAL_WHITE 0x58, 0xC8, 0xD8 // Atari $9A
    #define MENU_COLORS_PAL_GREEN 0x63, 0xAF, 0x00 // Atari $C8
    #define MENU_COLORS_PAL_RED 0x96, 0x27, 0x16 // Atari $24

    #ifdef INCREASE_MENU_CONTRAST
        #undef MENU_COLORS_PAL_BG
        #define MENU_COLORS_PAL_BG 0x00, 0x33, 0x3B
        #undef MENU_COLORS_PAL_RED
        #define MENU_COLORS_PAL_RED 0xff, 0x42, 0x25
    #endif

#endif
