// Useful links:
// https://forums.raspberrypi.com/viewtopic.php?t=349257
// https://github.com/TheMontezuma/SIO2BSD

// Config:
// Turbo system:
//   KSO 2000 J2
//   KSO 2000 SIO
//   ???
// HSIO divisor: 0-9

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
const std::string str_press_a_1 = "Press ";
const std::string str_press_a_2 = "A";
const std::string str_press_a_3 = " for";
const std::string str_press_a_4 = "USB drive...";
const std::string str_up_dir = "../";
const std::string str_more_files = "[Max files!]";
const std::string str_no_media = "No media!?";

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

Point text_location(str_x(str_press_a_4.length()), str_y(5)-4*font_scale);

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

bool repeating_timer_file_transfer(struct repeating_timer *t) {
	static int dy = 1;
	graphics.set_pen(BG); graphics.clear();
	text_location.y += dy;
	if(text_location.y == st7789.height-8*font_scale-1) dy = -1;
	if(text_location.y == 1) dy = 1;
	print_text(str_file_transfer);
	st7789.update(&graphics);
	return true;
}

std::string *ptr_str_file_name;

// TODO The scroll thing could be a C++ struct/class? like ProgressBar

const int scroll_fine_step = 4;
size_t scroll_length, scroll_size, scroll_index;
int scroll_x, scroll_y, scroll_d, scroll_fine;
std::string *scroll_ptr = nullptr;

void init_scroll_long_filename(std::string *s_ptr, int s_x, int s_y, size_t s_size, size_t s_length) {
	scroll_ptr = s_ptr;
	scroll_x = s_x;
	scroll_y = s_y;
	scroll_size = s_size;
	scroll_length = s_length;
	scroll_d = 1;
	scroll_index = 0;
	scroll_fine = -scroll_fine_step;
}

void scroll_long_filename() {
	if(scroll_ptr == nullptr) return;
	scroll_fine += scroll_d*scroll_fine_step;
	if(scroll_d > 0 && scroll_fine == 8*font_scale) {
		scroll_fine = 0;
		scroll_index += scroll_d;
		if(scroll_index + scroll_size == scroll_length) {
			scroll_d = -1;
			scroll_fine = 8*font_scale;
			scroll_index--;
		}
	}else if(scroll_d < 0 && scroll_fine == -scroll_fine_step) {
		scroll_fine = 8*font_scale-scroll_fine_step;
		scroll_index += scroll_d;
		if(scroll_index == -1) {
			scroll_d = 1;
			scroll_fine = -scroll_fine_step;
			scroll_index++;
		}
	}
	text_location.x = scroll_x - scroll_fine;
	text_location.y = scroll_y;
	graphics.set_clip(Rect(scroll_x,scroll_y,scroll_size*8*font_scale,8*font_scale));
	print_text((*scroll_ptr).substr(scroll_index,scroll_size+1),scroll_size+1);
	graphics.remove_clip();
	st7789.update(&graphics);
}

void usb_drive() {
	graphics.set_pen(BG); graphics.clear();

	text_location.x = str_x(str_file_transfer.length());
	text_location.y = str_y(1);
	print_text(str_file_transfer);
	st7789.update(&graphics);

	tud_init(BOARD_TUD_RHPORT);
	struct repeating_timer timer;
	add_repeating_timer_ms(200, repeating_timer_file_transfer, NULL, &timer);
	while (true) {
		tud_task();
		cdc_task();
	}
}

struct ProgressBar {
	int range;
	Rect progress_rect;
	const Rect clear_rect;
	const uint32_t progress_range;
	ProgressBar(const Point &p, int r, int s=0) :
			range(!r ? 1 : r),
			progress_rect(p.x+4*font_scale,p.y+4*font_scale,r ? s : 8*8*font_scale ,8*font_scale),
			clear_rect(p.x+4*font_scale,p.y+4*font_scale,st7789.width-2*p.x-8*font_scale,8*font_scale),
			progress_range(st7789.width-2*p.x-8*font_scale) {
		Rect rect(p.x, p.y-4*font_scale, progress_range+8*font_scale, 24*font_scale);
		if(!r) {
			graphics.set_pen(BG);
			graphics.rectangle(rect);
		}
		rect.y += 4*font_scale;
		rect.h -= 8*font_scale;
		graphics.set_pen(WHITE); graphics.rectangle(rect);
		rect.deflate(2*font_scale);
		graphics.set_pen(BG); graphics.rectangle(rect);
		if(r && s)
			update(s);
	}
	void update(uint32_t step) {
		if(range < 2)
			return;
		if (step > range) step = range;
		progress_rect.w = step*progress_range/range;
		graphics.set_pen(WHITE);
		graphics.rectangle(progress_rect);
	}
	void update() {
		if(range > 1)
			return;
		graphics.set_pen(BG); graphics.rectangle(clear_rect);
		if(range > 0 && progress_rect.x + progress_rect.w == clear_rect.x + clear_rect.w) {
			range = -1;
		}else if (range < 0 && progress_rect.x == clear_rect.x){
			range = 1;
		}
		progress_rect.x += range;
		graphics.set_pen(WHITE);
		graphics.rectangle(progress_rect);
	}
};

