// Useful links:
// https://forums.raspberrypi.com/viewtopic.php?t=349257
// https://github.com/TheMontezuma/SIO2BSD

// Screens 20x15 for 2-scale font


// Symbols needed: -> eject ^ V <-

// Config:
// Turbo system:
//   KSO 2000 J2
//   KSO 2000 SIO
//   ???
// HSIO divisor: 0-9

//

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

const uint32_t usb_boot_delay = 3000;
const uint8_t font_scale = 2;
const std::string str_file_transfer = "File transfer...";
const std::string str_press_a_1 = "Press 'A' for";
const std::string str_press_a_2 = "USB drive...";
const std::string str_up_dir = "../";
const std::string str_more_files = "[Max files!]";
std::string char_empty = " ";
std::string char_up = "!";
std::string char_down = "\"";
std::string char_left = "#";
std::string char_right = "$";
std::string char_inject = "%";
std::string char_eject = "&";

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

void print_text(const std::string_view &t, int inverse=0) {
	graphics.set_pen(WHITE);
	if(inverse) {
		Rect rect(text_location.x, text_location.y, inverse*font_scale*8, font_scale*8);
		graphics.rectangle(rect); graphics.set_pen(BG);
	}
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
	text_location.y += dy;
	if(text_location.y == st7789.height-8*font_scale-1) dy = -1;
	if(text_location.y == 1) dy = 1;
	print_text(str_file_transfer);
	st7789.update(&graphics);
	return true;
}

