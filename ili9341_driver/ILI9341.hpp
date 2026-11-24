#pragma once

#include <cstdint>
#include "common/pimoroni_bus.hpp"
#include "libraries/pico_graphics/pico_graphics.hpp"

using namespace pimoroni;

class ILI9341 {
public:
    ILI9341(uint16_t width, uint16_t height, Rotation rotation, bool round, pimoroni::SPIPins pins);
    void update(pimoroni::PicoGraphics *graphics_ctx);
    void set_backlight(uint8_t brightness);

    uint16_t width;
    uint16_t height;

private:
    void write_blocking_dma(const uint8_t *src, size_t len);
    
    uint st_dma;
    Rotation rotation;
    pimoroni::SPIPins pins;
};