ProgressBar *loading_pg;

bool repeating_timer_directory(struct repeating_timer *t) {
	loading_pg->update();
	st7789.update(&graphics);
	return true;
}

char curr_path[256] = {0};
size_t num_files;

const size_t file_names_buffer_len = (256+13)*256;
char file_names[file_names_buffer_len]; // Guarantee to hold 256 full length long (255+1) and short (12+1) file names

typedef struct  {
	size_t short_name_index; // Use for file operations, MSB bit set = directory
	size_t long_name_index; // Use for displaying in file selection
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

size_t get_filename_ext(char *filename) {
	size_t l = strlen(filename);
	size_t i=0;
	while(i < l && filename[i++] != '.');
	return i;
}

enum file_type {none, disk, casette};
file_type ft = file_type::none;

bool is_valid_file(char *filename) {
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
					ret = 2;
					break;
				}
			}
			f_closedir(&dir);
		} else {
			ret = 0;
		}
		f_mount(0, "", 1);
		qsort((file_entry_type *)file_entries, num_files, sizeof(file_entry_type), file_entry_cmp);
	} else {
		ret = 0;
	}
	return ret;
}

int cursor_prev = -1;
int cursor_position = 1;

const std::string str_config = "Config...";
std::string str_d1 = "D1:  <EMPTY>   ";
std::string str_d2 = "D2:  <EMPTY>   ";
std::string str_d3 = "D3:  <EMPTY>   ";
std::string str_d4 = "D4:  <EMPTY>   ";
const std::string str_rot_up = "Rotate Up";
const std::string str_rot_down = "Rotate Down";
std::string str_cas = "C:  <EMPTY>   ";
const std::string str_rewind = "Rewind";

const int menu_to_mount[] = {-1,0,1,2,3,-1,-1,4,-1};
const file_type menu_to_type[] = {
	file_type::none,
	file_type::disk, file_type::disk, file_type::disk, file_type::disk,
	file_type::none, file_type::none,
	file_type::casette,
	file_type::none
};

typedef struct {
	std::string *str;
	char *mount_path;
	bool mounted;
} mounts_type;

char d1_mount[256] = {0};
char d2_mount[256] = {0};
char d3_mount[256] = {0};
char d4_mount[256] = {0};
char c_mount[256] = {0};

mounts_type mounts[] = {
	{.str=&str_d1, .mount_path=d1_mount, .mounted=false},
	{.str=&str_d2, .mount_path=d2_mount, .mounted=false},
	{.str=&str_d3, .mount_path=d3_mount, .mounted=false},
	{.str=&str_d4, .mount_path=d4_mount, .mounted=false},
	{.str=&str_cas, .mount_path=c_mount, .mounted=false}
};

typedef struct {
	std::string *str;
	int x, y;
	size_t wd;
} menu_entry;

const menu_entry menu_entries[] = {
	{.str = (std::string*)&str_config,.x=6*8*font_scale,.y=4*font_scale,.wd=str_config.length()},
	{.str = &str_d1,.x=3*8*font_scale,.y=(3*8-4)*font_scale,.wd=str_d1.length()},
	{.str = &str_d2,.x=3*8*font_scale,.y=(4*8-4)*font_scale,.wd=str_d2.length()},
	{.str = &str_d3,.x=3*8*font_scale,.y=(5*8-4)*font_scale,.wd=str_d3.length()},
	{.str = &str_d4,.x=3*8*font_scale,.y=(6*8-4)*font_scale,.wd=str_d4.length()},
	{.str = (std::string*)&str_rot_up,.x=6*8*font_scale,.y=(7*8)*font_scale,.wd=str_rot_up.length()},
	{.str = (std::string*)&str_rot_down,.x=5*8*font_scale,.y=(8*8)*font_scale,.wd=str_rot_down.length()},
	{.str = &str_cas,.x=4*8*font_scale,.y=(10*8)*font_scale,.wd=str_cas.length()},
	{.str = (std::string*)&str_rewind,.x=7*8*font_scale,.y=(13*8+4)*font_scale,.wd=str_rewind.length()},
};
const size_t menu_entry_size = 9;

