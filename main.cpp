// Useful links:
// https://forums.raspberrypi.com/viewtopic.php?t=349257
// https://github.com/TheMontezuma/SIO2BSD


#include <string.h>
#include <cstdlib>

#include "pico/time.h"
#include "tusb.h"
#include "fatfs_disk.h"
#include "ff.h"

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

TCHAR curr_path[256] = {0};
size_t num_files;

const size_t file_names_buffer_len = (256+13)*256;
TCHAR file_names[file_names_buffer_len]; // Guarantee to hold 256 full length long (255+1) and short (12+1) file names

typedef struct  {
	size_t short_name_index; // Always present, use for file operations, MSB bit set = directory
	size_t long_name_index; // Potentially present (if different from short), use for displaying
} file_entry_type;

const size_t file_entries_len = 2048;
file_entry_type file_entries[file_entries_len];

int file_entry_cmp(const void* p1, const void* p2) {
	file_entry_type* e1 = (file_entry_type*)p1; file_entry_type* e2 = (file_entry_type*)p2;
	size_t e1d = e1->short_name_index & 0x80000000; size_t e2d = e2->short_name_index & 0x80000000;
	if (e1d && !e2d) return -1;
	else if (!e1d && e2d) return 1;
	else return strcasecmp(&file_names[e1->long_name_index], &file_names[e2->long_name_index]);
}

size_t get_filename_ext(TCHAR *filename) {
	size_t l = strlen(filename);
	size_t i=0;
	while(i < l) {
		if(filename[i] == '.') {
			i++;
			break;
		}
		i++;
	}
	return i;
}

bool is_valid_cas_file(TCHAR *filename) {
	size_t i = get_filename_ext(filename);
	return strcasecmp(&filename[i], "CAS") == 0 || strcasecmp(&filename[i], "WAV") == 0;
}

size_t mark_directory(size_t is_directory_mask, size_t current_file_name_index) {
	if(is_directory_mask) {
		file_names[current_file_name_index-1] = '/';
		file_names[current_file_name_index] = 0;
		current_file_name_index++;
	}
	return current_file_name_index;
}

uint8_t read_directory() {
	FATFS FatFs; FILINFO fno; DIR dir;
	size_t current_file_name_index = 0;
	uint8_t ret = 1;

	num_files = 0;
	if (f_mount(&FatFs, "", 1) == FR_OK) {
		if (f_opendir(&dir, curr_path) == FR_OK) {
			while (true) {
				if (f_readdir(&dir, &fno) != FR_OK || fno.fname[0] == 0) break;
				if (fno.fattrib & (AM_HID | AM_SYS)) continue;
				size_t is_directory_mask = fno.fattrib & AM_DIR ? 0x80000000 : 0;
				if (!is_directory_mask && !is_valid_cas_file(fno.fname)) continue;
				if (fno.altname[0]) {
					size_t ls = strlen(fno.altname)+1;
					size_t ll = strlen(fno.fname)+1;
					if(current_file_name_index + ls + ll + (is_directory_mask ? 2 : 0) > file_names_buffer_len) {
						// TODO mark files left
						ret = 2;
						break;
					}
					strcpy(&file_names[current_file_name_index], fno.altname);
					file_entries[num_files].short_name_index = current_file_name_index | is_directory_mask;
					current_file_name_index += ls;
					current_file_name_index = mark_directory(is_directory_mask, current_file_name_index);
					strcpy(&file_names[current_file_name_index], fno.fname);
					file_entries[num_files].long_name_index = current_file_name_index;
					current_file_name_index += ll;
					current_file_name_index = mark_directory(is_directory_mask, current_file_name_index);
				}else{
					size_t ls = strlen(fno.fname)+1;
					if(current_file_name_index + ls + (is_directory_mask ? 1 : 0) > file_names_buffer_len) {
						// TODO mark files left
						ret = 2;
						break;
					}
					strcpy(&file_names[current_file_name_index], fno.fname);
					file_entries[num_files].long_name_index = current_file_name_index;
					file_entries[num_files].short_name_index = current_file_name_index | is_directory_mask;
					current_file_name_index += ls;
					current_file_name_index = mark_directory(is_directory_mask, current_file_name_index);
				}
				num_files++;
				if(num_files >= file_entries_len) {
					// TODO mark files left
					ret = 2;
					break;
				}
			}
			f_closedir(&dir);
		} else {
			// TODO mark no files
			ret = 0;
		}
		f_mount(0, "", 1);
		qsort((file_entry_type *)file_entries, num_files, sizeof(file_entry_type), file_entry_cmp);
	} else {
		// Mark no files
		ret = 0;
	}
	return ret;
}


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
		sleep_ms(20);
	}while(boot_time <= usb_boot_delay);

	graphics.set_pen(BG); graphics.clear();
	st7789.update(&graphics);

	tud_mount_cb();

	curr_path[0] = 0;
	read_directory(); // TODO display some animation
	text_location.x = 0;
	text_location.y = 0;
	graphics.set_pen(WHITE);
	for(int i=0;i<num_files;i++) {
		if(i>10)
			break;
		print_text(&file_names[file_entries[i].long_name_index]);
		text_location.y += 8*font_scale;
		st7789.update(&graphics);
	}

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