void usb_drive() {
	graphics.set_pen(BG); graphics.clear();

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

enum file_type {disk, casette};
file_type ft = file_type::disk;

bool is_valid_file(TCHAR *filename) {
	size_t i = get_filename_ext(filename);
	switch(ft) {
		case file_type::casette:
			return strcasecmp(&filename[i], "CAS") == 0 || strcasecmp(&filename[i], "WAV") == 0;
		case file_type::disk:
			return strcasecmp(&filename[i], "ATR") == 0 || strcasecmp(&filename[i], "ATX") == 0 || strcasecmp(&filename[i], "XEX") == 0;
		default:
			return false;
	}
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
				if (!is_directory_mask && !is_valid_file(fno.fname)) continue;
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

int8_t cursor_prev = -1;
int8_t cursor_position = 1;

const std::string str_config = "Config...";
std::string str_d1 = "D1:   <EMPTY>   ";
std::string str_d2 = "D2:   <EMPTY>   ";
std::string str_d3 = "D3:   <EMPTY>   ";
std::string str_d4 = "D4:   <EMPTY>   ";
const std::string str_rot_up = "Rotate Up";
const std::string str_rot_down = "Rotate Down";
std::string str_cas = "C:   <EMPTY>   ";
const std::string str_rewind = "Rewind";

typedef struct {
	std::string *str;
	int x;
	int y;
	size_t wd;
} menu_entry;

const menu_entry menu_entries[] = {
	{.str = (std::string*)&str_config,.x=6*8*font_scale,.y=4*font_scale,.wd=str_config.length()},
	{.str = &str_d1,.x=2*8*font_scale,.y=(3*8-4)*font_scale,.wd=str_d1.length()},
	{.str = &str_d2,.x=2*8*font_scale,.y=(4*8-4)*font_scale,.wd=str_d2.length()},
	{.str = &str_d3,.x=2*8*font_scale,.y=(5*8-4)*font_scale,.wd=str_d3.length()},
	{.str = &str_d4,.x=2*8*font_scale,.y=(6*8-4)*font_scale,.wd=str_d4.length()},
	{.str = (std::string*)&str_rot_up,.x=6*8*font_scale,.y=(7*8)*font_scale,.wd=str_rot_up.length()},
	{.str = (std::string*)&str_rot_down,.x=5*8*font_scale,.y=(8*8)*font_scale,.wd=str_rot_down.length()},
	{.str = &str_cas,.x=3*8*font_scale,.y=(10*8)*font_scale,.wd=str_cas.length()},
	{.str = (std::string*)&str_rewind,.x=7*8*font_scale,.y=(13*8+4)*font_scale,.wd=str_rewind.length()},
};
const size_t menu_entry_size = 9;

typedef struct {
	std::string *str;
	int x;
	int y;
} button_entry;

button_entry main_buttons[] {
	{.str = &char_eject,.x=font_scale,.y=(11*8-1)*font_scale},
	{.str = &char_right,.x=font_scale,.y=(3*8+1)*font_scale},
	{.str = &char_up,.x=st7789.width-(9*font_scale),.y=(3*8+1)*font_scale},
	{.str = &char_down,.x=st7789.width-(9*font_scale),.y=(11*8-1)*font_scale}
};
const size_t main_buttons_size = 4;

button_entry nomedia_buttons[] {
	{.str = &char_left,.x=font_scale,.y=(11*8-1)*font_scale}
};
const size_t nomedia_buttons_size = 1;


void update_buttons(button_entry menu_buttons[], const int num_buttons) {
	graphics.set_font(&symbol_font);
	for(int i=0; i<num_buttons;i++) {
		text_location.x = menu_buttons[i].x;
		text_location.y = menu_buttons[i].y;
		graphics.set_pen(BG);
		Rect r(text_location.x,text_location.y,8*font_scale,8*font_scale);
		graphics.rectangle(r);
		print_text(*(menu_buttons[i].str));
	}
	graphics.set_font(&atari_font);
}

void update_menu_entry(int i) {
	text_location.x = menu_entries[i].x;
	text_location.y = menu_entries[i].y;
	print_text(*(menu_entries[i].str), i==cursor_position ? menu_entries[i].wd : 0);
}

void update_main_menu() {
	if(cursor_prev == -1) {
		graphics.set_pen(BG);
		graphics.clear();
		for(int i=0; i<menu_entry_size;i++)
			update_menu_entry(i);
	}else{
		int i = cursor_prev;
		while(true) {
			Rect r(menu_entries[i].x,menu_entries[i].y,menu_entries[i].wd*8*font_scale,8*font_scale);
			graphics.set_pen(BG);
			graphics.rectangle(r);
			update_menu_entry(i);
			if(i == cursor_position)
				break;
			i = cursor_position;
		}

	}
	if((cursor_position >= 1 && cursor_position <=4) || cursor_position == 7) {
		main_buttons[0].str = &char_eject;
	}else{
		main_buttons[0].str = &char_empty;
	}
	update_buttons(main_buttons, main_buttons_size);
}


// selected files = curr_path + short_file_name of the selected file
// read them from save state?

/*
	curr_path[0] = 0;
	read_directory(); // TODO display some animation
	text_location.x = 0;
	text_location.y = 0;
	for(int i=0;i<num_files;i++) {
		if(i>10)
			break;
		print_text(&file_names[file_entries[i].long_name_index]);
		text_location.y += 8*font_scale;
		st7789.update(&graphics);
	}
*/

std::string str_fit = "             ";

void fit_str(TCHAR *s, int sl, int l) {
	// l is either 12 (6, 1, 5) or 8 (4, 1, 3)
	int h = l/2;
	int i;
	for(i=0; i < h; i++) str_fit[i] = s[i];
	str_fit[i++] = '~';
	int j = sl - h + 1;
	while(i<l) str_fit[i++] = s[j++];
	str_fit[i] = 0;
}

std::string *ptr_str_file_name;

void update_one_display_file(int i, int fi) {
	TCHAR *f = &file_names[file_entries[fi].long_name_index];
	// Directory xxxxxxxxxxxxxxxxxxxx/
	// Directory xxxxxxxxxxxxxxxxxx.ext/
	// Directory xxx/
	// Directory xxx.ext/
	// File name xxxxxxxxxxxxxxxxxxx
	// File name xxxxxxxxxxxxxxxxxxxxx.ext
	// File name xxxx
	// File name xxx.ext
	size_t ei = get_filename_ext(f) - 1;
	bool ext = (f[ei] == '.') ? true : false;
	int pe = 8;
	if (file_entries[fi].short_name_index & 0x80000000) {
		if(!ext) pe = 12;
		if(ei < pe) pe = ei;
		std::string s2(&f[ei]);
		text_location.x += pe*8*font_scale;
		print_text(s2, i == cursor_position ? s2.length() : 0);
		text_location.x -= pe*8*font_scale;
	} else {
		text_location.x += 8*8*font_scale;
		if(ext) {
			print_text(std::string(&f[ei]), i == cursor_position ? 4 : 0);
		}else{
			if(i == cursor_position)
				print_text("    ", 4);
			ei++;
		}
		text_location.x -= 8*8*font_scale;
	}
	if(i != cursor_position && ei > pe) {
		fit_str(f, ei, pe);
		ptr_str_file_name = &str_fit;
		ei = pe;
		pe = 0;
	}else{
		ptr_str_file_name = new std::string(f, ei);
	}
	if(i == cursor_position) {
		// TODO do scrolling thing
		print_text(*ptr_str_file_name, ptr_str_file_name->length());
	} else {
		print_text(*ptr_str_file_name);
	}
	if(pe)
		delete ptr_str_file_name;
}

void update_display_files(int top_index) {
	int shift_index = curr_path[0] ? 1 : 0;
	text_location.x = (3*8+4)*font_scale;
	if(cursor_prev == -1) {
		Rect r(text_location.x,8*font_scale,13*8*font_scale,11*8*font_scale);
		graphics.set_pen(BG); graphics.rectangle(r);
		for(int i = 0; i<11; i++) {
			text_location.y = 8*(1+i)*font_scale;
			if(!i && !top_index && shift_index) {
				print_text(str_up_dir, i == cursor_position ? 13 : 0);
			} else {
				int fi = top_index+i-shift_index;
				if(fi >= num_files)
					break;
				update_one_display_file(i,fi);
			}
		}
	}else{
		int i = cursor_prev;
		while(true) {
			text_location.y = 8*(1+i)*font_scale;
			Rect r(text_location.x,text_location.y,13*8*font_scale,8*font_scale);
			graphics.set_pen(BG); graphics.rectangle(r);
			if(!i && !top_index && shift_index)
				print_text(str_up_dir, i == cursor_position ? 13 : 0);
			else
				update_one_display_file(i, top_index+i-shift_index);
			if(i == cursor_position)
				break;
			i = cursor_position;
		}
	}
}

void get_file(file_type t, int file_entry_index) {
	static uint8_t get_file_cursor_position = 0;
	static int top_index = 0;
	ft = t;
	uint8_t saved_cursor_position = cursor_position;
	cursor_position = get_file_cursor_position;
	cursor_prev = -1;

	main_buttons[0].str = &char_left;

	// clean up screen
	graphics.set_pen(BG); graphics.clear();
	// display buttons

	// Below do this in the loop, break only once window closed or file selected
	// setup screen, initiate infinite progress bar (implement it)

	// on action button reset get_file_cursor_position to 0
	uint8_t r = read_directory();
	if(!r && curr_path[0]) {
		curr_path[0] = 0;
		r = read_directory();
	}
	// Kill the progress bar

	graphics.set_pen(WHITE);

	if(!r) {
		// no media! only allow to close, no other buttons,
		// leave just one button
		// TODO

		update_buttons(nomedia_buttons, nomedia_buttons_size);
		st7789.update(&graphics);
		while(!button_b.read())
			tight_loop_contents();
	}else{
		update_buttons(main_buttons, main_buttons_size);
		if(r == 2) {
			text_location.x = (20-str_more_files.length())*4*font_scale;
			text_location.y = 13*8*font_scale;
			print_text(str_more_files, str_more_files.length());
		}
		update_display_files(top_index);
		st7789.update(&graphics);
		// 1 all available files are in the list
		// 2 not all files are in the list
		// if curr_path[0] != 0 the first entry is the up dir
		// if 2 display info at the bottom that there are undisplayed files
		// in inverse Max file limit!
	}
	cursor_position = saved_cursor_position;
	main_buttons[0].str = &char_eject;
	// restore buttons
}

int main() {
//	stdio_init_all();
	led.set_rgb(0, 0, 0);
	st7789.set_backlight(255);

	graphics.set_font(&atari_font);
	graphics.set_pen(BG); graphics.clear();

	if(!button_a.read()) {
		print_text(str_press_a_1, str_press_a_1.length());
		text_location.y += 12*font_scale;
		print_text(str_press_a_2, str_press_a_2.length());
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
		sleep_ms(1000/60);
	}while(boot_time <= usb_boot_delay);

	tud_mount_cb();


	update_main_menu();
	ProgressBar cas_pg(Point(2*8*font_scale,(11*8+2)*font_scale), 100);
	st7789.update(&graphics);

	get_file(file_type::casette, 5);

	while(true)
	  tight_loop_contents();


/*
	while(true) {
		if(button_x.read() && cursor_position > 0) {
			cursor_prev = cursor_position;
			cursor_position--;
			update_main_menu();
			st7789.update(&graphics);
		}else if(button_y.read() && cursor_position < menu_entry_size-1) {
			cursor_prev = cursor_position;
			cursor_position++;
			update_main_menu();
			st7789.update(&graphics);
		}
	}
*/
	return 0;
}



// main
// **********************
// *                    *
// *>     Config...    ^*
// *                    *
// *  D1:   <EMPTY>     *
// *  D2:   <EMPTY>     *
// *  D3:x12345678.910  *
// *  D4:x12345678.910  *
// *       .5           *
// *     Rotate Up      *
// *    Rotate Down     *
// *                    *
// *   C:x12345679.910  *
// *   |||||||||||||||  *
// *   |||||||||||||||  *
// *E     Rewind       V*
// *                    *
// **********************


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
