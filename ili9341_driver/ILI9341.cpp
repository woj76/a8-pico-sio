#include "ILI9341.hpp"
#include <cstdio>
#include <cmath>
#include "hardware/gpio.h"
#include "hardware/pwm.h"
#include "hardware/dma.h"

#define BAUDRATE 40000000
#define GAMMA 2.8f

using namespace pimoroni;

namespace {

enum class Command : uint8_t {
    SWRESET   = 0x01, // Software Reset
    SLPOUT    = 0x11, // Sleep Out
    DISPON    = 0x29, // Display ON
    CASET     = 0x2A, // Column Address Set
    PASET     = 0x2B, // Page Address Set
    RAMWR     = 0x2C, // Memory Write
    MADCTL    = 0x36, // Memory Access Control
    COLMOD    = 0x3A, // Pixel Format Set
    FRMCTR1   = 0xB1, // Frame Rate Control (Normal Mode)
    DFUNCTR   = 0xB6, // Display Function Control
    PWCTR1    = 0xC0, // Power Control 1
    PWCTR2    = 0xC1, // Power Control 2
    VMCTR1    = 0xC5, // VCOM Control 1
    VMCTR2    = 0xC7, // VCOM Control 2
    PWCTRA    = 0xCB, // Power Control A (extended)
    PWCTRB    = 0xCF, // Power Control B (extended)
    DTCA      = 0xE8, // Driver Timing Control A (extended)
    DTCB      = 0xEA, // Driver Timing Control B (extended)
    PWRONSEQ  = 0xED, // Power On Sequence Control (extended)
    PUMPRATIO = 0xF7  // Pump Ratio Control (extended)
};

enum class Madctl : uint8_t {
    NONE = 0x00,
    MH   = 0x04, // Horizontal Refresh Order (right to left)
    BGR  = 0x08, // RGB-BGR Order (1 = BGR color filter panel)
    ML   = 0x10, // Vertical Refresh Order (bottom to top)
    MV   = 0x20, // Row / Column Exchange (swap X and Y)
    MX   = 0x40, // Column Address Order (mirror X)
    MY   = 0x80  // Row Address Order (mirror Y)
};

constexpr Madctl operator|(Madctl lhs, Madctl rhs) {
    return static_cast<Madctl>(
        static_cast<uint8_t>(lhs) | static_cast<uint8_t>(rhs)
    );
}

/* --------------- Internal Hardware / Low-Level Helpers --------------- */

void write_cmd(const SPIPins &pins, Command cmd) {
    uint8_t raw = static_cast<uint8_t>(cmd);
    gpio_put(pins.dc, 0);
    gpio_put(pins.cs, 0);
    spi_write_blocking(pins.spi, &raw, 1);
    gpio_put(pins.cs, 1);
}

void write_data(const SPIPins &pins, uint8_t data) {
    gpio_put(pins.dc, 1);
    gpio_put(pins.cs, 0);
    spi_write_blocking(pins.spi, &data, 1);
    gpio_put(pins.cs, 1);
}

void write_data(const SPIPins &pins, Madctl flags) {
    write_data(pins, static_cast<uint8_t>(flags));
}

void LCD_setPins(const SPIPins &pins) {
    pwm_config cfg = pwm_get_default_config();

    gpio_init(pins.dc);
    gpio_set_dir(pins.dc, GPIO_OUT);

    gpio_init(pins.cs);
    gpio_set_dir(pins.cs, GPIO_OUT);

    gpio_set_function(pins.sck, GPIO_FUNC_SPI);
    gpio_set_function(pins.mosi, GPIO_FUNC_SPI);

    pwm_set_wrap(pwm_gpio_to_slice_num(pins.bl), 65535);
    pwm_init(pwm_gpio_to_slice_num(pins.bl), &cfg, true);
    gpio_set_function(pins.bl, GPIO_FUNC_PWM);
    pwm_set_gpio_level(pins.bl, 0);
}

void LCD_setAddrWindow(const SPIPins &pins, uint16_t x0, uint16_t y0, uint16_t x1, uint16_t y1) {
    // Set Column Address (x0 to x1)
    write_cmd(pins, Command::CASET); // Column Address Set (defines X window bounds)
    write_data(pins, x0 >> 8);       // Start column high byte
    write_data(pins, x0 & 0xFF);     // Start column low byte
    write_data(pins, x1 >> 8);       // End column high byte
    write_data(pins, x1 & 0xFF);     // End column low byte

    // Set Page/Row Address (y0 to y1)
    write_cmd(pins, Command::PASET); // Page Address Set (defines Y window bounds)
    write_data(pins, y0 >> 8);       // Start row high byte
    write_data(pins, y0 & 0xFF);     // Start row low byte
    write_data(pins, y1 >> 8);       // End row high byte
    write_data(pins, y1 & 0xFF);     // End row low byte

    // Memory Write
    write_cmd(pins, Command::RAMWR); // Memory Write (begins transferring pixel data to GRAM)
}

void LCD_init(const SPIPins &pins, uint32_t baudrate) {
    spi_init(pins.spi, baudrate);

    sleep_ms(150);

    // Software Reset
    write_cmd(pins, Command::SWRESET); // Resets all registers to default
    sleep_ms(100);

    // Init
    write_cmd(pins, Command::PWCTRA); // Power Control A (extended manufacturer command)
    write_data(pins, 0x39); // Fixed parameter 1
    write_data(pins, 0x2C); // Fixed parameter 2
    write_data(pins, 0x00); // Fixed parameter 3
    write_data(pins, 0x34); // Vcore control: 1.6V
    write_data(pins, 0x02); // DDVDH control: 5.6V

    write_cmd(pins, Command::PWCTRB); // Power Control B (extended manufacturer command)
    write_data(pins, 0x00); // Fixed parameter
    write_data(pins, 0xC1); // Power control & discharge timing
    write_data(pins, 0x30); // Enable ESD protection (DC_ENA = 1)

    write_cmd(pins, Command::DTCA); // Driver Timing Control A (extended manufacturer command)
    write_data(pins, 0x85); // Gate driver non-overlap timing control
    write_data(pins, 0x00); // EQ timing control
    write_data(pins, 0x78); // Pre-charge timing control

    write_cmd(pins, Command::DTCB); // Driver Timing Control B (extended manufacturer command)
    write_data(pins, 0x00); // Gate driver timing control (0 unit)
    write_data(pins, 0x00); // Fixed parameter

    write_cmd(pins, Command::PWRONSEQ); // Power On Sequence Control (extended manufacturer command)
    write_data(pins, 0x64); // Soft start control (CP1/CP23 soft start keep 1 frame)
    write_data(pins, 0x03); // Power on sequence control (DDVDH/VCL/VGH/VGL order)
    write_data(pins, 0x12); // Power on sequence timing
    write_data(pins, 0x81); // Enable DDVDH enhance mode

    write_cmd(pins, Command::PUMPRATIO); // Pump Ratio Control (extended manufacturer command)
    write_data(pins, 0x20); // Ratio control: DDVDH = 2x VCI

    // Power Control 1
    write_cmd(pins, Command::PWCTR1); // Sets GVDD reference level
    write_data(pins, 0x23); // VRH[5:0] = 0x23 -> GVDD = 4.60V

    // Power Control 2
    write_cmd(pins, Command::PWCTR2); // Sets step-up circuit factor
    write_data(pins, 0x10); // BT[3:0] = 0x0 -> AVDD = 2xVCI, VGH = 7xVCI, VGL = -4xVCI

    // VCOM Control 1
    write_cmd(pins, Command::VMCTR1); // Sets VCOMH and VCOML voltages
    write_data(pins, 0x3E); // VMH[6:0] = 0x3E -> VCOMH = 4.250V
    write_data(pins, 0x28); // VML[6:0] = 0x28 -> VCOML = -1.500V

    // VCOM Control 2
    write_cmd(pins, Command::VMCTR2); // Sets VCOM offset voltage
    write_data(pins, 0x86); // nVM = 1 (enable VMF), VMF[6:0] = 0x06 -> VCOMH/L offset = -58

    // Memory Access Control (initial orientation)
    write_cmd(pins, Command::MADCTL); // Scan direction & RGB/BGR order
    write_data(pins, Madctl::MX | Madctl::BGR); // 0x48: Portrait 0° (mirror X, BGR filter)

    // Pixel Format Set (16-bit RGB565)
    write_cmd(pins, Command::COLMOD); // Interface pixel format
    write_data(pins, 0x55); // DPI = 16-bit/pixel, DBI = 16-bit/pixel (RGB565)

    // Frame Rate Control
    write_cmd(pins, Command::FRMCTR1); // Frame rate control in Normal Mode / Full Colors
    write_data(pins, 0x00); // DIVA[1:0] = 0 -> fosc division ratio = 1
    write_data(pins, 0x18); // RTNA[4:0] = 0x18 (24 clocks per line) -> ~79 Hz frame rate

    // Display Function Control
    write_cmd(pins, Command::DFUNCTR); // Display function and scan control
    write_data(pins, 0x08); // PTG[1:0] = 2 (interval scan), PT[1:0] = 0 (non-display area source output)
    write_data(pins, 0x82); // REV = 1 (normally white LCD), GS = 0, SS = 0, SM = 0, ISC = 2 (5 frames)
    write_data(pins, 0x27); // NL[5:0] = 0x27 -> 8 * (39 + 1) = 320 driven LCD lines

    // Exit Sleep
    write_cmd(pins, Command::SLPOUT); // Turns on internal DC/DC converter and oscillator
    sleep_ms(120);

    // Display ON
    write_cmd(pins, Command::DISPON); // Enables output from frame memory to panel
    sleep_ms(50);
}

void LCD_setRotation(const SPIPins &pins, Rotation rotation) {
    write_cmd(pins, Command::MADCTL); // Sets orientation and RGB/BGR order

    if (rotation == ROTATE_0) {
        // Landscape, SD slot down
        write_data(pins, Madctl::MV | Madctl::BGR); // 0x28: Landscape 90° (swap X/Y)
    } else if (rotation == ROTATE_90) {
        write_data(pins, Madctl::MY | Madctl::BGR); // 0x88: Portrait 180° (mirror Y)
    } else if (rotation == ROTATE_180) {
        write_data(pins, Madctl::MY | Madctl::MX | Madctl::MV | Madctl::BGR); // 0xE8: Landscape 270° (mirror X/Y + swap X/Y)
    } else { // rotation == ROTATE_270
        write_data(pins, Madctl::MX | Madctl::BGR); // 0x48: Portrait 0° (mirror X)
    }
}

void LCD_setBacklight(const SPIPins &pins, uint8_t brightness) {
    // pimoroni ST7789::set_backlight
    // gamma correct the provided 0-255 brightness value onto a
    // 0-65535 range for the pwm counter
    uint16_t value = static_cast<uint16_t>(std::pow(static_cast<float>(brightness) / 255.0f, GAMMA) * 65535.0f + 0.5f);
    pwm_set_gpio_level(pins.bl, value);
}

} // namespace

