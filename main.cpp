/*
 * This file is part of the a8-pico-sio project --
 * An Atari 8-bit SIO drive and (turbo) tape emulator for
 * Raspberry Pi Pico, see
 *
 *         https://github.com/woj76/a8-pico-sio
 *
 * For information on what / whose work it is based on, check the corresponding
 * source files and the README file. This file is licensed under GNU General
 * Public License 3.0 or later.
 *
 * Copyright (C) 2024 Wojciech Mostowski <wojciech.mostowski@gmail.com>
 */

/*
Some useful links:

https://forums.raspberrypi.com/viewtopic.php?t=349257
https://github.com/TheMontezuma/SIO2BSD
https://forums.atariage.com/topic/290397-atari-sio-transmission-oscillogram/
https://www.atarimax.com/jindroush.atari.org/asio.html
https://github.com/HiassofT/highspeed-sio/blob/master/README.txt
https://tnt23.livejournal.com/605184.html
http://ftp.pigwa.net/stuff/collections/nir_dary_cds/Tech%20Info/The%20SIO%20protocol%20description/sio.html
https://allpinouts.org/pinouts/connectors/serial/atari-8-bit-serial-input-output-sio/
https://forums.atariage.com/topic/271215-why-the-k-on-boot/?do=findComment&comment=4209492
http://www.whizzosoftware.com/sio2arduino/vapi.html
https://github.com/carlk3/no-OS-FatFS-SD-SPI-RPi-Pico
https://www.a8preservation.com/#/guides/atx
https://forums.atariage.com/topic/282759-databyte-disks-on-atari-810/?do=findComment&comment=4112899
*/

#include "config.h"

#ifdef CORE1_PRIORITY
#include "hardware/regs/busctrl.h"
#include "hardware/structs/bus_ctrl.h"
#endif

#include "tusb.h"
#include "fatfs_disk.h"
#include "ff.h"

#include "libraries/pico_display_2/pico_display_2.hpp"
#include "drivers/st7789/st7789.hpp"
#include "libraries/pico_graphics/pico_graphics.hpp"
#include "drivers/button/button.hpp"

#include "led_indicator.hpp"
#include "mounts.hpp"
#include "file_load.hpp"
#include "io.hpp"
#include "options.hpp"
#include "wav_decode.hpp"
#include "sio.hpp"

#include "font_atari_data.hpp"

// In ms, time given to the user to press A/B during device boot
#define usb_boot_delay 3000

// TODO hard code this in? There is no use for font_scale == 1 really
#define font_scale 2

constexpr std::string_view str_file_transfer{"File transfer..."};
constexpr std::string_view str_press_1{"USB drive"};
constexpr std::string_view str_press_2{"Config reset"};

constexpr std::string_view str_up_dir{"../"};
constexpr std::string_view str_new_image{"New image... ->"};

constexpr std::string_view str_no_files{"[No files!]"};
constexpr std::string_view str_no_media{"No media!?"};
constexpr std::string_view str_config2{"Config"};
constexpr std::string_view str_creating{" Creating... "};
constexpr std::string_view str_create_failed{"Create failed!"};

constexpr std::string_view str_about1{"A8 Pico SIO"};
constexpr std::string_view str_about2{"by woj@AtariAge"};
constexpr std::string_view str_about3{"(c) 2025"};
constexpr std::string_view str_about4{"Inspired by and"};
constexpr std::string_view str_about5{"based on code of"};
constexpr std::string_view str_about6{"A8PicoCart"};
constexpr std::string_view str_about7{"SIO2BSD"};
constexpr std::string_view str_about8{"SDrive-MAX"};
constexpr std::string_view str_about9{"Altirra"};
constexpr std::string_view str_about10{"EclaireXL"};
constexpr std::string_view str_about11{"Version 0.92"};

constexpr std::string_view char_empty{" "};
constexpr std::string_view char_up{"!"};
constexpr std::string_view char_down{"\""};
constexpr std::string_view char_left{"#"};
constexpr std::string_view char_right{"$"};
constexpr std::string_view char_inject{"%"};
constexpr std::string_view char_eject{"&"};
constexpr std::string_view char_play{"'"};
constexpr std::string_view char_stop{"("};

using namespace pimoroni;

ST7789 st7789(320, 240, ROTATE_0, false, get_spi_pins(BG_SPI_FRONT));
PicoGraphics_PenP4 graphics(st7789.width, st7789.height, nullptr);

uint32_t inline str_x(uint32_t l) { return (st7789.width - l*8*font_scale)/2; }
uint32_t inline str_y(uint32_t h) { return (st7789.height - h*8*font_scale)/2; }

Point text_location(str_x(str_press_2.size()+2), str_y(5)-4*font_scale);

#ifdef RELOCATE_BUTTON_PINS
// Relocated button pins to free the SPI1 interface for the SD card
Button button_a(0), button_b(1), button_x(2), button_y(3);
#else
Button button_a(PicoDisplay2::A), button_b(PicoDisplay2::B), button_x(PicoDisplay2::X), button_y(PicoDisplay2::Y);
#endif

const Pen
	BG=graphics.create_pen(0x00, 0x00, 0x00), WHITE=graphics.create_pen(0xA4, 0xA4, 0xA4),
	GREEN=graphics.create_pen(0x00, 0x00, 0x00), RED=graphics.create_pen(0x00, 0x00, 0x00);


void update_colors() {
	if(current_options[clock_option_index]) {
		// NTSC
		// Atari $94
		graphics.update_pen(BG, 0x00, 0x42, 0xB0);
		// Atari $9A
		graphics.update_pen(WHITE, 0x5D, 0xAC, 0xFF);
		// Atari $C8
		graphics.update_pen(GREEN, 0x28, 0xB5, 0x20);
		// Atari $34
		graphics.update_pen(RED, 0x93, 0x13, 0x02);
	}else{
		// PAL
		// Atari $94
		graphics.update_pen(BG, 0x00, 0x66, 0x76);
		// Atari $9A
		graphics.update_pen(WHITE, 0x58, 0xC8, 0xD8);
		// Atari $C8
		graphics.update_pen(GREEN, 0x63, 0xAF, 0x00);
		// Atari $24
		graphics.update_pen(RED, 0x96, 0x27, 0x16);
	}
}

