#include <string.h>
#include <cstdlib>

#include "pico/time.h"
#include "tusb.h"
#include "fatfs_disk.h"

#include "libraries/pico_display_2/pico_display_2.hpp"
#include "drivers/st7789/st7789.hpp"
#include "libraries/pico_graphics/pico_graphics.hpp"
#include "drivers/rgbled/rgbled.hpp"
#include "drivers/button/button.hpp"

#include "font_atari_data.hpp"

const uint32_t usb_boot_delay = 2000;
const uint8_t font_scale = 2;
const std::string str_file_transfer = "File transfer...";
const std::string str_press_a_1 = "Press 'A' for";
const std::string str_press_a_2 = "USB drive...";

using namespace pimoroni;

ST7789 st7789(320, 240, ROTATE_0, false, get_spi_pins(BG_SPI_FRONT));
PicoGraphics_PenP4 graphics(st7789.width, st7789.height, nullptr);

uint32_t inline str_x(uint32_t l) { return (st7789.width - l*8*font_scale)/2; }
uint32_t inline str_y(uint32_t h) { return (st7789.height - h*8*font_scale)/2; }

Point text_location(str_x(std::max(str_press_a_1.length(),str_press_a_2.length())), str_y(5));

RGBLED led(PicoDisplay2::LED_R, PicoDisplay2::LED_G, PicoDisplay2::LED_B, Polarity::ACTIVE_LOW, 0);

Button
	button_a(PicoDisplay2::A),
	button_b(PicoDisplay2::B),
	button_x(PicoDisplay2::X),
	button_y(PicoDisplay2::Y);

const Pen
	BG=graphics.create_pen(0, 0x5F, 0x8A),
	WHITE=graphics.create_pen(0x5D, 0xC1, 0xEC);

void print_text(const std::string_view &t) {
	graphics.text(t, text_location, st7789.width, font_scale, 0.0, 0, true);
}

void cdc_task(void) {
	if ( tud_cdc_available() ) {
		char buf[64];
		uint32_t count = tud_cdc_read(buf, sizeof(buf));
		(void) count;
		tud_cdc_write(buf, count);
		tud_cdc_write_flush();
	}
}

bool repeating_timer_callback(struct repeating_timer *t) {
	static uint32_t dy = 1;
	graphics.set_pen(BG); graphics.clear();
	graphics.set_pen(WHITE);
	text_location.y += dy;
	if(text_location.y == st7789.height-8*font_scale-1) dy = -1;
	if(text_location.y == 1) dy = 1;
	print_text(str_file_transfer);
	st7789.update(&graphics);
	return true;
}

void usb_drive() {
	graphics.set_pen(BG); graphics.clear(); graphics.set_pen(WHITE);

	text_location.x = str_x(str_file_transfer.length());
	text_location.y = str_y(1);
	print_text(str_file_transfer);
	st7789.update(&graphics);

	tud_init(BOARD_TUD_RHPORT);
	struct repeating_timer timer;
	add_repeating_timer_ms(200, repeating_timer_callback, NULL, &timer);
	while (true) {
		tud_task();
		cdc_task();
	}
}

struct ProgressBar {
	const uint32_t range;
	Rect progress_rect;
	const uint32_t progress_range;
	ProgressBar(const Point &p, const uint32_t r) :
			range(r),
			progress_rect(p.x+4*font_scale,p.y+4*font_scale,0,8*font_scale),
			progress_range(st7789.width-2*p.x-8*font_scale) {
		Rect rect(p.x, p.y, progress_range+8*font_scale, 16*font_scale);
		graphics.set_pen(WHITE); graphics.rectangle(rect);
		rect.deflate(2*font_scale);
		graphics.set_pen(BG); graphics.rectangle(rect);
	}
	void update(uint32_t step) {
		if (step > range) step = range;
		progress_rect.w = step*progress_range/range;
		graphics.set_pen(WHITE);
		graphics.rectangle(progress_rect);
	}
};

int main() {
//	stdio_init_all();
	led.set_rgb(0, 0, 0);
	st7789.set_backlight(255);

	graphics.set_font(&atari_font);
	graphics.set_pen(BG); graphics.clear();

	if(!button_a.read()) {
		graphics.set_pen(WHITE);
		print_text(str_press_a_1);
		text_location.y += 12*font_scale;
		print_text(str_press_a_2);
		st7789.update(&graphics);
	}else
		usb_drive();

	ProgressBar pg(Point(text_location.x,text_location.y+12*font_scale), usb_boot_delay);
	st7789.update(&graphics);
	uint32_t boot_time;
	do {
		boot_time = to_ms_since_boot(get_absolute_time());
		pg.update(boot_time);
		st7789.update(&graphics);
		if(button_a.read())
			usb_drive();
	}while(boot_time <= usb_boot_delay);

	graphics.set_pen(BG); graphics.clear();
	st7789.update(&graphics);

	while(true) {
		tight_loop_contents();
	}

	return 0;
}


void tud_mount_cb(void) {
	if (!mount_fatfs_disk())
		create_fatfs_disk();
}

void tud_umount_cb(void) {
}

void tud_suspend_cb(bool remote_wakeup_en) {
	(void) remote_wakeup_en;
}

void tud_resume_cb(void) {
}

void tud_cdc_line_state_cb(uint8_t itf, bool dtr, bool rts) {
	(void) itf;
	(void) rts;
	(void) dtr;
}

void tud_cdc_rx_cb(uint8_t itf) {
	(void) itf;
}