/* -------------------- ILI9341 Class Implementation -------------------- */

ILI9341::ILI9341(uint16_t width, uint16_t height, Rotation rotation, bool _ignored, SPIPins pins)
  : width(width), height(height), rotation(rotation), pins(pins) {
    LCD_setPins(pins);
    LCD_init(pins, BAUDRATE);
    LCD_setRotation(pins, rotation);

    // Claim AND configure DMA channel for SPI TX (just like Pimoroni ST7789)
    st_dma = dma_claim_unused_channel(true);
    dma_channel_config config = dma_channel_get_default_config(st_dma);
    channel_config_set_transfer_data_size(&config, DMA_SIZE_8);
    channel_config_set_bswap(&config, false);
    channel_config_set_dreq(&config, spi_get_dreq(pins.spi, true));
    dma_channel_configure(st_dma, &config, &spi_get_hw(pins.spi)->dr, NULL, 0, false);
}

void ILI9341::write_blocking_dma(const uint8_t *src, size_t len) {
    while (dma_channel_is_busy(st_dma))
        ;
    dma_channel_set_trans_count(st_dma, len, false);
    dma_channel_set_read_addr(st_dma, src, true);
}

void ILI9341::update(PicoGraphics *graphics) {
    LCD_setAddrWindow(pins, 0, 0, width - 1, height - 1);

    gpio_put(pins.dc, 1); // data mode
    gpio_put(pins.cs, 0); // select display for DMA transfer

    graphics->frame_convert(PicoGraphics::PEN_RGB565, [this](void *data, size_t length) {
        if (length > 0) {
            write_blocking_dma(static_cast<const uint8_t*>(data), length);
        } else {
            dma_channel_wait_for_finish_blocking(st_dma);
        }
    });

    // Wait for final SPI byte to finish shifting out before raising CS
    while (spi_is_busy(pins.spi))
        ;
    gpio_put(pins.cs, 1); // deselect display
}

void ILI9341::set_backlight(uint8_t brightness) {
    LCD_setBacklight(pins, brightness);
}