void print_text(const std::string_view &t, int inverse=0) {
	graphics.set_pen(WHITE);
	if(inverse) {
		Rect rect(text_location.x, text_location.y, inverse*font_scale*8, font_scale*8);
		graphics.rectangle(rect); graphics.set_pen(BG);
	}
	graphics.text(t, text_location, st7789.width, font_scale, 0.0, 0, true);
}

void print_text_wait(const std::string_view &t) {
	red_blinks = 6;
	text_location.x = str_x(t.size());
	text_location.y = (14*8-4)*font_scale;
	print_text(t, t.size());
	st7789.update(&graphics);
	sleep_ms(2000);
	Rect r(text_location.x,text_location.y,8*t.size(),8*font_scale);
	graphics.set_pen(BG); graphics.rectangle(r);
	st7789.update(&graphics);
}

void cdc_task(void) {
	if ( tud_cdc_available() ) {
		char buf[64];
		uint32_t count = tud_cdc_read(buf, sizeof(buf));
		//(void) count;
		tud_cdc_write(buf, count);
		tud_cdc_write_flush();
	}
}

/**
  Sliding "File transfer..." sign for the USB mode.
*/
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

/** USB drive mode, no exit from this. */
void usb_drive() {
	graphics.set_pen(BG); graphics.clear();

	text_location.x = str_x(str_file_transfer.size());
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

#define scroll_fine_step 4
size_t scroll_length, scroll_size, scroll_index;
int scroll_x, scroll_y, scroll_d, scroll_fine;

std::string_view *ptr_str_file_name;
std::string_view *scroll_ptr = nullptr;

void init_scroll_long_filename(std::string_view *s_ptr, int s_x, int s_y, size_t s_size, size_t s_length) {
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

struct ProgressBar {
	const bool with_range;
	const Rect clear_rect;
	const uint16_t width;
	Rect progress_rect;
	int dir;
	ProgressBar(uint16_t w, int y, bool r) :
			progress_rect((st7789.width - w)/2,y+4*font_scale,r ? 0 : 8*8*font_scale ,8*font_scale),
			clear_rect((st7789.width - w)/2,y+4*font_scale,w,8*font_scale),
			with_range(r),
			dir(r ? 0 : 4),
			width(w) {
	}
	void init() {
		Rect rect((st7789.width - width - 2*8*font_scale)/2, progress_rect.y-8*font_scale, width+2*8*font_scale, 24*font_scale);
		if(!with_range) {
			graphics.set_pen(BG);
			graphics.rectangle(rect);
		}
		rect.deflate(4*font_scale);
		graphics.set_pen(WHITE); graphics.rectangle(rect);
		rect.deflate(2*font_scale);
		graphics.set_pen(BG); graphics.rectangle(rect);
		if(with_range && progress_rect.w)
			update(progress_rect.w);
	}
	void update(uint16_t step) {
		if(!with_range)
			return;
		if(step > width) step = width;
		if(step < progress_rect.w) {
			graphics.set_pen(BG);
			graphics.rectangle(progress_rect);
		}
		progress_rect.w = step;
		graphics.set_pen(WHITE);
		graphics.rectangle(progress_rect);
	}
	void update() {
		if(with_range)
			return;
		graphics.set_pen(BG);
		graphics.rectangle(clear_rect);
		if(dir > 0 && progress_rect.x + progress_rect.w == clear_rect.x + clear_rect.w) {
			dir = -4;
		}else if (dir < 0 && progress_rect.x == clear_rect.x){
			dir = 4;
		}
		progress_rect.x += dir;
		graphics.set_pen(WHITE);
		graphics.rectangle(progress_rect);
	}
};

ProgressBar loading_pg(192,(6*8+4)*font_scale, false);

bool repeating_timer_directory(struct repeating_timer *t) {
	loading_pg.update();
	st7789.update(&graphics);
	return true;
}

const std::string_view option_names[] = {
	"Mount RdWr:",
	"Host Clock:",
	"HSIO Speed:",
	"ATX Floppy:",
	"XEX Loader:",
	"Turbo Data:",
	"Turbo When:",
	"PWM Invert:",
	"WAV Output:",
	"   >> Save <<   "
};

typedef struct {
	const int count;
	const char* const *short_names;
	const char* const *long_names;
} option_list;

const char * const mount_option_names_short[] = {"  OFF", "   ON"};
const char * const mount_option_names_long[] = {"Read-only", "Read/Write"};
const char * const clock_option_names_short[] = {"  PAL", " NTSC"};
const char * const clock_option_names_long[] = {"PAL at 1.77MHz", "NTSC at 1.79MHz"};
const char * const hsio_option_names_short[] = {"  $28", "  $10", "   $6", "   $5","   $4", "   $3","   $2", "   $1", "   $0"};
const char * const hsio_option_names_long[] = {"$28 OFF/Standard", "$10 ~39 kbit/s", " $6 ~68 kbit/s", " $5 ~74 kbit/s"," $4 ~81 kbit/s", " $3 ~90 kbit/s", " $2 ~99 kbit/s", " $1 ~111 kbit/s", " $0 ~127 kbit/s"};
const char * const atx_option_names_short[] = {" 1050", "  810"};
const char * const atx_option_names_long[] = {"Atari 1050", "Atari 810"};
const char * const xex_option_names_short[] = {" $500", " $600", " $700", " $800", " $900", " $A00"};
const char * const xex_option_names_long[] = {"Loader at $500", "Loader at $600", "Loader at $700", "Loader at $800", "Loader at $900", "Loader at $A00"};
const char * const turbo1_option_names_short[] = {"  SIO", " J2P4", " PROC", "  INT", " J2P1"};
const char * const turbo1_option_names_long[] = {"SIO Data In", "Joy2 Port Pin 4", "SIO Proceed", "SIO Interrupt", "Joy2 Port Pin 1"};
const char * const turbo2_option_names_short[] = {" COMM", " J2P2", "  SIO", " NONE"};
const char * const turbo2_option_names_long[] = {"SIO Command", "Joy2 Port Pin 2", "SIO Data Out", "None / Motor"};
const char * const turbo3_option_names_long[] = {"Normal", "Inverted"};
const char * const wav_option_names_short[] = {"  FSK", "  PWM"};
const char * const wav_option_names_long[] = {"Regular FSK", "Turbo PWM"};

const option_list option_lists[] = {
	{
		.count = 2,
		.short_names = mount_option_names_short,
		.long_names = mount_option_names_long
	},
	{
		.count = 2,
		.short_names = clock_option_names_short,
		.long_names = clock_option_names_long
	},
	{
		.count = 9,
		.short_names = hsio_option_names_short,
		.long_names = hsio_option_names_long
	},
	{
		.count = 2,
		.short_names = atx_option_names_short,
		.long_names = atx_option_names_long
	},
	{
		.count = 6,
		.short_names = xex_option_names_short,
		.long_names = xex_option_names_long
	},
	{
		.count = 5,
		.short_names = turbo1_option_names_short,
		.long_names = turbo1_option_names_long
	},
	{
		.count = 4,
		.short_names = turbo2_option_names_short,
		.long_names = turbo2_option_names_long
	},
	{
		.count = 2,
		.short_names = mount_option_names_short,
		.long_names = turbo3_option_names_long
	},
	{
		.count = 2,
		.short_names = wav_option_names_short,
		.long_names = wav_option_names_long
	}
};

int cursor_prev = -1;
int cursor_position = 1;

const char * const str_config = "Config...";
const char * const str_rot_up = "Rotate Up";
const char * const str_rot_down = "Rotate Down";
const char * const str_about = "About...";

const int menu_to_mount[] = {-1,1,2,3,4,-1,-1,0,-1};

const file_type menu_to_type[] = {
	file_type::none, // Config
	file_type::disk, file_type::disk, file_type::disk, file_type::disk, // the 4 Dx: disk drives
	file_type::none, file_type::none, // Rotate buttons
	file_type::casette, // the C: cassette device
	file_type::none // About
};

//char txt_buf[25] = {0};

typedef struct {
	char *str;
	int x, y;
	size_t wd;
} menu_entry;

const menu_entry menu_entries[] = {
	{.str = (char *)str_config,.x=6*8*font_scale,.y=4*font_scale,.wd=strlen(str_config)},
	{.str = str_d1,.x=3*8*font_scale,.y=(3*8-4)*font_scale,.wd=strlen(str_d1)},
	{.str = str_d2,.x=3*8*font_scale,.y=(4*8-4)*font_scale,.wd=strlen(str_d2)},
	{.str = str_d3,.x=3*8*font_scale,.y=(5*8-4)*font_scale,.wd=strlen(str_d3)},
	{.str = str_d4,.x=3*8*font_scale,.y=(6*8-4)*font_scale,.wd=strlen(str_d4)},
	{.str = (char *)str_rot_up,.x=6*8*font_scale,.y=(7*8)*font_scale,.wd=strlen(str_rot_up)},
	{.str = (char *)str_rot_down,.x=5*8*font_scale,.y=(8*8)*font_scale,.wd=strlen(str_rot_down)},
	{.str = str_cas,.x=3*8*font_scale,.y=(10*8)*font_scale,.wd=strlen(str_cas)},
	{.str = (char *)str_about,.x=(6*8+4)*font_scale,.y=(13*8+4)*font_scale,.wd=strlen(str_about)}
};

#define menu_entry_size 9

const uint mount_to_menu[] = {7,1,2,3,4};

typedef struct {
	const std::string_view *str;
	int x, y;
} button_entry;

button_entry main_buttons[] {
	{.str = &char_eject,.x=font_scale,.y=(11*8-1)*font_scale},
	{.str = &char_right,.x=font_scale,.y=(3*8+1)*font_scale},
	{.str = &char_up,.x=st7789.width-(9*font_scale),.y=(3*8+1)*font_scale},
	{.str = &char_down,.x=st7789.width-(9*font_scale),.y=(11*8-1)*font_scale}
};
#define main_buttons_size 4

button_entry nomedia_buttons[] {
	{.str = &char_left,.x=font_scale,.y=(11*8-1)*font_scale}
};
#define nomedia_buttons_size 1

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
	print_text(menu_entries[i].str, i==cursor_position ? menu_entries[i].wd : 0);
}

#define cas_pg_width 208
ProgressBar cas_pg(cas_pg_width,(11*8+2)*font_scale, true);

void update_main_menu_buttons() {
	int m = menu_to_mount[cursor_position];
	if(m != -1) {
		if(mounts[m].mounted)
			main_buttons[0].str = m ? &char_eject : &char_stop;
		else if(mounts[m].mount_path[0])
			main_buttons[0].str = m ? &char_inject : &char_play;
		else
			main_buttons[0].str = &char_empty;
	}else
		main_buttons[0].str = &char_empty;
	update_buttons(main_buttons, cursor_prev == -1 ? main_buttons_size : 1);
}

void update_main_menu() {
	if(cursor_prev == -1) {
		graphics.set_pen(BG);
		graphics.clear();
		for(int i=0; i<menu_entry_size;i++)
			update_menu_entry(i);
		cas_pg.init();
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
}

static char str_fit[17];

void fit_str(const char s[], int sl, int l) {
	int h = l/2;
	strncpy(&str_fit[0], &s[0], h);
	str_fit[h] = '~';
	strncpy(&str_fit[h+1], &s[sl-h+1-(l&1)], l-h-1);
	str_fit[l] = 0;
}

void update_one_display_file(int i, int fi) {
	char *f = file_entries[fi].long_name;
	size_t ei;
	int pe;
	if (file_entries[fi].dir) {
		ei = strlen(f) - 1;
		pe = 16;
		if(ei < pe) pe = ei;
		text_location.x += pe*8*font_scale;
		print_text(std::string_view("/"), i == cursor_position ? 1 : 0);
		text_location.x -= pe*8*font_scale;
	} else {
		ei = get_filename_ext(f) - 1;
		pe = 13;
		text_location.x += pe*8*font_scale;
		print_text(std::string_view(&f[ei]), i == cursor_position ? 4 : 0);
		text_location.x -= pe*8*font_scale;
	}
	if(i != cursor_position && ei > pe) {
		fit_str(f, ei, pe);
		ei = pe;
		ptr_str_file_name = new std::string_view(str_fit, ei);
		pe = 0;
	}else
		ptr_str_file_name = new std::string_view(f, ei);
	if(i == cursor_position) {
		print_text((*ptr_str_file_name).substr(0,pe), pe);
		if(ei > pe) {
			init_scroll_long_filename(ptr_str_file_name, text_location.x, text_location.y, pe, ei);
			pe = 0;
		}
	}else
		print_text(*ptr_str_file_name);
	if(pe)
		delete ptr_str_file_name;
}

void update_one_display_file2(int page_index, int shift_index, int i) {
	if(!curr_path[0]) {
		print_text(std::string_view(volume_labels[i]), (i == cursor_position) ? 16 : 0);
	} else if(!page_index && i < shift_index) {
		bool new_image_label = (!i && ft == file_type::disk);
		const std::string_view& s = new_image_label ? str_new_image : str_up_dir;
		if(new_image_label) {
			text_location.y -= 8*font_scale;
			Rect r(text_location.x,text_location.y,17*8*font_scale,8*font_scale);
			graphics.set_pen(BG); graphics.rectangle(r);
		}
		print_text(s, (i == cursor_position) ? s.size() : 0);
	}else
		update_one_display_file(i, i-shift_index);
}

void update_display_files(int page_index, int shift_index) {
	scroll_length = 0;
	text_location.x = (8+4)*font_scale;
	if(cursor_prev == -1) {
		Rect r(text_location.x,8*font_scale,17*8*font_scale,(num_files ? max_files_per_page+1 : 2)*8*font_scale);
		graphics.set_pen(BG); graphics.rectangle(r);
		for(int i = 0; i < num_files_page; i++) {
			text_location.y = 8*(2+i)*font_scale;
			update_one_display_file2(page_index, shift_index, i);
		}
	}else{
		int i = cursor_prev;
		while(true) {
			text_location.y = 8*(2+i)*font_scale;
			Rect r(text_location.x,text_location.y,17*8*font_scale,8*font_scale);
			graphics.set_pen(BG); graphics.rectangle(r);
			update_one_display_file2(page_index, shift_index, i);
			if(i == cursor_position)
				break;
			i = cursor_position;
		}
	}
}

constexpr std::string_view new_image_sd{" SD 90K"};
constexpr std::string_view new_image_ed{" ED 130K"};
constexpr std::string_view new_image_dd{" DD 180K"};
constexpr std::string_view new_image_qd{" QD 360K"};

constexpr std::string_view new_image_none{" None"};
constexpr std::string_view new_image_dos{" DOS 2.x"};
constexpr std::string_view new_image_mydos{" MyDOS"};
constexpr std::string_view new_image_sparta{" SpartaDOS"};
constexpr std::string_view new_image_yes{" Yes"};
constexpr std::string_view new_image_no{" No"};

const std::string_view *new_file_options_0[] = { &new_image_sd, &new_image_ed, &new_image_dd, &new_image_qd };
const std::string_view *new_file_options_1[] = { &new_image_none, &new_image_dos, &new_image_mydos, &new_image_sparta };
const std::string_view *new_file_options_2[] = { &new_image_none, &new_image_mydos, &new_image_sparta };
const std::string_view *new_file_options_3[] = { &new_image_yes, &new_image_no };

const uint8_t new_file_options_2_vals[] = {1, 3, 4};

constexpr std::string_view new_image_title_0{"Size?"};
constexpr std::string_view new_image_title_1{"Format?"};
constexpr std::string_view new_image_title_2{"Dummy boot?"};

const std::string_view *new_file_titles[] = { &new_image_title_0, &new_image_title_1, &new_image_title_1, &new_image_title_2 };

void update_one_new_file_option(int i, int opt_level) {
	print_text(!opt_level ? *(new_file_options_0[i])  : (opt_level == 1 ? *(new_file_options_1[i]) : (opt_level == 3 ? *(new_file_options_3[i]) : *(new_file_options_2[i]))) , i == cursor_position ? 12 : 0);
}

void update_new_file_options(int opt_level) {
	text_location.x = (4*8)*font_scale;
	if(cursor_prev == -1) {
		Rect r(4*8*font_scale, (4*8+4)*font_scale,12*8*font_scale,4*12*font_scale);
		graphics.set_pen(BG); graphics.rectangle(r);
		for(int i = 0; i < (opt_level == 2 ? 3 : (opt_level == 3 ? 2 : 4)); i++) {
			text_location.y = (4*8+4+12*i)*font_scale;
			update_one_new_file_option(i, opt_level);
		}
	}else{
		int i = cursor_prev;
		while(true) {
			text_location.y = (4*8+4+12*i)*font_scale;
			Rect r(text_location.x,text_location.y,12*8*font_scale,8*font_scale);
			graphics.set_pen(BG); graphics.rectangle(r);
			update_one_new_file_option(i, opt_level);
			if(i == cursor_position)
				break;
			i = cursor_position;
		}
	}
}


int16_t select_new_file_options(int selections, int opt_level) {
	cursor_position = 0;
	cursor_prev = -1;
	text_location.x = (4*8)*font_scale;
	text_location.y = 8*3*font_scale;
	Rect r(text_location.x,text_location.y,12*8*font_scale,8*font_scale);
	graphics.set_pen(BG); graphics.rectangle(r);
	print_text(*(new_file_titles[opt_level]));

	update_new_file_options(opt_level);
	st7789.update(&graphics);

	while(true) {
		if(button_y.read()) {
			if(cursor_position < (opt_level == 2 ? 2 : (opt_level == 3 ? 1 : 3))) {
				cursor_prev = cursor_position;
				cursor_position++;
				update_new_file_options(opt_level);
				st7789.update(&graphics);
			}
		}else if(button_x.read()) {
			if(cursor_position > 0) {
				cursor_prev = cursor_position;
				cursor_position--;
				update_new_file_options(opt_level);
				st7789.update(&graphics);
			}
		}else if(button_a.read()) {
			int li;
			switch(opt_level) {
				case 0: selections = (cursor_position+1); return select_new_file_options(selections, (cursor_position < 3) ? 1 : 2);
				case 1: selections |= ((cursor_position+1) & 0xF) << 4; li = 3; break;
				case 2: selections |= (new_file_options_2_vals[cursor_position] & 0xF) << 4; li = 2; break;
				case 3: selections |= (cursor_position == 0) ? 0x100 : 0; return selections;
				default: break;
			}
			if(cursor_position > 0 && cursor_position < li)
				return select_new_file_options(selections, 3);
			else
				return selections;
		} else if(button_b.read())
			return 0;
		sleep_ms(20);
	}
}

void get_file(int file_entry_index) {
	struct repeating_timer loading_pg_timer;
	int saved_cursor_position = cursor_position;

	cursor_prev = -1;
	cursor_position = (ft == file_type::disk && curr_path[0]) ? 1 : 0;
	int page_index = 0;

	main_buttons[0].str = &char_left;
	uint8_t last_sd_card_file = sd_card_present;

	while(true) {
		// Show a progress slider in case it takes a while to read (it probably would for very
		// large directories).
		loading_pg.init();
		add_repeating_timer_ms(1000/60, repeating_timer_directory, NULL, &loading_pg_timer);
		int32_t r = 0;
		// f_closedir(&dir);
		if(curr_path[0]) {
			mutex_enter_blocking(&fs_lock);
			if(f_opendir(&dir, curr_path) != FR_OK)
				r = -1;
			mutex_exit(&fs_lock);
		}
		if(r >= 0) {
			r = read_directory(-1, 0);
			if(r < 0 && curr_path[0]) {
				f_closedir(&dir);
				//if(sd_card_present)
				curr_path[0] = 0;
				cursor_position = 0;
				//else
				//	strcpy(curr_path, volume_names[0]);
				last_file_name[0] = 0;
				r = read_directory(-1, 0);
			}
		}
		cancel_repeating_timer(&loading_pg_timer);
		graphics.set_pen(BG); graphics.clear();
		if(r < 0) {
			// I really do not see how could this situation occur having the
			// internal FLASH drive always availalbe, but just to be on the safe side...
			text_location.x = str_x(str_no_media.size());
			text_location.y = 7*8*font_scale;
			print_text(str_no_media, str_no_media.size());
			update_buttons(nomedia_buttons, nomedia_buttons_size);
			st7789.update(&graphics);
			while(!button_b.read())
				tight_loop_contents();
			goto get_file_exit;
		}
		num_files = r;
		int shift_index = (curr_path[0] ? 1 : 0) + (ft == file_type::disk && curr_path[0] ? 1 : 0);
		update_buttons(main_buttons, main_buttons_size);

		if(!num_files) {
			text_location.x = str_x(str_no_files.size());
			text_location.y = 7*8*font_scale;
			print_text(str_no_files, str_no_files.size());
		}
		int num_pages = (num_files+shift_index+max_files_per_page-1) / max_files_per_page;
		int last_page = (num_files+shift_index) % max_files_per_page;
		if(!last_page)
			last_page = max_files_per_page;
		num_files_page = (page_index == num_pages-1) ? last_page : max_files_per_page;
		if(num_files) {
			loading_pg.init();
			add_repeating_timer_ms(1000/60, repeating_timer_directory, NULL, &loading_pg_timer);
			while(page_index < num_pages) {
				int act_num_files_page = page_index ? num_files_page : (num_files_page-shift_index);
				read_directory(page_index, act_num_files_page);
				if(!last_file_name[0]) break;
				bool file_found = false;
				for(cursor_position=0; cursor_position < act_num_files_page; cursor_position++) {
					if(file_entries[cursor_position].last_file) {
						if(!page_index) cursor_position += shift_index;
						file_found = true;
						last_file_name[0] = 0;
						break;
					}
				}
				if(file_found) break;
				page_index++;
				num_files_page = (page_index == num_pages-1) ? last_page : max_files_per_page;
			}
			cancel_repeating_timer(&loading_pg_timer);
		}
		if(num_files || shift_index)
			update_display_files(page_index, page_index ? 0 : shift_index);
		int y1 = main_buttons[2].y+10*font_scale, y2 = main_buttons[3].y-2*font_scale;
		int scroll_block_len = (y2-y1) / (num_pages ? num_pages : 1);
		Rect scroll_bar(st7789.width - 7*font_scale, y1, 4*font_scale, y2-y1);
		graphics.set_pen(BG); graphics.rectangle(scroll_bar);
		scroll_bar.y += scroll_block_len*page_index;
		scroll_bar.h = scroll_block_len;
		graphics.set_pen(WHITE); graphics.rectangle(scroll_bar);
		st7789.update(&graphics);
		while(true) {
			if(sd_card_present != last_sd_card_file) {
				last_sd_card_file = sd_card_present;
				if((!sd_card_present && curr_path[0] == '1') || !curr_path[0]) {
					if(curr_path[0])
						f_closedir(&dir);
					delete scroll_ptr; scroll_ptr = nullptr;
					page_index = 0;
					cursor_prev = -1;
					cursor_position = 0;
					curr_path[0] = 0;
					last_file_name[0] = 0;
					break;
				}
			}
			if(num_pages && button_y.read()) {
				if(cursor_position < num_files_page-1 || page_index < num_pages-1) {
					if(cursor_position == max_files_per_page-1 && page_index < num_pages-1) {
						cursor_prev = -1;
						cursor_position = 0;
						page_index++;
						num_files_page = (page_index == num_pages-1) ? last_page : max_files_per_page;
						loading_pg.init();
						add_repeating_timer_ms(1000/60, repeating_timer_directory, NULL, &loading_pg_timer);
						read_directory(page_index, num_files_page);
						cancel_repeating_timer(&loading_pg_timer);
						graphics.set_pen(BG); graphics.rectangle(scroll_bar);
						scroll_bar.y += scroll_block_len;
						graphics.set_pen(WHITE); graphics.rectangle(scroll_bar);
					} else {
						cursor_prev = cursor_position;
						cursor_position++;
					}
					delete scroll_ptr; scroll_ptr = nullptr;
					update_display_files(page_index, page_index ? 0 : shift_index);
					st7789.update(&graphics);
				}
			}else if(num_pages && button_x.read()) {
				if(cursor_position > 0 || page_index > 0) {
					if(cursor_position == 0 && page_index > 0) {
						cursor_prev = -1;
						cursor_position = max_files_per_page-1;
						page_index--;
						num_files_page = max_files_per_page;
						loading_pg.init();
						add_repeating_timer_ms(1000/60, repeating_timer_directory, NULL, &loading_pg_timer);
						read_directory(page_index, page_index ? num_files_page : num_files_page-shift_index);
						cancel_repeating_timer(&loading_pg_timer);
						graphics.set_pen(BG); graphics.rectangle(scroll_bar);
						scroll_bar.y -= scroll_block_len;
						graphics.set_pen(WHITE); graphics.rectangle(scroll_bar);
					} else {
						cursor_prev = cursor_position;
						cursor_position--;
					}
					delete scroll_ptr; scroll_ptr = nullptr;
					update_display_files(page_index, page_index ? 0 : shift_index);
					st7789.update(&graphics);
				}
			}else if(num_pages && button_a.read()) {
				f_closedir(&dir);
				int fi = cursor_position - (page_index ? 0 : shift_index);
				int i = strlen(curr_path);
				delete scroll_ptr; scroll_ptr = nullptr;
				if(fi < 0) {
					if(cursor_position == (ft == file_type::disk ? 1 : 0)) {
						page_index = 0;
						cursor_prev = -1;
						i--;
						while(curr_path[i-1] != '/' && curr_path[i-1] != ':' && i > 0) i--;
						strcpy(last_file_name, &curr_path[i]);
						curr_path[i] = 0;
						// cursor_position = (ft == file_type::disk && curr_path[0]) ? 1 : 0;
						break;
					} else {
						FILINFO fil_info;
						int fn = 0;
						mutex_enter_blocking(&fs_lock);
						//if(f_mount(&fatfs[0], curr_path, 1) == FR_OK) {
							for(; fn < 10000; fn++) {
								sprintf(&curr_path[i], "DISK%04d.ATR", fn);
								if(f_stat(curr_path, &fil_info) != FR_OK) {
									strcpy(temp_array, curr_path);
									fn = 10000;
								}
							}
						//}
						//f_mount(0, curr_path, 1);
						mutex_exit(&fs_lock);
						if(fn == 10001) {
							graphics.set_pen(BG); graphics.rectangle(scroll_bar);
							Rect r((8+4)*font_scale,8*font_scale,17*8*font_scale,(max_files_per_page+1)*8*font_scale);
							graphics.rectangle(r);
							char *f = &curr_path[i];
							text_location.x = 4*8*font_scale;
							text_location.y = 8*font_scale;
							print_text(std::string_view(f));
							int16_t cnf = select_new_file_options(0, 0);
							if(cnf) {
								text_location.x = str_x(str_creating.size());
								text_location.y = (14*8-4)*font_scale;
								print_text(str_creating, str_creating.size());
								st7789.update(&graphics);
								create_new_file = cnf;
								while(create_new_file > 0) tight_loop_contents();
								Rect r2(text_location.x,text_location.y,8*str_creating.size(),8*font_scale);
								graphics.set_pen(BG); graphics.rectangle(r2);
								st7789.update(&graphics);
								if(!create_new_file) {
									strcpy(last_file_name, f);
									mount_file(f, file_entry_index, f);
									//if(!mount_file(f, file_entry_index))
									//	red_blinks = 2;

								} else
									print_text_wait(str_create_failed);
							}
							//else
								// Cancelled
							//	create_new_file = -2;
						} else
							print_text_wait(str_create_failed);
						//else
							// No more files possible
						//	create_new_file = -3;
						curr_path[i] = 0;
						// if(create_new_file >= 0)
						goto get_file_exit;
						// cursor_prev = -1;
					}
				} else {
					char *f = file_entries[fi].short_name;
					if(file_entries[fi].dir) {
						page_index = 0;
						cursor_prev = -1;
						strcpy(&curr_path[i], f);
						last_file_name[0] = 0;
						cursor_position = (ft == file_type::disk && curr_path[0]) ? 1 : 0;
						break;
					} else {
						strcpy(&curr_path[i], f);
						strcpy(last_file_name, f);
						mount_file(f, file_entry_index, file_entries[fi].long_name);
						//if(!mount_file(f, file_entry_index))
						//	red_blinks = 2;
						curr_path[i] = 0;
						goto get_file_exit;
					}
				}
			} else if(button_b.read()) {
				f_closedir(&dir);
				int fi = cursor_position - (page_index ? 0 : shift_index);
				if(fi >= 0)
					strcpy(last_file_name, file_entries[fi].short_name);
				delete scroll_ptr; scroll_ptr = nullptr;
				goto get_file_exit;
			}
			sleep_ms(20);
			scroll_long_filename();
		}
	}
get_file_exit:
	cursor_position = saved_cursor_position;
	cursor_prev = -1;
	main_buttons[0].str = &char_eject;
}

void update_selection_entry(const char **opt_names, int i, bool erase) {
	text_location.x = 2*8*font_scale;
	text_location.y = (2+(2+i)*10)*font_scale;
	if(erase) {
		Rect r(text_location.x,text_location.y,16*8*font_scale,8*font_scale);
		graphics.set_pen(BG); graphics.rectangle(r);
	}
	print_text(std::string_view(opt_names[i]), i==cursor_position ? 16 : 0);
}

void update_selections(const char **opt_names, int opt_count) {
	if(cursor_prev == -1) {
		graphics.set_pen(BG);
		graphics.clear();
		for(int i=0; i<opt_count;i++)
			update_selection_entry(opt_names, i, false);
	}else{
		int i = cursor_prev;
		while(true) {
			update_selection_entry(opt_names, i, true);
			if(i == cursor_position)
				break;
			i = cursor_position;
		}
	}
}

void select_option(int opt_num) {
	int saved_cursor_position = cursor_position;
	cursor_prev = -1;
	cursor_position = current_options[opt_num];

	const char **opt_names = (const char **)option_lists[opt_num].long_names;
	int opt_count = option_lists[opt_num].count;
	update_selections(opt_names, opt_count);

	text_location.x = str_x(option_names[opt_num].size());
	text_location.y = 4*font_scale;
	print_text(option_names[opt_num]);
	Rect r(text_location.x,text_location.y+10*font_scale,option_names[opt_num].size()*8*font_scale,2*font_scale);
	graphics.set_pen(WHITE); graphics.rectangle(r);

	main_buttons[0].str = &char_left;
	update_buttons(main_buttons, main_buttons_size);

	while(true) {
		int d = menu_to_mount[cursor_position];
		if(button_x.read() && cursor_position > 0) {
			cursor_prev = cursor_position;
			cursor_position--;
			update_selections(opt_names, opt_count);
		}else if(button_y.read() && cursor_position < opt_count-1) {
			cursor_prev = cursor_position;
			cursor_position++;
			update_selections(opt_names, opt_count);
		}else if(button_b.read()) {
			break;
		}else if(button_a.read()) {
			current_options[opt_num] = cursor_position;
			break;
		}
		st7789.update(&graphics);
		sleep_ms(20);
	}
	cursor_position = saved_cursor_position;
	cursor_prev = -1;
}

void update_options_entry(int i, bool erase) {
	text_location.x = 2*8*font_scale;
	text_location.y = (2+(2+i)*9)*font_scale;
	if(i == option_count)
		text_location.y += 2*font_scale;
	if(erase) {
		Rect r(text_location.x,text_location.y,16*8*font_scale,8*font_scale);
		graphics.set_pen(BG); graphics.rectangle(r);
	}
	print_text(option_names[i], i==cursor_position ? option_names[i].size() : 0);
	if(i != option_count) {
		text_location.x += option_names[i].size()*8*font_scale;
		print_text(std::string_view(option_lists[i].short_names[current_options[i]]), i==cursor_position ? 5 : 0);
	}
}

void update_options() {
	if(cursor_prev == -1) {
		graphics.set_pen(BG);
		graphics.clear();
		for(int i=0; i <= option_count; i++)
			update_options_entry(i, false);
	}else{
		int i = cursor_prev;
		while(true) {
			update_options_entry(i, true);
			if(i == cursor_position)
				break;
			i = cursor_position;
		}
	}
}

void change_options() {
	static int old_option_cursor = 0;
	int saved_cursor_position = cursor_position;
	cursor_prev = -1;
	cursor_position = old_option_cursor;

restart_options:
	update_options();
	text_location.x = str_x(str_config2.size());
	text_location.y = 4*font_scale;
	print_text(str_config2);
	Rect r(text_location.x,text_location.y+10*font_scale,str_config2.size()*8*font_scale,2*font_scale);
	graphics.set_pen(WHITE); graphics.rectangle(r);

	main_buttons[0].str = &char_left;
	update_buttons(main_buttons, main_buttons_size);

	while(true) {
		if(button_x.read() && cursor_position > 0) {
			cursor_prev = cursor_position;
			cursor_position--;
			update_options();
		}else if(button_y.read() && cursor_position < option_count) {
			cursor_prev = cursor_position;
			cursor_position++;
			update_options();
		}else if(button_b.read()) {
			break;
		}else if(button_a.read()) {
			if(cursor_position == option_count) {
				save_config_flag = true;
				break;
			} else {
				int old_hsio_option = current_options[hsio_option_index];
				int old_clock_option = current_options[clock_option_index];
				select_option(cursor_position);
				if(current_options[hsio_option_index] != old_hsio_option) {
					high_speed = -1;
					uart_set_baudrate(uart1, current_options[clock_option_index] ? hsio_opt_to_baud_ntsc[0] : hsio_opt_to_baud_pal[0]);
				}
				if(current_options[clock_option_index] != old_clock_option)
					update_colors();
				goto restart_options;
			}
		}
		st7789.update(&graphics);
		sleep_ms(20);
	}
	old_option_cursor = cursor_position == option_count ? 0 : cursor_position;
	cursor_position = saved_cursor_position;
	cursor_prev = -1;
}

void show_about() {
	graphics.set_pen(BG); graphics.clear();
	text_location.x = str_x(str_about1.size());
	text_location.y = 4*font_scale;
	print_text(str_about1, str_about1.size());

	text_location.x = str_x(str_about2.size());
	text_location.y += 10*font_scale;
	print_text(str_about2);

	text_location.x = str_x(str_about3.size());
	text_location.y += 10*font_scale;
	print_text(str_about3);

	text_location.x = str_x(str_about4.size());
	text_location.y += 16*font_scale;
	print_text(str_about4);

	text_location.x = str_x(str_about5.size());
	text_location.y += 10*font_scale;
	print_text(str_about5);

	text_location.x = str_x(str_about6.size()+str_about7.size()+1);
	text_location.y += 12*font_scale;
	print_text(str_about6, str_about6.size());
	text_location.x += font_scale*8*(1+ str_about6.size());
	print_text(str_about7, str_about7.size());

	text_location.x = str_x(str_about8.size()+str_about9.size()+1);
	text_location.y += 10*font_scale;
	print_text(str_about8, str_about8.size());
	text_location.x += font_scale*8*(1+ str_about8.size());
	print_text(str_about9, str_about9.size());

	text_location.x = str_x(str_about10.size());
	text_location.y += 10*font_scale;
	print_text(str_about10, str_about10.size());

	text_location.x = str_x(str_about11.size());
	text_location.y += 16*font_scale;
	print_text(str_about11);

#ifdef RASPBERRYPI_PICO2
	sprintf(temp_array, "HW: Pico2 %dMB", BOARD_SIZE);
#else
	sprintf(temp_array, "HW: Pico %dMB", BOARD_SIZE);
#endif
	text_location.x = str_x(strlen(temp_array));
	text_location.y += 10*font_scale;
	print_text(std::string_view(temp_array));
	st7789.update(&graphics);
	while(!(button_a.read() || button_b.read() || button_x.read() || button_y.read())) tight_loop_contents();
}

void core1_entry() {

	init_io();
#ifdef PIO_DISK_COUNTER
	init_disk_counter();
#endif
	main_sio_loop();
}

int main() {

#ifndef RASPBERRYPI_PICO2
#ifdef WAV_96K
	// Overclocking is required for 96K WAV support on Pico1
	set_sys_clock_khz(250000, true);
#endif
#endif

	// Core 1 can lockout this one when needed (for writing to the FLASH)

	multicore_lockout_victim_init();
	st7789.set_backlight(255);

	graphics.set_font(&atari_font);
	graphics.set_pen(BG); graphics.clear();

	init_rgb_led();

	int x = text_location.x;
	print_text("A", 1); text_location.x += 2*8*font_scale;
	print_text(str_press_1);
	text_location.x = x; text_location.y += 12*font_scale;
	print_text("B", 1); text_location.x += 2*8*font_scale;
	print_text(str_press_2);
	st7789.update(&graphics);

	ProgressBar pg(208, text_location.y+16*font_scale, true);
	pg.init();
	st7789.update(&graphics);
	uint32_t boot_time;
	bool b_pressed = false;
	do {
		boot_time = to_ms_since_boot(get_absolute_time());
		pg.update(200*boot_time/usb_boot_delay);
		st7789.update(&graphics);
		b_pressed = button_b.read();
		if(b_pressed)
			break;
		if(button_a.read())
			usb_drive();
		sleep_ms(1000/60);
	}while(boot_time <= usb_boot_delay);

	init_locks();

	check_and_load_config(b_pressed);

	update_colors();

	tud_mount_cb();

	f_mount(&fatfs[0], volume_names[0], 1);
	get_drive_label(0);

	// This is initialized in SD card code, but we need it earlier just in case
	// there is no sign of the SD card or even the reader
	gpio_init(9); gpio_set_dir(9, GPIO_IN);
	//gpio_pull_up(9); // Pulled up in hardware

	// It is vital to check the card presence pin first and not to attempt
	// mounting if the card is not there. This is important for the setup
	// where there is no SD card reader and the display is connected
	// to the SPI1 pins
	if(!gpio_get(9) || !try_mount_sd())
		blue_blinks = 4;

	if(curr_path[0] && f_opendir(&dir, curr_path) != FR_OK)
		curr_path[0] = 0;
	if(curr_path[0])
		f_closedir(&dir);

	multicore_launch_core1(core1_entry);

#ifdef CORE1_PRIORITY
	// This would give more scheduling priority to core 1 that
	// serves the SIO communication, but it does not seem to be
	// necessary
	bus_ctrl_hw->priority = BUSCTRL_BUS_PRIORITY_PROC1_BITS;
#endif

	update_main_menu();
	st7789.update(&graphics);
	FSIZE_t last_cas_offset = -1;

	while(true) {
		if(fs_error_or_change) {
			fs_error_or_change = false;
			cursor_prev = -1;
			update_main_menu();
		}
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
				if(cursor_position == 0) {
					change_options();
				}else if(cursor_position == 5 || cursor_position == 6) {
					int si = (cursor_position - 5) ? 4 : 1;
					int li = (cursor_position - 5) ? 1 : 4;
					int di = (cursor_position - 5) ? -1 : 1;
					mutex_enter_blocking(&mount_lock);
					memcpy(temp_array, &mounts[si].str[3], 13);
					memcpy(&temp_array[16], (const void *)mounts[si].mount_path, 256);
					bool t = mounts[si].mounted;
					if(t)
						f_close(&mounts[si].fil);
					for(int i=si; i != li; i += di) {
						memcpy(&mounts[i].str[3], &mounts[i+di].str[3], 13);
						memcpy((void *)mounts[i].mount_path, (void *)mounts[i+di].mount_path, 256);
						if(mounts[i+di].mounted)
							f_close(&mounts[i+di].fil);
						mounts[i].mounted = mounts[i+di].mounted;
						mounts[i].status = 0;
					}
					memcpy(&mounts[li].str[3], temp_array, 13);
					memcpy((void *)mounts[li].mount_path, &temp_array[16], 256);
					mounts[li].mounted = t;
					mounts[li].status = 0;
					if(last_drive > 0)
						last_drive = -1;
					mutex_exit(&mount_lock);
					cursor_prev = -1;
				}else if(cursor_position == 8) {
					show_about();
					cursor_prev = -1;
				}
				update_main_menu();
			}else{
				file_type new_ft = menu_to_type[cursor_position];
				if(ft != new_ft) {
					ft = new_ft;
					last_file_name[0] = 0;
				}
				save_path_flag = false;
				get_file(d);
				if(strcmp((const char*)flash_config_pointer, curr_path))
					save_path_flag = true;
				update_main_menu();
			}
		}else if(button_b.read()) {
			if(d != -1) {
				if(!d && wav_sample_size && mounts[d].mounted)
					flush_pio();
				mutex_enter_blocking(&mount_lock);
				if(mounts[d].mounted) {
					f_close(&mounts[d].fil);
					mounts[d].status = 0;
					mounts[d].mounted = false;
					blue_blinks = 0;
					update_rgb_led(false);
				} else {
					if(mounts[d].mount_path[0]) {
						mounts[d].mounted = true;
						mounts[d].status = 0;
						if(!d) last_cas_offset = -1;
					}
				}
				mutex_exit(&mount_lock);
			}
		}
		FSIZE_t s = mounts[0].status >> 8;
		if(cursor_prev == -1 || (mounts[0].mounted && s != last_cas_offset)) {
			if(s < 0) s = 0;
			cas_pg.update(cas_pg_width*s/(cas_size >> 8));
			last_cas_offset = s;
		}
		update_main_menu_buttons();

/*
		sprintf(txt_buf, "%d %d", rot_pos, rot_pos2);
		text_location.x = 0;
		text_location.y = 0;
		Rect rt(text_location.x, text_location.y, 20*font_scale*8, font_scale*16);
		graphics.set_pen(BG);
		graphics.rectangle(rt);
		print_text(txt_buf);
*/

//		text_location.y += 16;
//		print_text(&txt_buf[20]);


		if(to_ms_since_boot(get_absolute_time()) - last_drive_access > (last_drive == 0 ? 25000 : 2000))
			last_drive = -1;

		graphics.set_font(&symbol_font);
		for(int i=0; i<5; i++) {
			d = mount_to_menu[i];

			text_location.x = menu_entries[d].x-10*font_scale;
			text_location.y = menu_entries[d].y;
			graphics.set_pen(BG);
			Rect rr(text_location.x,text_location.y,8*font_scale,8*font_scale);
			graphics.rectangle(rr);

			bool mntd = mounts[i].mounted;

			graphics.set_pen(mntd ? (i == last_drive ? GREEN : BG) : RED);

			graphics.text(mntd ? ")" : "-", text_location, st7789.width, 1, 0.0, 0, true);
			text_location.x += 8;
			graphics.text(mntd ? "*" : ".", text_location, st7789.width, 1, 0.0, 0, true);
			text_location.x -= 8;
			text_location.y += 8;
			graphics.text(mntd ? "+" : "/", text_location, st7789.width, 1, 0.0, 0, true);
			text_location.x += 8;
			graphics.text(mntd ? "," : "0", text_location, st7789.width, 1, 0.0, 0, true);
		}
		st7789.update(&graphics);
		sleep_ms(20);
		graphics.set_font(&atari_font);
	}
	return 0;
}

void tud_mount_cb(void) {
	if (!mount_fatfs_disk())
		create_fatfs_disk();
}

void tud_umount_cb(void) {}

void tud_suspend_cb(bool remote_wakeup_en) {
	// (void) remote_wakeup_en;
}

void tud_resume_cb(void) {}

void tud_cdc_line_state_cb(uint8_t itf, bool dtr, bool rts) {
	//(void) itf;
	//(void) rts;
	//(void) dtr;
}

void tud_cdc_rx_cb(uint8_t itf) {
	//(void) itf;
}