typedef struct {
	std::string *str;
	int x, y;
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
		// TODO this needs to be global
		ProgressBar cas_pg(Point(2*8*font_scale,(11*8+2)*font_scale), 100, 15);
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
	int m = menu_to_mount[cursor_position];
	if(m != -1) {
		if(mounts[m].mounted)
			main_buttons[0].str = &char_eject;
		else if(mounts[m].mount_path[0])
			main_buttons[0].str = &char_inject;
		else
			main_buttons[0].str = &char_empty;
	}else{
		main_buttons[0].str = &char_empty;
	}
	update_buttons(main_buttons, cursor_prev == -1 ? main_buttons_size : 1);
	st7789.update(&graphics);
}

std::string str_fit(13,' ');

void fit_str(char *s, int sl, int l) {
	int h = l/2;
	int i;
	for(i=0; i < h; i++) str_fit[i] = s[i];
	str_fit[i++] = '~';
	int j = sl - h + 1;
	while(i<l) str_fit[i++] = s[j++];
	str_fit[i] = 0;
}

void update_one_display_file(int i, int fi) {
	char *f = &file_names[file_entries[fi].long_name_index];
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
		print_text((*ptr_str_file_name).substr(0,pe), pe);
		if(ei > pe) {
			init_scroll_long_filename(ptr_str_file_name, text_location.x, text_location.y, pe, ei);
			pe = 0;
		}
	} else {
		print_text(*ptr_str_file_name);
	}
	if(pe)
		delete ptr_str_file_name;
}

void update_display_files(int top_index, int shift_index) {
	scroll_length = 0;
	text_location.x = (3*8+4)*font_scale;
	if(cursor_prev == -1) {
		Rect r(2*8*font_scale,8*font_scale,16*8*font_scale,11*8*font_scale);
		graphics.set_pen(BG); graphics.rectangle(r);
		for(int i = 0; i<11; i++) {
			text_location.y = 8*(1+i)*font_scale;
			if(!i && !top_index && shift_index) {
				print_text(str_up_dir, i == cursor_position ? str_up_dir.length() : 0);
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
				print_text(str_up_dir, i == cursor_position ? str_up_dir.length() : 0);
			else
				update_one_display_file(i, top_index+i-shift_index);
			if(i == cursor_position)
				break;
			i = cursor_position;
		}
	}
}

int dir_browse_stack[128];
int dir_browse_stack_index=0;

void get_file(file_type t, int file_entry_index) {
	if(t != ft) {
		dir_browse_stack[dir_browse_stack_index] = 0;
		dir_browse_stack[dir_browse_stack_index+1] = 0;
	}
	ft = t;
	int saved_cursor_position = cursor_position;
	cursor_prev = -1;
	cursor_position = dir_browse_stack[dir_browse_stack_index];
	int top_index = dir_browse_stack[dir_browse_stack_index+1];

	main_buttons[0].str = &char_left;
	graphics.set_pen(BG); graphics.clear();

	while(true) {
		loading_pg = new ProgressBar(Point(2*8*font_scale,(6*8+4)*font_scale), 0);

		struct repeating_timer timer;
		add_repeating_timer_ms(1000/60, repeating_timer_directory, NULL, &timer);

		uint8_t r = read_directory();
		if(!r && curr_path[0]) {
			curr_path[0] = 0;
			r = read_directory();
		}
		// sleep_ms(1000); For testing
		cancel_repeating_timer(&timer);
		delete loading_pg;

		if(!r) {
			graphics.set_pen(BG); graphics.clear();
			text_location.x = str_x(str_no_media.length());
			text_location.y = 7*8*font_scale;
			print_text(str_no_media, str_no_media.length());
			update_buttons(nomedia_buttons, nomedia_buttons_size);
			st7789.update(&graphics);
			while(!button_b.read());
			dir_browse_stack_index = 0;
			dir_browse_stack[0] = 0;
			dir_browse_stack[1] = 0;
			goto get_file_exit;
		}

		int shift_index = curr_path[0] ? 1 : 0;
		update_buttons(main_buttons, main_buttons_size);
		if(r == 2) {
			text_location.x = str_x(str_more_files.length());
			text_location.y = 13*8*font_scale;
			print_text(str_more_files, str_more_files.length());
		}
		update_display_files(top_index, shift_index);
		st7789.update(&graphics);
		while(true) {
			if(button_y.read()) {
				if(top_index+cursor_position < num_files+shift_index-1) {
					if(cursor_position == 10) {
						cursor_prev = -1;
						cursor_position = 0;
						top_index += 11;
					} else {
						cursor_prev = cursor_position;
						cursor_position++;
					}
					delete scroll_ptr; scroll_ptr = nullptr;
					update_display_files(top_index, shift_index);
					st7789.update(&graphics);
				}
			}else if(button_x.read()) {
				if(top_index+cursor_position > 0) {
					if(cursor_position == 0) {
						cursor_prev = -1;
						cursor_position = 10;
						top_index -= 11;
					} else {
						cursor_prev = cursor_position;
						cursor_position--;
					}
					delete scroll_ptr; scroll_ptr = nullptr;
					update_display_files(top_index, shift_index);
					st7789.update(&graphics);
				}
			}else if(button_a.read()) {
				int fi = top_index+cursor_position-shift_index;
				int i = strlen(curr_path);
				delete scroll_ptr; scroll_ptr = nullptr;
				if(fi == -1) {
					dir_browse_stack_index -= 2;
					top_index = dir_browse_stack[dir_browse_stack_index+1];
					cursor_position = dir_browse_stack[dir_browse_stack_index];
					cursor_prev = -1;
					i -= 2;
					while(curr_path[i] != '/' && i) i--;
					curr_path[i] = 0;
					break;
				} else {
					char *f = &file_names[file_entries[fi].short_name_index & 0x7FFFFFFF];
					if(file_entries[fi].short_name_index & 0x80000000) {
						dir_browse_stack[dir_browse_stack_index] = cursor_position;
						dir_browse_stack[dir_browse_stack_index+1] = top_index;
						dir_browse_stack_index += 2;
						top_index = 0;
						cursor_prev = -1;
						cursor_position = 0;
						strcpy(&curr_path[i], f);
						break;
					} else {
						mounts[file_entry_index].mounted = true;
						strcpy(mounts[file_entry_index].mount_path, &curr_path[0]);
						strcpy(&(mounts[file_entry_index].mount_path[i]), f);
						i = 0;
						int si = ft == file_type::disk ? 3 : 2;
						while(i<12) {
							(*mounts[file_entry_index].str)[si+i] = (i < strlen(f) ? f[i] : ' ');
							i++;
						}
						goto get_file_exit;
					}
				}
			} else if(button_b.read()) {
				delete scroll_ptr; scroll_ptr = nullptr;
				goto get_file_exit;
			}
			sleep_ms(20);
			scroll_long_filename();
		}
	}
get_file_exit:
	dir_browse_stack[dir_browse_stack_index] = cursor_position;
	dir_browse_stack[dir_browse_stack_index+1] = top_index;
	cursor_position = saved_cursor_position;
	cursor_prev = -1;
	main_buttons[0].str = &char_eject;
}

int main() {
//	stdio_init_all();
	led.set_rgb(0, 0, 0);
	st7789.set_backlight(255);

	graphics.set_font(&atari_font);
	graphics.set_pen(BG); graphics.clear();

	if(!button_a.read()) {
		int x = text_location.x;
		print_text(str_press_a_1);
		text_location.x += str_press_a_1.length()*8*font_scale;
		print_text(str_press_a_2, 1);
		text_location.x += 8*font_scale;
		print_text(str_press_a_3);
		text_location.x = x;
		text_location.y += 12*font_scale;
		print_text(str_press_a_4);
		st7789.update(&graphics);
	}else
		usb_drive();

	ProgressBar pg(Point(text_location.x,text_location.y+16*font_scale), usb_boot_delay);
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

	while(true) {
		int d = menu_to_mount[cursor_position];
		if(button_x.read() && cursor_position > 0) {
			cursor_prev = cursor_position;
			cursor_position--;
			update_main_menu();
		}else if(button_y.read() && cursor_position < menu_entry_size-1) {
			cursor_prev = cursor_position;
			cursor_position++;
			update_main_menu();
		}else if(button_a.read()){
			if(d == -1) {
				// React to:
				// config, rotate up, rotate down, rewind
			}else{
				get_file(menu_to_type[cursor_position], d);
				update_main_menu();
			}

		}else if(button_b.read()) {
			if(d != -1) {
				if(mounts[d].mounted) {
					mounts[d].mounted = false;
				}else{
					if(mounts[d].mount_path[0])
						mounts[d].mounted = true;
				}
				update_main_menu();
			}
		}
		sleep_ms(20);
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
