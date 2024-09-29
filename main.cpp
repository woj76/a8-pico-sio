// Useful links:
// https://forums.raspberrypi.com/viewtopic.php?t=349257
// https://github.com/TheMontezuma/SIO2BSD
// https://forums.atariage.com/topic/290397-atari-sio-transmission-oscillogram/
// https://www.atarimax.com/jindroush.atari.org/asio.html
// https://github.com/HiassofT/highspeed-sio/blob/master/README.txt
// https://tnt23.livejournal.com/605184.html
// http://ftp.pigwa.net/stuff/collections/nir_dary_cds/Tech%20Info/The%20SIO%20protocol%20description/sio.html
// https://allpinouts.org/pinouts/connectors/serial/atari-8-bit-serial-input-output-sio/

// https://forums.atariage.com/topic/271215-why-the-k-on-boot/?do=findComment&comment=4209492
// http://www.whizzosoftware.com/sio2arduino/vapi.html
// https://github.com/carlk3/no-OS-FatFS-SD-SPI-RPi-Pico
// https://www.a8preservation.com/#/guides/atx
// https://forums.atariage.com/topic/282759-databyte-disks-on-atari-810/?do=findComment&comment=4112899

#include <string.h>
#include <cstdlib>
#include <ctype.h>

#include "pico/multicore.h"
#include "pico/util/queue.h"
#include "pico/time.h"
#include "pico/rand.h"
#include "hardware/uart.h"
#include "hardware/sync.h"
#include "hardware/flash.h"
#include "hardware/pio.h"
#include "hardware/dma.h"
#include "hardware/irq.h"
#include "hardware/regs/busctrl.h"
#include "hardware/structs/bus_ctrl.h"
#include "tusb.h"
#include "fatfs_disk.h"
#include "flash_fs.h"
#include "ff.h"
#include "sd_card.h"
#include "diskio.h"

#include "libraries/pico_display_2/pico_display_2.hpp"
#include "drivers/st7789/st7789.hpp"
#include "libraries/pico_graphics/pico_graphics.hpp"
#include "drivers/rgbled/rgbled.hpp"
#include "drivers/button/button.hpp"

#include "font_atari_data.hpp"
#include "pin_io.pio.h"

#define RELOCATE_BUTTON_PINS

// State of the pin to detect motor on, should be 0 or 1 depending if
// connected through a NOT gate transistor (needed for Pico 2 to function properly)
#define MOTOR_ON_STATE 0u

// Use the PIO based emulated disk rotational counter for the ATX support
// (This is more of a PIO programming exercise rather than anything else)
// #define PIO_DISK_COUNTER

#ifdef PIO_DISK_COUNTER
#include "disk_counter.pio.h"
#endif

#include "boot_loader.h"

const uint32_t usb_boot_delay = 3000;
const uint8_t font_scale = 2;
constexpr std::string_view str_file_transfer{"File transfer..."};

constexpr std::string_view str_press_1{"USB drive"};
constexpr std::string_view str_press_2{"Config reset"};

constexpr std::string_view str_up_dir{"../"};
constexpr std::string_view str_new_image{"New image...>"};

constexpr std::string_view str_no_files{"[No files!]"};
constexpr std::string_view str_no_media{"No media!?"};
constexpr std::string_view str_config2{"Config"};
constexpr std::string_view str_creating{" Creating... "};
constexpr std::string_view str_create_failed{"Create failed!"};

constexpr std::string_view str_about1{"A8 Pico SIO"};
constexpr std::string_view str_about2{"by woj@AtariAge"};
constexpr std::string_view str_about3{"(c) 2024"};
constexpr std::string_view str_about4{"Inspired by and"};
constexpr std::string_view str_about5{"based on code of"};
constexpr std::string_view str_about6{"A8PicoCart"};
constexpr std::string_view str_about7{"SIO2BSD"};
constexpr std::string_view str_about8{"SDrive-MAX"};
constexpr std::string_view str_about9{"Altirra"};
constexpr std::string_view str_about10{"EclaireXL"};
constexpr std::string_view str_about11{"Version 0.90"};

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

RGBLED led(PicoDisplay2::LED_R, PicoDisplay2::LED_G, PicoDisplay2::LED_B, Polarity::ACTIVE_LOW, 25);


#ifdef RELOCATE_BUTTON_PINS
// Relocated button pins to free the SPI1 interface for the SD card
Button button_a(0), button_b(1), button_x(2), button_y(3);
#else
Button
	button_a(PicoDisplay2::A),
	button_b(PicoDisplay2::B),
	button_x(PicoDisplay2::X),
	button_y(PicoDisplay2::Y);
#endif


const Pen
	BG=graphics.create_pen(0, 0x5F, 0x8A),
	WHITE=graphics.create_pen(0x5D, 0xC1, 0xEC),
	GREEN=graphics.create_pen(0x85,0xA0,0),
	RED=graphics.create_pen(0x96,0x27,0x16);


void print_text(const std::string_view &t, int inverse=0) {
	graphics.set_pen(WHITE);
	if(inverse) {
		Rect rect(text_location.x, text_location.y, inverse*font_scale*8, font_scale*8);
		graphics.rectangle(rect); graphics.set_pen(BG);
	}
	graphics.text(t, text_location, st7789.width, font_scale, 0.0, 0, true);
}

volatile int8_t red_blinks = 0;
volatile int8_t green_blinks = 0;
volatile int8_t blue_blinks = 0;

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

void update_rgb_led(bool from_interrupt) {
	uint32_t ints;
	if(!from_interrupt)
		ints = save_and_disable_interrupts();
	uint8_t r = (red_blinks & 1) ? 128 : 0;
	uint8_t g = (green_blinks & 1) ? 128 : 0;
	uint8_t b = (blue_blinks & 1) ? 128 : 0;
	led.set_rgb(r,g,b);
	if(!from_interrupt)
		restore_interrupts(ints);
}

bool repeating_timer_led(struct repeating_timer *t) {
	if(red_blinks > 0) red_blinks--;
	if(green_blinks > 0) green_blinks--;
	if(blue_blinks > 0) blue_blinks--;
	update_rgb_led(true);
	return true;
}

std::string *ptr_str_file_name;

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
	"   >> Save <<   "
};
const int option_count = 9;

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
const char * const xex_option_names_short[] = {" $700", " $500", " $600", " $800", " $900", " $A00"};
const char * const xex_option_names_long[] = {"Loader at $700", "Loader at $500", "Loader at $600", "Loader at $800", "Loader at $900", "Loader at $A00"};
const char * const turbo1_option_names_short[] = {"  SIO", " J2P4", " PROC", "  INT", " J2P1"};
const char * const turbo1_option_names_long[] = {"SIO Data In", "Joy2 Port Pin 4", "SIO Proceed", "SIO Interrupt", "Joy2 Port Pin 1"};
const char * const turbo2_option_names_short[] = {" COMM", " J2P3", "  SIO", " NONE"};
const char * const turbo2_option_names_long[] = {"SIO Command", "Joy2 Port Pin 3", "SIO Data Out", "None / Motor"};
const char * const turbo3_option_names_long[] = {"Normal", "Inverted"};

const int mount_option_index = 0;
const int clock_option_index = 1;
const int hsio_option_index = 2;
const int atx_option_index = 3;
const int xex_option_index = 4;
const int turbo1_option_index = 5;
const int turbo2_option_index = 6;
const int turbo3_option_index = 7;

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
	}
};

uint8_t current_options[option_count-1] = {0};
const uint curr_path_len = 512;
char curr_path[curr_path_len];
size_t num_files, num_files_page;

volatile uint8_t sd_card_present = 0;

const char * const volume_names[] = {"0:", "1:"};

char temp_array[curr_path_len];
volatile int16_t create_new_file = 0;

char last_file_name[15] = {0};

typedef struct  __attribute__((__packed__)) {
	bool dir;
	char short_name[15];
	char long_name[258];
	uint16_t dir_index;
	bool last_file;
} file_entry_type;

const int files_per_page = 12;
file_entry_type file_entries[files_per_page+2];

int file_entry_cmp(int i, int j) {
	file_entry_type e1 = file_entries[i];
	file_entry_type e2 = file_entries[j];
	if (e1.dir && !e2.dir) return -1;
	else if (!e1.dir && e2.dir) return 1;
	else return strcasecmp(e1.long_name, e2.long_name);
}

size_t get_filename_ext(char *filename) {
	size_t l = strlen(filename);
	size_t i = l;
	while(i > 0 && filename[--i] != '.');
	return (i == 0 || l-i > 4) ? l : i+1;
}

enum file_type {none, disk, casette};
file_type ft = file_type::none;

bool is_valid_file(char *filename) {
	size_t i = get_filename_ext(filename);
	switch(ft) {
		case file_type::casette:
			return strcasecmp(&filename[i], "CAS") == 0;
		case file_type::disk:
			return strcasecmp(&filename[i], "ATR") == 0 || strcasecmp(&filename[i], "ATX") == 0 || strcasecmp(&filename[i], "XEX") == 0  || strcasecmp(&filename[i], "COM") == 0;
		default:
			return false;
	}
}

void mark_directory(int i) {
	int l = strlen(file_entries[i].long_name);
	file_entries[i].long_name[l] = '/';
	file_entries[i].long_name[l+1] = 0;
	l = strlen(file_entries[i].short_name);
	file_entries[i].short_name[l] = '/';
	file_entries[i].short_name[l+1] = 0;
}

FATFS fatfs[2];

auto_init_mutex(fs_lock);

uint16_t next_page_references[5464]; // This is 65536 / 12 (files_per_page) with a tiny slack

// param page_index signed int, -1 count only
// result -1 error, otherwise files read / counted
int32_t read_directory(int32_t page_index, int page_size) {
	//FATFS fatfs;
	FILINFO fno; DIR dir;
	int32_t ret = -1;

	if(!curr_path[0]) {
		int i;
		for(i=0; i < sd_card_present+1; i++) {
			strcpy(file_entries[i].short_name, volume_names[i]);
			strcpy(file_entries[i].long_name, volume_names[i]);
			file_entries[i].dir = true;
			file_entries[i].dir_index = i;
			file_entries[i].last_file = false;
			if(last_file_name[0] && !strcmp(file_entries[i].short_name, last_file_name))
				file_entries[i].last_file = true;
		}
		return i;
	}

	loading_pg.init();
	struct repeating_timer timer;
	add_repeating_timer_ms(1000/60, repeating_timer_directory, NULL, &timer);

	mutex_enter_blocking(&fs_lock);
	//f (f_mount(&fatfs[0], curr_path, 1) == FR_OK) {
		if (f_opendir(&dir, curr_path) == FR_OK) {
			ret = 0;
			uint16_t dir_index = -1;
			int32_t look_for_index = page_index >= 0 ? next_page_references[page_index] : -1;
			while (true) {
				dir_index++;
				if (f_readdir(&dir, &fno) != FR_OK || fno.fname[0] == 0) break;
				if (fno.fattrib & (AM_HID | AM_SYS)) continue;
				bool is_dir = (fno.fattrib & AM_DIR);
				if (!is_dir && !is_valid_file(fno.fname)) continue;
				if(page_index < 0) {
					file_entries[page_size].dir = is_dir;
					file_entries[page_size].dir_index = dir_index;
					strcpy(file_entries[page_size].long_name, fno.fname);
					strcpy(file_entries[page_size].short_name, fno.altname[0] ? fno.altname : fno.fname);
					if(is_dir)
						mark_directory(page_size);
					if(!ret || file_entry_cmp(page_size, page_size+1) < 0)
						file_entries[page_size+1] = file_entries[page_size];
					ret++;
				} else {
					if(look_for_index >= 0 && dir_index != look_for_index)
						continue;
					file_entries[ret].dir = is_dir;
					file_entries[ret].dir_index = dir_index;
					strcpy(file_entries[ret].long_name, fno.fname);
					strcpy(file_entries[ret].short_name, fno.altname[0] ? fno.altname : fno.fname);
					file_entries[ret].last_file = false;
					if(is_dir) mark_directory(ret);
					if(last_file_name[0] && !strcmp(file_entries[ret].short_name, last_file_name))
						file_entries[ret].last_file = true;
					if(!ret) {
						ret++;
						look_for_index = -1;
						dir_index = -1;
						f_rewinddir(&dir);
						continue;
					}
					if(file_entry_cmp(ret, 0) <= 0)
						continue;
					int32_t ri = ret;
					while(ri > 0 && file_entry_cmp(ri, ri-1) < 0) {
						file_entry_type fet = file_entries[ri];
						file_entries[ri] = file_entries[ri-1];
						file_entries[ri-1] = fet;
						ri--;
					}
					if(ret < page_size) {
						ret++;
					}else if(look_for_index == -1 || file_entry_cmp(page_size, page_size+1) <= 0) {
						look_for_index = -2;
						file_entries[page_size+1] = file_entries[page_size];
					}
				}
				next_page_references[page_index+1] = file_entries[page_size+1].dir_index;
			}
			f_closedir(&dir);
		}
		//f_mount(0, curr_path, 1);
	//}
	mutex_exit(&fs_lock);
	// sleep_ms(2000); // For testing
	cancel_repeating_timer(&timer);
	return ret;
}

int cursor_prev = -1;
int cursor_position = 1;

const char * const str_config = "Config...";
char str_d1[] = "D1:  <EMPTY>   ";
char str_d2[] = "D2:  <EMPTY>   ";
char str_d3[] = "D3:  <EMPTY>   ";
char str_d4[] = "D4:  <EMPTY>   ";
const char * const str_rot_up = "Rotate Up";
const char * const str_rot_down = "Rotate Down";
char str_cas[] = "C:  <EMPTY>   ";
const char * const str_about = "About...";


const int menu_to_mount[] = {-1,1,2,3,4,-1,-1,0,-1};

const file_type menu_to_type[] = {
	file_type::none,
	file_type::disk, file_type::disk, file_type::disk, file_type::disk,
	file_type::none, file_type::none,
	file_type::casette, file_type::none
};

typedef struct {
	char *str;
	char *mount_path;
	bool mounted;
	FSIZE_t status;
} mounts_type;

char d1_mount[curr_path_len] = {0};
char d2_mount[curr_path_len] = {0};
char d3_mount[curr_path_len] = {0};
char d4_mount[curr_path_len] = {0};
char c_mount[curr_path_len] = {0};

mounts_type mounts[] = {
	{.str=str_cas, .mount_path=c_mount, .mounted=false, .status = 0},
	{.str=str_d1, .mount_path=d1_mount, .mounted=false, .status = 0},
	{.str=str_d2, .mount_path=d2_mount, .mounted=false, .status = 0},
	{.str=str_d3, .mount_path=d3_mount, .mounted=false, .status = 0},
	{.str=str_d4, .mount_path=d4_mount, .mounted=false, .status = 0}
};

const size_t sector_buffer_size = 768;
uint8_t sector_buffer[sector_buffer_size];

const uint32_t cas_header_FUJI = 0x494A5546;
const uint32_t cas_header_baud = 0x64756162;
const uint32_t cas_header_data = 0x61746164;
const uint32_t cas_header_fsk  = 0x206B7366;
const uint32_t cas_header_pwms = 0x736D7770;
const uint32_t cas_header_pwmc = 0x636D7770;
const uint32_t cas_header_pwmd = 0x646D7770;
const uint32_t cas_header_pwml = 0x6C6D7770;

uint32_t timing_base_clock, max_clock_ms;

typedef struct __attribute__((__packed__)) {
	uint32_t signature;
	uint16_t chunk_length;
	union {
		uint8_t aux_b[2];
		uint16_t aux_w;
	} aux;
} cas_header_type;

cas_header_type cas_header;
volatile FSIZE_t cas_size;
uint8_t pwm_bit_order;
uint8_t pwm_bit;
uint32_t pwm_sample_duration; // in cycles dep the base timing value
uint32_t cas_sample_duration; // in cycles dep the base timing value
uint16_t silence_duration; // in ms
uint16_t cas_block_index;
uint16_t cas_block_multiple;
uint8_t cas_fsk_bit;

volatile bool cas_block_turbo;

const uint8_t percom_table[] = {
	0x28,0x03,0x00,0x12,0x00,0x00,0x00,0x80, 0x80,0x16,0x80,0x00, 0,
	0x28,0x03,0x00,0x1A,0x00,0x04,0x00,0x80, 0x80,0x20,0x80,0x00, 0,
	0x28,0x03,0x00,0x12,0x00,0x04,0x01,0x00, 0xE8,0x2C,0x00,0x01, 0,
	0x28,0x03,0x00,0x12,0x01,0x04,0x01,0x00, 0xE8,0x59,0x00,0x01, 0
};

const int percom_table_size = 4;

typedef struct __attribute__((__packed__)) {
	uint16_t magic;
	uint16_t pars;
	uint16_t sec_size;
	uint8_t pars_high;
	uint32_t crc;
	uint8_t temp1, temp2, temp3, temp4;
	uint8_t flags;
} atr_header_type;

typedef union {
	atr_header_type atr_header;
	uint8_t data[sizeof(atr_header_type)];
} disk_header_type;

disk_header_type disk_headers[4];

const uint8_t disk_type_atr = 1;
const uint8_t disk_type_xex = 2;
const uint8_t disk_type_atx = 3;

uint8_t locate_percom(int drive_number) {
	int i = 0;
	uint8_t r = 0x80;
	while(i<percom_table_size) {
		if(!memcmp(&disk_headers[drive_number-1].data[2], &percom_table[i*13+8], 5)) {
			r = i;
			break;
		}
		i++;
	}
	return r;
}


bool inline compare_percom(int drive_number) {
	int i = disk_headers[drive_number-1].atr_header.temp3;
	if(i & 0x80)
		return false;
	i &= 0x3;
	if(percom_table[i*13] != sector_buffer[0] || memcmp(&percom_table[i*13+2], &sector_buffer[2], 6))
		return false;
	return true;
}


FSIZE_t cas_read_forward(FIL *fil, FSIZE_t offset) {
	uint bytes_read;
	if(f_lseek(fil, offset) != FR_OK) {
		offset = 0;
		goto cas_read_forward_exit;
	}
	while(true) {
		if(f_read(fil, &cas_header, sizeof(cas_header_type), &bytes_read) != FR_OK || bytes_read != sizeof(cas_header_type)) {
			offset = 0;
			goto cas_read_forward_exit;
		}
		offset += bytes_read;
		cas_block_index = 0;
		switch(cas_header.signature) {
			case cas_header_FUJI:
				offset += cas_header.chunk_length;
				if(f_lseek(fil, offset) != FR_OK) {
					offset = 0;
					goto cas_read_forward_exit;
				}
				break;
			case cas_header_baud:
				if(cas_header.chunk_length) {
					offset = 0;
					goto cas_read_forward_exit;
				}
				cas_sample_duration = timing_base_clock/cas_header.aux.aux_w;
				break;
			case cas_header_data:
				cas_block_turbo = false;
				silence_duration = cas_header.aux.aux_w;
				cas_block_multiple = 1;
				goto cas_read_forward_exit;
			case cas_header_fsk:
				cas_block_turbo = false;
				silence_duration = cas_header.aux.aux_w;
				cas_block_multiple = 2;
				cas_fsk_bit = 0;
				goto cas_read_forward_exit;
			case cas_header_pwms:
				if(cas_header.chunk_length != 2) {
					offset = 0;
					goto cas_read_forward_exit;
				}
				pwm_bit_order = (cas_header.aux.aux_b[0] >> 2) & 0x1;
				cas_header.aux.aux_b[0] &= 0x3;
				if(cas_header.aux.aux_b[0] == 0b01)
					pwm_bit = 0; // 0
				else if(cas_header.aux.aux_b[0] == 0b10)
					pwm_bit = 1; // 1
				else {
					offset = 0;
					goto cas_read_forward_exit;
				}
				pwm_sample_duration = 0;
				if(f_read(fil, &pwm_sample_duration, sizeof(uint16_t), &bytes_read) != FR_OK || bytes_read != sizeof(uint16_t)) {
					offset = 0;
					goto cas_read_forward_exit;
				}
				offset += bytes_read;
				pwm_sample_duration = timing_base_clock/(2*pwm_sample_duration);
				break;
			case cas_header_pwmc:
				cas_block_turbo = true;
				silence_duration = cas_header.aux.aux_w;
				cas_block_multiple = 3;
				goto cas_read_forward_exit;
			case cas_header_pwmd:
				cas_block_turbo = true;
				cas_block_multiple = 1;
				goto cas_read_forward_exit;
			case cas_header_pwml:
				cas_block_turbo = true;
				silence_duration = cas_header.aux.aux_w;
				cas_block_multiple = 2;
				cas_fsk_bit = pwm_bit;
				goto cas_read_forward_exit;
			default:
				break;
		}
	}
cas_read_forward_exit:
	return offset;
}

// const uint led_pin = 25;

const uint sio_tx_pin = 4;
const uint sio_rx_pin = 5;
const uint normal_motor_pin = 10;
const uint command_line_pin = 11;
const uint proceed_pin = 21;
const uint interrupt_pin = 22;
const uint joy2_p1_pin = 26;
const uint joy2_p3_pin = 27;
const uint joy2_p4_pin = 28;

// Conventional SIO, but also Turbo D & Turbo 6000
const uint32_t normal_motor_pin_mask = (1u << normal_motor_pin);
const uint32_t normal_motor_value_on = (MOTOR_ON_STATE << normal_motor_pin);

// Turbo 2000 KSO - Joy 2 port
const uint32_t kso_motor_pin_mask = (1u << joy2_p3_pin);
const uint32_t kso_motor_value_on = (0u << joy2_p3_pin);

// Turbo 2001 / 2000F and sorts over SIO data
const uint32_t comm_motor_pin_mask = (1u << command_line_pin) | (1u << normal_motor_pin);
const uint32_t comm_motor_value_on = (0u << command_line_pin) | (MOTOR_ON_STATE << normal_motor_pin);

// Turbo Blizzard - SIO Data Out
const uint32_t sio_motor_pin_mask = (1u << sio_rx_pin) | (1u << normal_motor_pin);
const uint32_t sio_motor_value_on = (0u << sio_rx_pin) | (MOTOR_ON_STATE << normal_motor_pin);

uint32_t turbo_motor_pin_mask;
uint32_t turbo_motor_value_on;
uint turbo_data_pin;

// Turbo 2000 KSO
// const uint turbo_data_pin = joy2_p4_pin; // Joy 2 port, pin 4
// Turbo D
//const uint turbo_data_pin = joy2_p1_pin; // Joy 2 port, pin 1
// Turbo 2001 / 2000F SIO / Turbo Blizzard
//const uint turbo_data_pin = sio_tx_pin;
// Turbo 6000, this needs pwm_bit invertion
// const uint turbo_data_pin = proceed_pin;
// Rambit, also needs bit inversion
//const uint turbo_data_pin = interrupt_pin;

const PIO cas_pio = pio0;
const PIO disk_pio = pio1;
const auto GPIO_FUNC_PIOX = GPIO_FUNC_PIO0;

uint sm, sm_turbo;
pio_sm_config sm_config_turbo;
int8_t turbo_conf[] = {-1, -1, -1};
uint pio_offset;
const uint opt_to_turbo_data_pin[] = {sio_tx_pin, joy2_p4_pin, proceed_pin, interrupt_pin, joy2_p1_pin};
const uint32_t opt_to_turbo_motor_pin_mask[] = {comm_motor_pin_mask, kso_motor_pin_mask, sio_motor_pin_mask, normal_motor_pin_mask};
const uint32_t opt_to_turbo_motor_pin_val[] = {comm_motor_value_on, kso_motor_value_on, sio_motor_value_on, normal_motor_value_on};

void check_turbo_conf() {
	if(turbo_conf[0] != current_options[turbo1_option_index]) {
		if(turbo_conf[0] >= 0) {
			pio_sm_set_enabled(cas_pio, sm_turbo, false);
		}
		turbo_conf[0] = current_options[turbo1_option_index];
		turbo_data_pin = opt_to_turbo_data_pin[turbo_conf[0]];
		pio_gpio_init(cas_pio, turbo_data_pin);
		sm_config_set_out_pins(&sm_config_turbo, turbo_data_pin, 1);
		pio_sm_set_consecutive_pindirs(cas_pio, sm_turbo, turbo_data_pin, 1, true);
		pio_sm_init(cas_pio, sm_turbo, pio_offset, &sm_config_turbo);
		pio_sm_set_enabled(cas_pio, sm_turbo, true);
	}
	turbo_conf[1] = current_options[turbo2_option_index];
	turbo_motor_pin_mask = opt_to_turbo_motor_pin_mask[turbo_conf[1]];
	turbo_motor_value_on = opt_to_turbo_motor_pin_val[turbo_conf[1]];
	turbo_conf[2] = current_options[turbo3_option_index];
}


volatile bool dma_block_turbo;
volatile bool dma_going = false;
int disk_dma_channel, dma_channel, dma_channel_turbo;

queue_t pio_queue;
// 10*8 is not enough for Turbo D 9000, but going wild here costs memory, each item is 4 bytes
// 16*8 also fails sometimes with the 1MHz base clock
const int pio_queue_size = 64*8;

uint32_t pio_e;

void dma_handler() {
	int dc = dma_block_turbo ? dma_channel_turbo : dma_channel;
	if(dma_going)
		dma_hw->ints1 = 1u << dc;
	else
		dma_going = true;
	if(queue_try_remove(&pio_queue, &pio_e))
		dma_channel_start(dc);
	else
		dma_going = false;
}

void pio_enqueue(uint8_t b, uint32_t d) {
	uint32_t e = (b^(cas_block_turbo ? turbo_conf[2] : 0) | ((d - pio_prog_cycle_corr) << 1));
	queue_add_blocking(&pio_queue, &e);
	if(!dma_going) {
		dma_block_turbo = cas_block_turbo;
		dma_handler();
	}
}

/*
void pio_enqueue(uint sm, uint8_t b, uint32_t d) {
	uint32_t e = (b | (d << 1));
	while(pio_sm_is_tx_fifo_full(cas_pio, sm))
		tight_loop_contents();
	cas_pio->txf[sm] = (b | (d << 1));
}
*/

#ifdef PIO_DISK_COUNTER
volatile uint32_t disk_counter;

void disk_dma_handler() {
	if(dma_hw->ints0 & (1u << disk_dma_channel)) {
		dma_hw->ints0 = 1u << disk_dma_channel;
		dma_channel_start(disk_dma_channel);
	}
}
#endif

const uint8_t boot_reloc_locs[] = {0x03, 0x0a, 0x0d, 0x12, 0x17, 0x1a, 0x21, 0x2c, 0x33, 0x3e, 0x44, 0x5c, 0x6b, 0x6e, 0x7f, 0x82, 0x87, 0x98, 0x9b, 0xa2, 0xa7, 0xae, 0xb2};
const int boot_reloc_locs_size = 23;
const int8_t boot_reloc_delta[] = {0, -2, -1, 1, 2, 3};

const uint8_t hsio_opt_to_index[] = {0x28, 0x10, 6, 5, 4, 3, 2, 1, 0};
const uint hsio_opt_to_baud_ntsc[] = {19040, 38908, 68838, 74575, 81354, 89490, 99433, 111862, 127842};
const uint hsio_opt_to_baud_pal[] = {18866, 38553, 68210, 73894, 80611, 88672, 98525, 110840, 126675};
// PAL/NTSC average
// const uint hsio_opt_to_baud[] = {18953, 38731, 68524, 74234, 80983, 89081, 98979, 111351, 127258};
volatile int8_t high_speed = -1;

typedef struct __attribute__((__packed__)) {
	uint8_t device_id;
	uint8_t command_id;
	uint16_t sector_number;
	uint8_t checksum;
} sio_command_type;

sio_command_type sio_command;

uint8_t sio_checksum(uint8_t *data, size_t len) {
	uint8_t cksum = 0;
	uint16_t nck;

	for(int i=0; i<len; i++) {
		nck = cksum + data[i];
		cksum = (nck > 0x00ff) ? 1 : 0;
		cksum += (nck & 0x00ff);
	}
	return cksum;
}

const uint32_t serial_read_timeout = 5000;

auto_init_mutex(mount_lock);

// volatile bool sio_command_received = false;
// uint gpio, uint32_t event_mask

bool try_get_sio_command() {
	//if(gpio != command_line_pin || !(event_mask & GPIO_IRQ_EDGE_FALL))
	//	return false;
	// sio_command_received = false;
	static uint16_t freshly_changed = 0;
	bool r = true;
	int i = 0;

	// Assumption - when casette motor is on the active command line
	// is much more likely due to turbo activation and casette transfer
	// should not be interrupted
	if(gpio_get(command_line_pin) || gpio_get(normal_motor_pin) == MOTOR_ON_STATE)
		return false;

	// add sleep?
	//if(gpio_get(normal_motor_pin) == MOTOR_ON_STATE)
	//		return;

	memset(&sio_command, 0x01, 5);

	// HiassofT suggests that if bytes == 5 with wrong checksum only or only a single framing error
	// repeat once without changing the speed
	// wrong #of bytes, multiple framing errors
	// continue reading while command line pin is low? When to break this? Let's say after 64?!

	while(!gpio_get(command_line_pin) && i<5 && uart_is_readable_within_us(uart1, serial_read_timeout) && !uart_get_hw(uart1)->rsr)
		((uint8_t *)&sio_command)[i++] = (uint8_t)uart_get_hw(uart1)->dr;

	//while(!gpio_get(command_line_pin) && uart_is_readable(uart1))
	//	((uint8_t *)&sio_command)[i] = (uint8_t)uart_get_hw(uart1)->dr;
	//if(!gpio_get(command_line_pin) && uart_is_readable(uart1))
	//	((uint8_t *)&sio_command)[i] = (uint8_t)uart_get_hw(uart1)->dr;

	if(gpio_get(command_line_pin) || uart_get_hw(uart1)->rsr || i != 5 ||
			sio_checksum((uint8_t *)&sio_command, 4) != sio_command.checksum || sio_command.command_id < 0x21) {
		hw_clear_bits(&uart_get_hw(uart1)->rsr, UART_UARTRSR_BITS);
		r = false;
	}

	if(r) {
		absolute_time_t t = make_timeout_time_us(1000); // According to Avery's manual 950us
		while (!gpio_get(command_line_pin) && absolute_time_diff_us(get_absolute_time(), t) > 0)
			tight_loop_contents();
		if(!gpio_get(command_line_pin))
			r = false;
		else
			freshly_changed = 0;
	}else{
		if(current_options[hsio_option_index] && !freshly_changed && high_speed >= 0) {
			high_speed ^= 1;
			freshly_changed = 2 - high_speed;
			uint8_t s = high_speed ? current_options[hsio_option_index] : 0;
			uart_set_baudrate(uart1, current_options[clock_option_index] ? hsio_opt_to_baud_ntsc[s] : hsio_opt_to_baud_pal[s]);
		}else if(freshly_changed)
			freshly_changed--;
	}

	// sio_command_received = r;
	return r;
}

uint8_t check_drive_and_sector_status(int drive_number, FSIZE_t *offset, FSIZE_t *to_read, bool op_write=false) {

	if(op_write && (disk_headers[drive_number-1].atr_header.flags & 0x1)) {
		disk_headers[drive_number-1].atr_header.temp2 &= 0xBF;
		return 'N';
	}
	*offset = sio_command.sector_number-1;
	if(*offset < 3) {
		*offset <<= 7;
		*to_read = 128;
	} else {
		*to_read = disk_headers[drive_number-1].atr_header.sec_size;
		*offset = 384+(*offset-3)*(*to_read);
	}
	if(sio_command.sector_number == 0 || *offset + *to_read > mounts[drive_number].status) {
		disk_headers[drive_number-1].atr_header.temp2 &= 0xEF;
		return 'N';
	}
	return 'A';
}

uint8_t try_receive_data(int drive_number, FSIZE_t to_read) {
	int i=0;
	uint8_t r = 'A';
	uint8_t status = 0;
	while(i<to_read && uart_is_readable_within_us(uart1, serial_read_timeout))
		sector_buffer[i++] = (uint8_t)uart_get_hw(uart1)->dr;
	if(i != to_read || !uart_is_readable_within_us(uart1, serial_read_timeout) || (uint8_t)uart_get_hw(uart1)->dr != sio_checksum(sector_buffer, to_read)) {
		status |= 0x02;
		r = 'N';
	}
	disk_headers[drive_number-1].atr_header.temp1 = status;
	return r;
}

FRESULT mounted_file_transfer(int drive_number, FSIZE_t offset, FSIZE_t to_transfer, bool op_write, size_t t_offset = 0, FSIZE_t brpt=1) {
	//FATFS fatfs;
	FIL fil;
	FRESULT f_op_stat;
	uint bytes_transferred;
	uint8_t *data = &sector_buffer[t_offset];

	mutex_enter_blocking(&fs_lock);
	do {
		//if((f_op_stat = f_mount(&fatfs[0], (const char *)mounts[drive_number].mount_path, 1)) != FR_OK)
		//	break;
		if((f_op_stat = f_open(&fil, (const char *)mounts[drive_number].mount_path, op_write ? FA_WRITE : FA_READ)) != FR_OK)
			break;
		if((f_op_stat = f_lseek(&fil, offset)) != FR_OK)
			break;
		uint vol_num = mounts[drive_number].mount_path[0] - '0';
		if(op_write) {
			uint32_t ints;
			if(!vol_num) {
				ints = save_and_disable_interrupts();
				multicore_lockout_start_blocking();
			}
			for(uint i=0; i<brpt; i++)
				f_op_stat = f_write(&fil, data, to_transfer, &bytes_transferred);
			f_op_stat = f_sync(&fil);
			if(!vol_num){
				multicore_lockout_end_blocking();
				restore_interrupts(ints);
			}
		} else {
			f_op_stat = f_read(&fil, data, to_transfer, &bytes_transferred);

		}
		if(f_op_stat != FR_OK)
			break;
		if(bytes_transferred != to_transfer /* || !mounts[drive_number].mounted */)
			f_op_stat = FR_INT_ERR;
	}while(false);
	f_close(&fil);
	//f_mount(0, (const char *)mounts[drive_number].mount_path, 1);
	mutex_exit(&fs_lock);
	return f_op_stat;
}

// (Quite) modified implementation from sdrive-max project
// Original author of the ATX code dated 21/01/2018 is Daniel Noguerol
// Check the sdrive-max project for details

enum atx_density { atx_single, atx_medium, atx_double};

struct __attribute__((__packed__)) atxFileHeader {
	uint8_t signature[4];
	uint16_t version;
	uint16_t minVersion;
	uint16_t creator;
	uint16_t creatorVersion;
	uint32_t flags;
	uint16_t imageType;
	uint8_t density;
	uint8_t reserved0;
	uint32_t imageId;
	uint16_t imageVersion;
	uint16_t reserved1;
	uint32_t startData;
	uint32_t endData;
	uint8_t reserved2[12];
};

struct __attribute__((__packed__)) atxTrackHeader {
	uint32_t size;
	uint16_t type;
	uint16_t reserved0;
	uint8_t trackNumber;
	uint8_t reserved1;
	uint16_t sectorCount;
	uint16_t rate;
	uint16_t reserved3;
	uint32_t flags;
	uint32_t headerSize;
	uint8_t reserved4[8];
};

struct __attribute__((__packed__)) atxSectorListHeader {
	uint32_t next;
	uint16_t type;
	uint16_t pad0;
};

struct __attribute__((__packed__)) atxSectorHeader {
	uint8_t number;
	uint8_t status;
	uint16_t timev;
	uint32_t data;
};

struct __attribute__((__packed__)) atxTrackChunk {
	uint32_t size;
	uint8_t type;
	uint8_t sectorIndex;
	uint16_t data;
};

const uint16_t atx_version = 0x01;
const size_t max_track = 42;

const uint au_full_rotation = 26042; // number of angular units in a full disk rotation
const uint us_drive_request_delay = 3220; // number of microseconds drive takes to process a request
//const uint us_crc_calculation = 2000; // According to SDrive-Max
const uint us_cs_calculation_1050 = 270; // According to Altirra
const uint us_cs_calculation_810 = 5136; // According to Altirra
const uint us_track_step_810 = 5300; // number of microseconds drive takes to step 1 track
//const uint us_track_step_1050 = 12410; // According to SDrive-Max
const uint us_track_step_1050 = 20120; // According to Avery / Altirra
const uint us_head_settle_1050 = 20000;
const uint us_head_settle_810 = 10000;
const uint ms_3fake_rot_810 = 1566;
const uint ms_2fake_rot_1050 = 942;
const int max_retries_810 = 4;
const int max_retries_1050 = 2;

const uint8_t mask_fdc_busy = 0x01;
const uint8_t mask_fdc_drq = 0x02;
const uint8_t mask_fdc_dlost = 0x04; // mask for checking FDC status "data lost" bit
const uint8_t mask_fdc_crc = 0x08;
const uint8_t mask_fdc_missing = 0x10; // RNF mask for checking FDC status "missing" bit
const uint8_t mask_fdc_record_type = 0x20;
const uint8_t mask_fdc_write_protect = 0x40;

const uint8_t mask_extended_data = 0x40; // mask for checking FDC status extended data bit
const uint8_t mask_reserved = 0x80;

uint8_t atx_track_size[4]; // number of sectors in each track
uint8_t atx_density[4];
uint32_t gTrackInfo[4][max_track]; // pre-calculated info for each track
uint8_t gCurrentHeadTrack[4];

typedef struct {
	absolute_time_t stamp;
	uint16_t angle;
} head_position_t;

void getCurrentHeadPosition(head_position_t *hp) {
	uint64_t s = get_absolute_time();
	hp->stamp = s;
#ifdef PIO_DISK_COUNTER
	hp->angle = (uint16_t)(au_full_rotation-disk_counter-1);
#else
	hp->angle = (uint16_t)((to_us_since_boot(s) >> 3) % au_full_rotation);
#endif
}

/*
uint16_t incAngularDisplacement(uint16_t start, uint16_t delta) {
	uint16_t ret = start + delta;
	if (ret >= au_full_rotation)
		ret -= au_full_rotation;
	return ret;
}

uint16_t getCurrentHeadPosition() {
	return (uint16_t)(au_full_rotation-disk_counter-1);
}

void waitForAngularPosition(uint16_t pos) {
	// Alternative 1
	while(getCurrentHeadPosition() != pos)
		tight_loop_contents();

	// Alternative 2
	//int32_t to_wait = pos - getCurrentHeadPosition();
	//if(to_wait < 0)
	//	to_wait += au_full_rotation;
	//sleep_us(8*to_wait);

}
*/

bool loadAtxFile(FIL *fil, int atx_drive_number) {
	struct atxFileHeader *fileHeader;
	struct atxTrackHeader *trackHeader;
	uint bytes_read;

	if(f_read(fil, sector_buffer, sizeof(struct atxFileHeader), &bytes_read) != FR_OK || bytes_read != sizeof(struct atxFileHeader))
		return false;

	fileHeader = (struct atxFileHeader *) sector_buffer;
	// The AT8X header should have been checked by now
	if (fileHeader->version != atx_version || fileHeader->minVersion != atx_version)
		return false;

	atx_density[atx_drive_number] = fileHeader->density;
	atx_track_size[atx_drive_number] = (fileHeader->density == atx_medium) ? 26 : 18;
	disk_headers[atx_drive_number].atr_header.sec_size = (fileHeader->density == atx_double) ? 256 : 128;
	gCurrentHeadTrack[atx_drive_number] = 0;

	uint32_t startOffset = fileHeader->startData;
	for (uint8_t track = 0; track < max_track; track++) {
		f_lseek(fil, startOffset);
		if(f_read(fil, sector_buffer, sizeof(struct atxTrackHeader), &bytes_read) != FR_OK || bytes_read != sizeof(struct atxTrackHeader))
			break;
		trackHeader = (struct atxTrackHeader *) sector_buffer;
		gTrackInfo[atx_drive_number][track] = startOffset;
		startOffset += trackHeader->size;
	}
	return true;
}

// volatile int64_t sd_read_time;

bool transferAtxSector(int atx_drive_number, uint16_t num, uint8_t *status, bool op_write=false, bool op_verify=false) {
	struct atxTrackHeader *trackHeader;
	struct atxSectorListHeader *slHeader;
	struct atxSectorHeader *sectorHeader;
	struct atxTrackChunk *extSectorData;

	uint16_t i;
	const size_t si = 256;
	bool r = false;

	uint16_t atx_sector_size = disk_headers[atx_drive_number].atr_header.sec_size;
	bool is1050 = (disk_headers[atx_drive_number].atr_header.temp3 & 0x40) ? false : true;

	// calculate track and relative sector number from the absolute sector number
	uint8_t tgtTrackNumber = (num - 1) / atx_track_size[atx_drive_number];
	uint8_t tgtSectorNumber = (num - 1) % atx_track_size[atx_drive_number] + 1;

	// set initial status (in case the target sector is not found)
	*status = mask_fdc_missing;

	//absolute_time_t head_move_time = get_absolute_time();

	// delay for track stepping if needed
	if (gCurrentHeadTrack[atx_drive_number] != tgtTrackNumber) {
		int diff = tgtTrackNumber - gCurrentHeadTrack[atx_drive_number];
		if (diff > 0)
			diff += (is1050 ? 1 : 0);
		else
			diff = -diff;
		sleep_us(is1050 ? (diff*us_track_step_1050 + us_head_settle_1050) : (diff*us_track_step_810 + us_head_settle_810));
	}

	// set new head track position
	gCurrentHeadTrack[atx_drive_number] = tgtTrackNumber;

	//sleep_until(head_move_time);

	// sample current head position

	head_position_t headPosition;
	getCurrentHeadPosition(&headPosition);
	//uint16_t headPosition = getCurrentHeadPosition();

	// read the track header
	uint32_t currentFileOffset = gTrackInfo[atx_drive_number][tgtTrackNumber];
	uint16_t sectorCount = 0;

	// exit, if track not present
	if (currentFileOffset && mounted_file_transfer(atx_drive_number+1, currentFileOffset, sizeof(struct atxTrackHeader), false, si) == FR_OK) {
		trackHeader = (struct atxTrackHeader *)&sector_buffer[si];
		sectorCount = trackHeader->sectorCount;
	}

	// For "healthy" ATX files the first check should probably be always fail
	if (trackHeader->trackNumber != tgtTrackNumber || atx_density[atx_drive_number] != ((trackHeader->flags & 0x2) ? atx_medium : atx_single))
		sectorCount = 0;

	uint32_t track_header_size = trackHeader->headerSize;

	if(sectorCount) {
		currentFileOffset += track_header_size;
		if (mounted_file_transfer(atx_drive_number+1, currentFileOffset, sizeof(struct atxSectorListHeader), false, si) == FR_OK) {
			slHeader = (struct atxSectorListHeader *)&sector_buffer[si];
			// sector list header is variable length, so skip any extra header bytes that may be present
			currentFileOffset += slHeader->next - sectorCount * sizeof(struct atxSectorHeader);
		}else
			sectorCount = 0;
	}


	uint32_t tgtSectorOffset; // the offset of the target sector data
	uint32_t writeStatusOffset;
	int16_t weakOffset;
	uint retries = is1050 ? max_retries_1050 : max_retries_810;
	uint32_t retryOffset = currentFileOffset;
	uint8_t write_status;
	uint16_t ext_sector_size;
	while (retries > 0) {
		retries--;
		currentFileOffset = retryOffset;
		int pTT;
		uint16_t tgtSectorIndex = 0; // the index of the target sector within the sector list
		tgtSectorOffset = 0;
		writeStatusOffset = 0;
		weakOffset = -1;
		write_status = mask_fdc_missing;
		// iterate through all sector headers to find the target sector
		if (sectorCount && mounted_file_transfer(atx_drive_number+1, currentFileOffset, sectorCount*sizeof(struct atxSectorHeader), false, si) == FR_OK) {
			for (i=0; i < sectorCount; i++) {
				sectorHeader = (struct atxSectorHeader *)&sector_buffer[si+i*sizeof(struct atxSectorHeader)];
				// if the sector is not flagged as missing and its number matches the one we're looking for...
				if (sectorHeader->number == tgtSectorNumber) {
					if(sectorHeader->status & mask_fdc_missing) {
						write_status |= sectorHeader->status;
						currentFileOffset += sizeof(struct atxSectorHeader);
						continue;
					}
					// check if it's the next sector that the head would encounter angularly...
					//int tt = sectorHeader->timev - headPosition;
					int tt = sectorHeader->timev - headPosition.angle;
					if (!tgtSectorOffset || (tt > 0 && pTT < 0) || (tt > 0 && pTT > 0 && tt < pTT) || (tt < 0 && pTT < 0 && tt < pTT)) {
						pTT = tt;
						*status = sectorHeader->status;
						writeStatusOffset = currentFileOffset + 1;
						tgtSectorIndex = i;
						tgtSectorOffset = sectorHeader->data;
					}
				}
				currentFileOffset += sizeof(struct atxSectorHeader);
			}
		}
		uint16_t act_sector_size = atx_sector_size;
		ext_sector_size = 0;
		if (*status & mask_extended_data) {
			// NOTE! the first part of the trackHeader data (stored in sector_buffer) is by now overwritten,
			// but the headerSize field is far enough into the struct to stay untouched!
			currentFileOffset = gTrackInfo[atx_drive_number][tgtTrackNumber] + track_header_size;
			do {
				if (mounted_file_transfer(atx_drive_number+1, currentFileOffset, sizeof(struct atxTrackChunk), false, si) != FR_OK)
					break;
				extSectorData = (struct atxTrackChunk *) &sector_buffer[si];
				if (extSectorData->size) {
					// if the target sector has a weak data flag, grab the start weak offset within the sector data
					if (extSectorData->sectorIndex == tgtSectorIndex) {
						if(extSectorData->type == 0x10) // weak sector
							weakOffset = extSectorData->data;
						else if(extSectorData->type == 0x11) { // extended sector
							ext_sector_size = 128 << extSectorData->data;
							// 1050 waits for long sectors, 810 does not
							if(is1050 ? (ext_sector_size > act_sector_size) : (ext_sector_size < act_sector_size))
								act_sector_size = ext_sector_size;
						}
						break;
					}
					currentFileOffset += extSectorData->size;
				}
			} while (extSectorData->size);
		}
		if(tgtSectorOffset){
			//absolute_time_t t1 = get_absolute_time();
			if(mounted_file_transfer(atx_drive_number+1, gTrackInfo[atx_drive_number][tgtTrackNumber] + tgtSectorOffset, atx_sector_size, op_write, 0) != FR_OK)
				tgtSectorOffset = 0;
			else if(op_verify) {
				if(mounted_file_transfer(atx_drive_number+1, gTrackInfo[atx_drive_number][tgtTrackNumber] + tgtSectorOffset, atx_sector_size, false, 256) != FR_OK
				|| memcmp(sector_buffer, &sector_buffer[256], atx_sector_size))
				tgtSectorOffset = 0;
			}
			//sd_read_time = absolute_time_diff_us(t1, get_absolute_time());
			// sd_read_time = absolute_time_diff_us(headPosition.stamp,get_absolute_time());

			uint16_t au_one_sector_read = (23+act_sector_size)*(atx_density[atx_drive_number] == atx_single ? 8 : 4)+2;
			if(op_verify)
				au_one_sector_read += au_full_rotation;
			sleep_until(delayed_by_us(headPosition.stamp, (au_one_sector_read + pTT + (pTT > 0 ? 0 : au_full_rotation))*8));

			if(*status)
				// TODO This is according to Altirra, but it breaks test J for 1050?!
				//sleep_us(is1050 ? (us_track_step_1050+us_head_settle_1050) : (au_full_rotation*8));
				sleep_us(au_full_rotation*8);
		} else {
			// No matching sector found at all or the track does not match the disk density
			sleep_until(delayed_by_ms(headPosition.stamp, is1050 ? ms_2fake_rot_1050 : ms_3fake_rot_810));
			if(is1050 || retries == 2)
				if(!is1050)
					sleep_us((43+tgtTrackNumber)*us_track_step_810+us_head_settle_810);
				else if(tgtTrackNumber)
					sleep_us((2*tgtTrackNumber+1)*us_track_step_1050+us_head_settle_1050);
		}

		getCurrentHeadPosition(&headPosition);

		if(!*status || (op_write && tgtSectorOffset))
			break;

	}

	*status &= ~(mask_reserved | mask_extended_data);

	if(op_write) {
		if(tgtSectorOffset)
			*status &= ~(mask_fdc_crc | mask_fdc_record_type);
		else
			*status = write_status & ~(mask_reserved | mask_extended_data);
	} else {
		if (*status & mask_fdc_dlost) {
			if(is1050)
				*status |= mask_fdc_drq;
			else {
				*status &= ~(mask_fdc_dlost | mask_fdc_crc);
				*status |= mask_fdc_busy;
			}
		}
		if(!is1050 && (*status & mask_fdc_record_type))
			*status |= mask_fdc_write_protect;
	}

	if (tgtSectorOffset && !*status)
		r = true;

	if(!op_write) {
		// if a weak offset is defined, randomize the appropriate data
		if (weakOffset > -1)
			for (i = (uint16_t) weakOffset; i < atx_sector_size; i++)
				sector_buffer[i] = (uint8_t) get_rand_32();
		sleep_until(delayed_by_us(headPosition.stamp, is1050 ? us_cs_calculation_1050 : us_cs_calculation_810));
		//sleep_us(is1050 ? us_cs_calculation_1050 : us_cs_calculation_810);
	}else if(tgtSectorOffset) {
		if(writeStatusOffset) {
			sector_buffer[si] = *status;
			if(mounted_file_transfer(atx_drive_number+1, writeStatusOffset, 1, true, si) != FR_OK)
				ext_sector_size = 0;
		}
		if(ext_sector_size > atx_sector_size)
			ext_sector_size = ext_sector_size - atx_sector_size;
		else
			ext_sector_size = 0;
		if((*status & mask_fdc_dlost) && ext_sector_size) {
			memset(&sector_buffer[si], 0xFF, 128);
			currentFileOffset = gTrackInfo[atx_drive_number][tgtTrackNumber] + tgtSectorOffset + atx_sector_size;
			while(ext_sector_size) {
				if(mounted_file_transfer(atx_drive_number+1, currentFileOffset, 128, true, si) != FR_OK)
					break;
				currentFileOffset += 128;
				ext_sector_size -= 128;
			}
		}
	}

	// the Atari expects an inverted FDC status byte
	*status = ~(*status);

	return r;
}

volatile bool save_config_flag = false;
volatile bool save_path_flag = false;

const uint32_t flash_save_offset = HW_FLASH_STORAGE_BASE-FLASH_SECTOR_SIZE;
const uint8_t *flash_config_pointer = (uint8_t *)(XIP_BASE+flash_save_offset);
const size_t flash_config_offset = curr_path_len;
const size_t flash_check_sig_offset = flash_config_offset+64;
const uint32_t config_magic = 0xDEADBEEF;

void inline check_and_load_config(bool reset_config) {
	if(!reset_config && *(uint32_t *)&flash_config_pointer[flash_check_sig_offset] == config_magic) {
		memcpy(curr_path, &flash_config_pointer[0], curr_path_len);
		memcpy(current_options, &flash_config_pointer[flash_config_offset], option_count-1);
	} else {
		save_path_flag = true;
		save_config_flag = true;
	}
}

void inline check_and_save_config() {
	if(!save_path_flag && !save_config_flag)
		return;
	memset(sector_buffer, 0, sector_buffer_size);
	memcpy(sector_buffer, save_path_flag ? (uint8_t *)curr_path : &flash_config_pointer[0], curr_path_len);
	memcpy(&sector_buffer[flash_config_offset], save_config_flag ? current_options : (uint8_t *)&flash_config_pointer[flash_config_offset], option_count-1);
	*(uint32_t *)&sector_buffer[flash_check_sig_offset] = config_magic;
	uint32_t ints = save_and_disable_interrupts();
	multicore_lockout_start_blocking();
	flash_range_erase(flash_save_offset, FLASH_SECTOR_SIZE);
	flash_range_program(flash_save_offset, (uint8_t *)sector_buffer, sector_buffer_size);
	multicore_lockout_end_blocking();
	restore_interrupts(ints);
	save_path_flag = false;
	save_config_flag = false;
}

//char txt_buf[25] = {0};

const char * const str_int_flash = "Int. FLASH";
const char * const str_sd_card = "SD/MMC Card";

char sd_label[12] = {0};

//FRESULT f_mount_result;

uint8_t try_mount_sd() {
	if(f_mount(&fatfs[1], volume_names[1], 1) == FR_OK) {
		DWORD sn;
		if(f_getlabel(volume_names[1], sd_label, &sn) != FR_OK || !sd_label[0])
			strcpy(sd_label, str_sd_card);
		green_blinks = 4;
		sd_card_present = 1;
	}else
		sd_card_present = 0;
	return sd_card_present;
}

volatile int last_drive = -1;
volatile uint32_t last_drive_access = 0;

void update_last_drive(uint drive_number) {
	last_drive = drive_number;
	last_drive_access = to_ms_since_boot(get_absolute_time());
}


#include "disk_images_data.h"

int last_access_error_drive = -1;
bool last_access_error[5] = {false};

void set_last_access_error(int d) {
	last_access_error_drive = d;
	last_access_error[last_access_error_drive] = true;
}

void main_sio_loop(uint sm, uint sm_turbo) {
	//FATFS fatfs;
	FIL fil;
	FILINFO fil_info;
	FSIZE_t offset;
	FSIZE_t to_read;
	FRESULT f_op_stat;
	int i;
	uint bytes_read;
	absolute_time_t last_sd_check = get_absolute_time();
	while(true) {
		uint8_t cd_temp = gpio_get(9);
		// Debounce 500ms - can it be smaller?
		if(cd_temp != sd_card_present && absolute_time_diff_us(last_sd_check, get_absolute_time()) > 500000) {
			last_sd_check = get_absolute_time();
			mutex_enter_blocking(&mount_lock);
			if(cd_temp)
				try_mount_sd();
			else {
				red_blinks = 4;
				if(!curr_path[0])
					last_file_name[0] = 0;
				/*
				if(!curr_path[0] || curr_path[0] == '1') {
					last_file_name[0] = 0;
					curr_path[0] = 0;
					// strcpy(curr_path, volume_names[0]);
				}
				*/
				sd_card_present = 0;
				//sd_card_t *p_sd = sd_get_by_num(1);
				//p_sd->sd_test_com(p_sd);
				// This one is enough (way quicker)
				sd_get_by_num(1)->m_Status |= STA_NOINIT;
			}
			cd_temp = sd_card_present ^ 1;
			mutex_exit(&mount_lock);
		} else {
			// last_sd_check = get_absolute_time();
			cd_temp = 0;
		}
		if(cd_temp || last_access_error_drive >= 0) {
			mutex_enter_blocking(&mount_lock);
			for(i=0; i<5; i++) {
				if(last_access_error[i] || (cd_temp && mounts[i].mount_path[0] == '1')) {
					mounts[i].status = 0;
					mounts[i].mounted = false;
					mounts[i].mount_path[0] = 0;
					strcpy(&mounts[i].str[i ? 3 : 2],"  <EMPTY>   ");
					/*
						red_blinks = 0;
						green_blinks = 0;
						*/
					blue_blinks = 0;
					update_rgb_led(false);
				}
				last_access_error[i] = false;
			}
			last_access_error_drive = -1;
			mutex_exit(&mount_lock);
		}
		for(i=0; i<5; i++) {
			mutex_enter_blocking(&mount_lock);
			if(!mounts[i].mounted || mounts[i].status) {
				mutex_exit(&mount_lock);
				continue;
			}
			mutex_enter_blocking(&fs_lock);
			if(/*f_mount(&fatfs[0], (const char *)mounts[i].mount_path, 1) == FR_OK && */ f_stat((const char *)mounts[i].mount_path, &fil_info) == FR_OK && f_open(&fil, (const char *)mounts[i].mount_path, FA_READ) == FR_OK) {
				if(!i) {
					check_turbo_conf();
					cas_sample_duration = timing_base_clock/600;
					cas_size = f_size(&fil);
					if(f_read(&fil, &cas_header, sizeof(cas_header_type), &bytes_read) == FR_OK &&
							bytes_read == sizeof(cas_header_type) &&
							cas_header.signature == cas_header_FUJI)
						mounts[i].status = cas_read_forward(&fil, cas_header.chunk_length + sizeof(cas_header_type));
					if(!mounts[i].status)
						set_last_access_error(i);
					else if(last_drive == 0)
							last_drive = -1;
				} else {
					uint8_t disk_type = 0;
					if(f_read(&fil, sector_buffer, 4, &bytes_read) == FR_OK && bytes_read == 4) {
						if(*(uint16_t *)sector_buffer == 0x0296) // ATR magic
							disk_type = disk_type_atr;
						else if(*(uint16_t *)sector_buffer == 0xFFFF)
							disk_type = disk_type_xex;
						else if(*(uint32_t *)sector_buffer == 0x58385441) // AT8X
							disk_type = disk_type_atx;
						f_lseek(&fil, 0);
					}
					switch(disk_type) {
						case disk_type_atr:
							if(f_read(&fil, &disk_headers[i-1].atr_header, sizeof(atr_header_type), &bytes_read) == FR_OK && bytes_read == sizeof(atr_header_type)) {
								mounts[i].status = (disk_headers[i-1].atr_header.pars | ((disk_headers[i-1].atr_header.pars_high << 16) & 0xFF0000)) << 4;
								disk_headers[i-1].atr_header.temp2 = 0xFF;
								if(!current_options[mount_option_index] || (fil_info.fattrib & AM_RDO))
									disk_headers[i-1].atr_header.flags |= 0x1;
								disk_headers[i-1].atr_header.temp3 = locate_percom(i);
							} else
								disk_type = 0;
							break;
						case disk_type_xex:
							// Sectors occupied by the file itself
							offset = f_size(&fil);
							disk_headers[i-1].atr_header.pars = (offset+124)/125;
							disk_headers[i-1].atr_header.pars_high = offset % 125;
							if(!disk_headers[i-1].atr_header.pars_high)
								disk_headers[i-1].atr_header.pars_high = 125;
							// Incidentally this can be a proper SD or ED disk,
							// but this is perfectly OK
							mounts[i].status = (disk_headers[i-1].atr_header.pars+3+0x170)*128;
							disk_headers[i-1].atr_header.sec_size = 128;
							disk_headers[i-1].atr_header.flags = 0x1;
							disk_headers[i-1].atr_header.temp2 = 0xFF;
							disk_headers[i-1].atr_header.temp3 = 0x80;
							break;
						case disk_type_atx:
							if(loadAtxFile(&fil, i-1)) {
								mounts[i].status = 40*atx_track_size[i-1]*disk_headers[i-1].atr_header.sec_size-(disk_headers[i-1].atr_header.sec_size == 256 ? 384 : 0);
								disk_headers[i-1].atr_header.pars = mounts[i].status >> 4;
								disk_headers[i-1].atr_header.pars_high = 0x00;
								disk_headers[i-1].atr_header.flags = 0;
								if(!current_options[mount_option_index] || (fil_info.fattrib & AM_RDO))
									disk_headers[i-1].atr_header.flags |= 0x1;
								disk_headers[i-1].atr_header.temp2 = 0xFF;
								disk_headers[i-1].atr_header.temp3 = locate_percom(i) | (current_options[atx_option_index] ? 0x40 : 0x00);
							}else
								disk_type = 0;
							break;
						default:
							break;
					}
					if(!disk_type)
						set_last_access_error(i);
					else {
						disk_headers[i-1].atr_header.temp4 = disk_type;
						if(last_drive == i)
							last_drive = -1;
					}
				}
				f_close(&fil);
			}else
				set_last_access_error(i);
			//f_mount(0, (const char *)mounts[i].mount_path, 1);
			mutex_exit(&fs_lock);
			mutex_exit(&mount_lock);
		}

		// command_line_pin, GPIO_IRQ_EDGE_FALL
		if(try_get_sio_command()) {
			//sio_command_received = false;
			int drive_number = sio_command.device_id-0x30;
			mutex_enter_blocking(&mount_lock);
			uint8_t r = 'A';
			uint64_t us_pre_ce = 300;
			if(drive_number < 1 || drive_number > 4 || !mounts[drive_number].mounted || last_access_error[drive_number])
				goto ignore_sio_command_frame;
			sleep_us(100); // Needed for BiboDos according to atari_drive_emulator.c
			gpio_set_function(sio_tx_pin, GPIO_FUNC_UART);
			memset(sector_buffer, 0, sector_buffer_size);
			blue_blinks = (high_speed == 1) ? -1 : 0;
			update_rgb_led(false);
			update_last_drive(drive_number);
			disk_headers[drive_number-1].atr_header.temp1 = 0x0;
			f_op_stat = FR_OK;
			to_read = 0;
			switch(sio_command.command_id) {
				case 'S': // get status
					uart_putc_raw(uart1, r);
					to_read = 4;
					// TODO motor on only after first actual disk operation?
					// for not mounted drives report motor on but no disk (drive always present,
					// not only when the disk is in), or???
					sector_buffer[0] = disk_headers[drive_number-1].atr_header.temp1;
					sector_buffer[1] = disk_headers[drive_number-1].atr_header.temp2;
					//if(mounts[drive_number].mounted) {
					sector_buffer[0] |= 0x10; // motor on
					if(disk_headers[drive_number-1].atr_header.flags & 0x1)
						sector_buffer[0] |= 0x08; // write protect
					if(disk_headers[drive_number-1].atr_header.sec_size == 256) {
						sector_buffer[0] |= 0x20;
						if(disk_headers[drive_number-1].atr_header.temp3 & 0x03 == 0x03)
							sector_buffer[0] |= 0x40;
					} else if(mounts[drive_number].status == 0x20800) // 1040*128
						sector_buffer[0] |= 0x80; // medium density
					//} else {
					//	sector_buffer[1] &= 0x7F; // disk removed
					//}
					sector_buffer[2] = 0xe0;
					sector_buffer[3] = 0x0;
					break;
				case 'N': // read percom
					uart_putc_raw(uart1, r);
					if(disk_headers[drive_number-1].atr_header.temp3 & 0x80) {
						sector_buffer[0] = 1; // # tracks
						sector_buffer[1] = 3; // step rate
						offset = disk_headers[i-1].atr_header.sec_size;
						sector_buffer[6] = (offset >> 8) & 0xFF; // bytes / sec high
						sector_buffer[7] = offset & 0xFF; // bytes / sec low
						offset = 3+(mounts[i].status-384)/offset;
						sector_buffer[2] = (offset >> 8) & 0xFF; // # sectors high
						sector_buffer[3] = (offset & 0xFF); // # sectors low
						sector_buffer[4] = 0; // # sides
						sector_buffer[5] = 4; // record method
					} else {
						memcpy(sector_buffer, &percom_table[(disk_headers[drive_number-1].atr_header.temp3 & 0x3)*13], 8);
					}
					sector_buffer[8] = 0xFF;
					memset(&sector_buffer[9], 0x00, 3);
					to_read = 12;
					break;
				case 'R': // read sector
					switch(disk_headers[drive_number-1].atr_header.temp4) {
						case disk_type_atr:
							r = check_drive_and_sector_status(drive_number, &offset, &to_read);
							uart_putc_raw(uart1, r);
							if(r == 'N') break;
							green_blinks = -1;
							update_rgb_led(false);
							if((f_op_stat = mounted_file_transfer(drive_number, sizeof(atr_header_type)+offset, to_read , false)) != FR_OK) {
								set_last_access_error(drive_number);
								disk_headers[drive_number-1].atr_header.temp2 &= 0xEF;
							}
							else
								disk_headers[drive_number-1].atr_header.temp2 = 0xFF;
							break;
						case disk_type_xex:
							r = check_drive_and_sector_status(drive_number, &offset, &to_read);
							uart_putc_raw(uart1, r);
							if(r == 'N') break;
							green_blinks = -1;
							update_rgb_led(false);
							if(sio_command.sector_number >= 0x171) {
									offset = (sio_command.sector_number-0x171);
									if(offset == disk_headers[drive_number-1].atr_header.pars - 1) {
										to_read = disk_headers[drive_number-1].atr_header.pars_high;
									} else {
										sector_buffer[125]=((sio_command.sector_number+1)>>8) & 0xFF;
										sector_buffer[126]=((sio_command.sector_number+1) & 0xFF);
										to_read = 125;
									}
									sector_buffer[127] = to_read;
									offset *= 125;
									if((f_op_stat = mounted_file_transfer(drive_number, offset, to_read , false)) != FR_OK) {
										set_last_access_error(drive_number);
										disk_headers[drive_number-1].atr_header.temp2 &= 0xEF;
									}else
										disk_headers[drive_number-1].atr_header.temp2 = 0xFF;
									to_read = 128;
							} else {
								if(sio_command.sector_number <= 2) {
									offset = 128*(sio_command.sector_number-1);
									memcpy(sector_buffer, &boot_loader[offset], 128);
									if(boot_reloc_delta[current_options[xex_option_index]])
										for(i=0; i<boot_reloc_locs_size; i++)
											if(boot_reloc_locs[i] >= offset && boot_reloc_locs[i] < offset + 128)
												sector_buffer[boot_reloc_locs[i]-offset] += boot_reloc_delta[current_options[xex_option_index]];
								} else {
									if(sio_command.sector_number == 0x168) { // VTOC
										uint total_sectors = mounts[drive_number].status >> 7;
										uint vtoc_sectors = total_sectors >> 10;
										uint rem = total_sectors - (vtoc_sectors << 10);
										if(rem > 943) vtoc_sectors += 2;
										else if(rem) vtoc_sectors++;
										if(!(vtoc_sectors % 2)) vtoc_sectors++;
										total_sectors -= (vtoc_sectors + 12);
										sector_buffer[0] = (uint8_t)((vtoc_sectors + 3)/2);
										sector_buffer[1] = (total_sectors & 0xFF);
										sector_buffer[2] = ((total_sectors >> 8) & 0xFF);
									}else if(sio_command.sector_number == 0x169) { // Directory
										uint file_sectors = disk_headers[drive_number-1].atr_header.pars;
										sector_buffer[0] = (file_sectors > 0x28F) ? 0x46 : 0x42;
										sector_buffer[1] = (file_sectors & 0xFF);
										sector_buffer[2] = ((file_sectors >> 8) & 0xFF);
										sector_buffer[3] = 0x71;
										sector_buffer[4] = 0x01;
										// The file name is guaranteed to have an extension that is 3 characters long
										memset(&sector_buffer[5], ' ', 8);
										offset = 0;
										for(i=0; i<11; i++) {
											uint8_t c = mounts[drive_number].str[3+offset];
											if(c == '.')
												i = 7;
											else {
												// 'a'-1 or > 'z' -> '@'
												// To uppercase: 'a'..'z' -> -32 Needed?
												if(c == 'a'-1 || c > 'z') c = '@';
												sector_buffer[5+i] = c;
											}
											offset++;
										}
									}
								}
							}
							break;
						case disk_type_atx:
							// delay for the time the drive takes to process the request
							sleep_us(us_drive_request_delay);
							if(sio_command.sector_number == 0 || sio_command.sector_number > 40*atx_track_size[drive_number-1]) {
								disk_headers[drive_number-1].atr_header.temp2 &= 0xEF;
								r = 'N';
							}
							uart_putc_raw(uart1, r);
							if (r == 'N')
								break;
							green_blinks = -1;
							update_rgb_led(false);
							to_read = disk_headers[drive_number-1].atr_header.sec_size;
							us_pre_ce = 0; // Handled in loadAtxSector
							// TODO Distinguish between emulated error and FLASH/SD access error
							if(!transferAtxSector(drive_number-1, sio_command.sector_number, &disk_headers[drive_number-1].atr_header.temp2))
								f_op_stat = FR_INT_ERR;
							// break;
						default:
							break;
					}
					green_blinks = 0;
					update_rgb_led(false);
					break;
				case 'O': // write percom
					// Only writable disks react to PERCOM write command frame
					if((disk_headers[drive_number-1].atr_header.flags & 0x1) || (disk_headers[drive_number-1].atr_header.temp3 & 0x80))
						r = 'N';
					uart_putc_raw(uart1, r);
					if(r == 'N') break;
					r = try_receive_data(drive_number, 12);
					//for(i=0;i<12;i++)
					//	sprintf(&txt_buf[2*i], "%02X", sector_buffer[i]);
					if(r == 'A' && !compare_percom(drive_number))
						r = 'N';
					sleep_us(850);
					uart_putc_raw(uart1, r);
					if(r == 'N') break;
					 // Mark that PERCOM has been written successfully
					disk_headers[drive_number-1].atr_header.temp3 |= 0x04;
					break;
				case 'P': // put sector
				case 'W': // write (+verify) sector
					switch(disk_headers[drive_number-1].atr_header.temp4) {
						case disk_type_atr:
							r = check_drive_and_sector_status(drive_number, &offset, &to_read, true);
							uart_putc_raw(uart1, r);
							if(r == 'N')
								break;
							r = try_receive_data(drive_number, to_read);
							sleep_us(850);
							uart_putc_raw(uart1, r);
							if(r == 'N')
								break;
							red_blinks = -1;
							update_rgb_led(false);
							if((f_op_stat = mounted_file_transfer(drive_number, sizeof(atr_header_type)+offset, to_read, true)) != FR_OK) {
								disk_headers[drive_number-1].atr_header.temp1 |= 0x4; // write error
								set_last_access_error(drive_number);
							} else if (sio_command.command_id == 'W') {
								if((f_op_stat = mounted_file_transfer(drive_number, sizeof(atr_header_type)+offset, to_read, false, to_read)) != FR_OK)
									set_last_access_error(drive_number);
								else if(memcmp(sector_buffer, &sector_buffer[to_read], to_read)) {
									f_op_stat = FR_INT_ERR;
									disk_headers[drive_number-1].atr_header.temp1 |= 0x4; // write error
								}
							}
							to_read = 0;
							break;
						case disk_type_atx:
							sleep_us(us_drive_request_delay);
							if(sio_command.sector_number == 0 || sio_command.sector_number > 40*atx_track_size[drive_number-1]) {
								disk_headers[drive_number-1].atr_header.temp2 &= 0xEF;
								r = 'N';
							}
							if(disk_headers[drive_number-1].atr_header.flags & 0x1) {
								disk_headers[drive_number-1].atr_header.temp2 &= 0xBF;
								r = 'N';
							}
							uart_putc_raw(uart1, r);
							if (r == 'N')
								break;
							to_read = disk_headers[drive_number-1].atr_header.sec_size;
							r = try_receive_data(drive_number, to_read);
							sleep_us(850);
							uart_putc_raw(uart1, r);
							if(r == 'N')
								break;
							red_blinks = -1;
							update_rgb_led(false);
							// TODO Distinguish between the emulated error and Flash/SD access error
							if(!transferAtxSector(drive_number-1, sio_command.sector_number, &disk_headers[drive_number-1].atr_header.temp2, true, sio_command.command_id == 'W')) {
								f_op_stat = FR_INT_ERR;
								disk_headers[drive_number-1].atr_header.temp1 |= 0x4; // write error
							}

							to_read = 0;
							// break;
						case disk_type_xex:
						default:
							break;
					}
					red_blinks = 0;
					update_rgb_led(false);
					break;
				case '!': // Format SD / PERCOM
				case '"': // Format ED
					switch(disk_headers[drive_number-1].atr_header.temp4) {
					case disk_type_atr:
						i = sio_command.command_id - 0x21;
						if( /* !mounts[drive_number].mounted || */
							(disk_headers[drive_number-1].atr_header.flags & 0x1) ||
							(i != (disk_headers[drive_number-1].atr_header.temp3 & 0x3)
								&& !(disk_headers[drive_number-1].atr_header.temp3 & 0x4)))
							r = 'N';
						uart_putc_raw(uart1, r);
						if(r == 'N') break;
						red_blinks = -1;
						update_rgb_led(false);
						to_read = disk_headers[drive_number-1].atr_header.sec_size;
						memset(sector_buffer, 0, to_read);
						if((f_op_stat = mounted_file_transfer(drive_number, sizeof(atr_header_type)+i*128, 128, true,0,mounts[drive_number].status >> 7)) != FR_OK)
							set_last_access_error(drive_number);
						sector_buffer[0] = sector_buffer[1] = 0xFF;
						break;
					case disk_type_xex:
					case disk_type_atx:
						r = 'N';
						uart_putc_raw(uart1, r);
						// break;
					default:
						break;
					}
					red_blinks = 0;
					update_rgb_led(false);
					break;
				case '?': // get speed index
					if(!current_options[hsio_option_index] || disk_headers[drive_number-1].atr_header.temp4 == disk_type_atx)
						r = 'N';
					uart_putc_raw(uart1, r);
					if(r == 'N') break;
					sector_buffer[0] = hsio_opt_to_index[current_options[hsio_option_index]];
					to_read = 1;
					break;
				default:
					r = 'N';
					uart_putc_raw(uart1, r);
					break;
			}
			if(r == 'A') {
				uart_tx_wait_blocking(uart1);
				sleep_us(us_pre_ce); // Again, timeout mentioned by atari_drive_emulator.c
				uart_putc_raw(uart1, f_op_stat == FR_OK ? 'C' : 'E');
				if(to_read) {
					uart_tx_wait_blocking(uart1);
					sleep_us(150); // Another one from atari_drive_emulator.c
					uart_write_blocking(uart1, sector_buffer, to_read);
					uart_putc_raw(uart1, sio_checksum(sector_buffer, to_read));
				}
				uart_tx_wait_blocking(uart1);
				if(sio_command.command_id == '?') {
					high_speed = 1;
					r = current_options[hsio_option_index];
					uart_set_baudrate(uart1, current_options[clock_option_index] ? hsio_opt_to_baud_ntsc[r] : hsio_opt_to_baud_pal[r]);
				}
			}
ignore_sio_command_frame:
			blue_blinks = 0;
			update_rgb_led(false);
			mutex_exit(&mount_lock);
		} else if(mounts[0].mounted && (offset = mounts[0].status) && offset < cas_size &&
			(cas_block_turbo ? turbo_motor_value_on : normal_motor_value_on) ==
				(gpio_get_all() & (cas_block_turbo ? turbo_motor_pin_mask : normal_motor_pin_mask))) {
			mutex_enter_blocking(&mount_lock);
			to_read = std::min(cas_header.chunk_length-cas_block_index, (cas_block_turbo ? 128 : 256)*cas_block_multiple);
			if(mounted_file_transfer(0, offset, to_read, false) != FR_OK) {
				set_last_access_error(0);
				mutex_exit(&mount_lock);
				continue;
			}
			update_last_drive(0);
			green_blinks = -1;
			blue_blinks = cas_block_turbo ? -1 : 0;
			update_rgb_led(false);
			offset += to_read;
			cas_block_index += to_read;
			mounts[0].status = offset;
			gpio_set_function(sio_tx_pin, GPIO_FUNC_PIOX);
			uint8_t silence_bit = (cas_block_turbo ? 0 : 1);
			while(silence_duration > 0) {
				uint16_t silence_block_len = silence_duration;
				if(silence_block_len >= max_clock_ms)
					silence_block_len = max_clock_ms;
				pio_enqueue(silence_bit, (timing_base_clock/1000)*silence_block_len);
				silence_duration -= silence_block_len;
			}
			int bs, be, bd;
			uint8_t b;
			uint16_t ld;
			for(i=0; i < to_read; i += cas_block_multiple) {
				switch(cas_header.signature) {
					case cas_header_data:
						pio_enqueue(0, cas_sample_duration);
						b = sector_buffer[i];
						for(int j=0; j!=8; j++) {
							pio_enqueue(b & 0x1, cas_sample_duration);
							b >>= 1;
						}
						pio_enqueue(1, cas_sample_duration);
						break;
					case cas_header_fsk:
					case cas_header_pwml:
						ld = *(uint16_t *)&sector_buffer[i];
						// ld = (sector_buffer[i] & 0xFF) | ((sector_buffer[i+1] << 8) & 0xFF00);
						if(ld != 0) {
							pio_enqueue(cas_fsk_bit, (cas_block_turbo ? 2*pwm_sample_duration : (timing_base_clock/10000))*ld);
						}
						cas_fsk_bit ^= 1;
						break;
					case cas_header_pwmc:
						ld = (sector_buffer[i+1] & 0xFF) | ((sector_buffer[i+2] << 8) & 0xFF00);
						for(uint16_t j=0; j<ld; j++) {
							pio_enqueue(pwm_bit, sector_buffer[i]*pwm_sample_duration);
							pio_enqueue(pwm_bit^1, sector_buffer[i]*pwm_sample_duration);
						}
						break;
					case cas_header_pwmd:
						b = sector_buffer[i];
						if (pwm_bit_order) {
							bs=7; be=-1; bd=-1;
						} else {
							bs=0; be=8; bd=1;
						}
						for(int j=bs; j!=be; j += bd) {
							uint8_t d = cas_header.aux.aux_b[(b >> j) & 0x1];
							pio_enqueue(pwm_bit, d*pwm_sample_duration);
							pio_enqueue(pwm_bit^1, d*pwm_sample_duration);
						}
						break;
					default:
						break;
				}
			}
			if(cas_block_index == cas_header.chunk_length) {
				if(offset < cas_size && mounts[0].mounted) {
					mutex_enter_blocking(&fs_lock);
					if(/* f_mount(&fatfs[0], (const char *)mounts[0].mount_path, 1) == FR_OK && */ f_open(&fil, (const char *)mounts[0].mount_path, FA_READ) == FR_OK) {
						offset = cas_read_forward(&fil, offset);
					}
					f_close(&fil);
					// f_mount(0, (const char *)mounts[0].mount_path, 1);
					mutex_exit(&fs_lock);
					mounts[0].status = offset;
					if(!offset)
						set_last_access_error(0);
					else if(cas_header.signature == cas_header_pwmc || cas_header.signature == cas_header_data || (cas_header.signature == cas_header_fsk && silence_duration) || dma_block_turbo^cas_block_turbo) {
						while(!pio_sm_is_tx_fifo_empty(cas_pio, dma_block_turbo ? sm_turbo : sm))
						tight_loop_contents();
						// This is to account for the possible motor off switching lag
						sleep_ms(10);
					}
				}else{
					blue_blinks = 0;
					update_rgb_led(false);
				}
			}
			green_blinks = 0;
			update_rgb_led(false);
			mutex_exit(&mount_lock);
		}else if(create_new_file > 0 && last_drive == -1) {
			uint8_t new_file_size_index = (create_new_file & 0xF)-1; // SD ED DD QD
			uint8_t new_file_format_index = ((create_new_file >> 4 ) & 0xF)-1; // None DOS MyDOS Sparta
			uint32_t image_size = image_sizes[new_file_size_index];
			bool new_file_boot = ((create_new_file >> 8 ) & 0x1);
			f_op_stat = FR_OK;
			uint disk_image_size = disk_images_size[new_file_size_index][new_file_format_index];
			memcpy(sector_buffer, (uint8_t *)(disk_images_data[new_file_size_index][new_file_format_index]), disk_image_size);
			if(new_file_boot)
				memcpy(&sector_buffer[256], dummy_boot, dummy_boot_len);
			uint32_t bs;
			uint32_t ints;
			uint vol_num = temp_array[0]-'0';
			if(!vol_num) {
				ints = save_and_disable_interrupts();
				multicore_lockout_start_blocking();
			}
			do {
				//if((f_op_stat = f_mount(&fatfs[0], temp_array, 1)) != FR_OK)
				//	break;
#if FF_MAX_SS != FF_MIN_SS
				bs = fatfs[vol_num].csize * fatfs[vol_num].ssize;
#else
				bs = fatfs[vol_num].csize * FF_MAX_SS;
#endif
				image_size = (image_size + bs - 1) / bs;
				FATFS *ff;
				if((f_op_stat = f_getfree(temp_array, &bs, &ff)) != FR_OK)
					break;
				if(bs < image_size) {
					f_op_stat = FR_INT_ERR;
					break;
				}
				if((f_op_stat = f_open(&fil, temp_array, FA_CREATE_NEW | FA_WRITE)) != FR_OK)
					break;
				uint ind=0;
				while(ind < disk_image_size) {
					memcpy(&bs, &sector_buffer[ind], 4);
					ind += 4;
					bool nrb = (bs & 0x80000000) ? true : false;
					bs &= 0x7FFFFFFF;
					while(bs > 0) {
						if(f_putc(sector_buffer[ind], &fil) == -1) {
							f_op_stat = FR_INT_ERR;
							break;
						}
						if(nrb) ind++;
						bs--;
					}
					if(f_op_stat != FR_OK)
						break;
					if(!nrb) ind++;
				}
				if(f_op_stat != FR_OK || !new_file_boot)
					break;
				if((f_op_stat = f_lseek(&fil, 16)) != FR_OK)
					break;
				f_op_stat = f_write(&fil, &sector_buffer[256], dummy_boot_len, &ind);
			} while(false);
			f_close(&fil);
			//f_mount(0, temp_array, 1);
			if(!vol_num) {
				multicore_lockout_end_blocking();
				restore_interrupts(ints);
			}
			create_new_file = (f_op_stat != FR_OK) ? -1 : 0;
			/*
			if(f_op_stat != FR_OK) {
				create_new_file = -1;
				// Access error was not Atari drive specific
				last_access_error_drive = 5;
			}else
				create_new_file = 0;
			*/
		}else if(last_drive == -1)
			check_and_save_config();
	}
}

void core1_entry() {

	//timing_base_clock = clock_get_hz(clk_sys);
	timing_base_clock = 1000000;
	max_clock_ms = 0x7FFFFFFF/(timing_base_clock/1000)/1000*1000;

	gpio_init(joy2_p3_pin); gpio_set_dir(joy2_p3_pin, GPIO_IN); gpio_pull_up(joy2_p3_pin);

	gpio_init(normal_motor_pin); gpio_set_dir(normal_motor_pin, GPIO_IN);

	// Pico2 bug! This pull down will lock the pin state high after the first read, instead
	// one has to use an external pull down resistor of not too high value, for example 4.7kohm
	// (reports sugguest that it has to be below 9kohm)
#ifndef RASPBERRYPI_PICO2
#if MOTOR_ON_STATE == 1
	gpio_pull_down(normal_motor_pin);
#endif
#endif
	gpio_init(command_line_pin); gpio_set_dir(command_line_pin, GPIO_IN); gpio_pull_up(command_line_pin);

	queue_init(&pio_queue, sizeof(uint32_t), pio_queue_size);

#ifdef PIO_DISK_COUNTER
	uint disk_pio_offset = pio_add_program(disk_pio, &disk_counter_program);
	float disk_clk_divider = (float)clock_get_hz(clk_sys)/1000000;
	uint disk_sm = pio_claim_unused_sm(disk_pio, true);
	pio_sm_config disk_sm_config = pin_io_program_get_default_config(disk_pio_offset);
	sm_config_set_clkdiv(&disk_sm_config, disk_clk_divider);
	sm_config_set_in_shift(&disk_sm_config, true, false, 32);
	sm_config_set_out_shift(&disk_sm_config, true, true, 32);
	pio_sm_init(disk_pio, disk_sm, disk_pio_offset, &disk_sm_config);

	disk_dma_channel = dma_claim_unused_channel(true);
	dma_channel_config disk_dma_c = dma_channel_get_default_config(disk_dma_channel);
	channel_config_set_transfer_data_size(&disk_dma_c, DMA_SIZE_32);
	channel_config_set_read_increment(&disk_dma_c, false);
	channel_config_set_write_increment(&disk_dma_c, false);
	channel_config_set_dreq(&disk_dma_c, pio_get_dreq(disk_pio, disk_sm, false));
	pio_sm_set_enabled(disk_pio, disk_sm, true);
	dma_channel_set_irq0_enabled(disk_dma_channel, true);

	irq_add_shared_handler(DMA_IRQ_0, disk_dma_handler, PICO_SHARED_IRQ_HANDLER_DEFAULT_ORDER_PRIORITY);
	irq_set_enabled(DMA_IRQ_0, true);

	// Pico 2 does not like the "full" 0x80000000 for transfer counter, the core freezes
	dma_channel_configure(disk_dma_channel, &disk_dma_c, &disk_counter, &disk_pio->rxf[disk_sm], 0x8000000, true);
	// dma_channel_configure(disk_dma_channel, &disk_dma_c, &disk_counter, &disk_pio->rxf[disk_sm], 1, true);

	disk_pio->txf[disk_sm] = (au_full_rotation-1);
#endif

	pio_offset = pio_add_program(cas_pio, &pin_io_program);
 	float clk_divider = (float)clock_get_hz(clk_sys)/timing_base_clock;


	sm = pio_claim_unused_sm(cas_pio, true);
	pio_gpio_init(cas_pio, sio_tx_pin);
	pio_sm_set_consecutive_pindirs(cas_pio, sm, sio_tx_pin, 1, true);
	pio_sm_config c = pin_io_program_get_default_config(pio_offset);
	sm_config_set_fifo_join(&c, PIO_FIFO_JOIN_TX);
	sm_config_set_clkdiv(&c, clk_divider);
	sm_config_set_out_pins(&c, sio_tx_pin, 1);
	sm_config_set_out_shift(&c, true, true, 32);
	pio_sm_init(cas_pio, sm, pio_offset, &c);
	pio_sm_set_enabled(cas_pio, sm, true);

	sm_turbo = pio_claim_unused_sm(cas_pio, true);

	sm_config_turbo = pin_io_program_get_default_config(pio_offset);
	sm_config_set_fifo_join(&sm_config_turbo, PIO_FIFO_JOIN_TX);
	sm_config_set_clkdiv(&sm_config_turbo, clk_divider);
	sm_config_set_out_shift(&sm_config_turbo, true, true, 32);

	check_turbo_conf();

	dma_channel = dma_claim_unused_channel(true);
	dma_channel_config dma_c = dma_channel_get_default_config(dma_channel);
	channel_config_set_transfer_data_size(&dma_c, DMA_SIZE_32);
	channel_config_set_read_increment(&dma_c, false);
	channel_config_set_dreq(&dma_c, pio_get_dreq(cas_pio, sm, true));
	dma_channel_configure(dma_channel, &dma_c, &cas_pio->txf[sm], &pio_e, 1, false);
	dma_channel_set_irq1_enabled(dma_channel, true);

	dma_channel_turbo = dma_claim_unused_channel(true);
	dma_channel_config dma_c1 = dma_channel_get_default_config(dma_channel_turbo);
	channel_config_set_transfer_data_size(&dma_c1, DMA_SIZE_32);
	channel_config_set_read_increment(&dma_c1, false);
	channel_config_set_dreq(&dma_c1, pio_get_dreq(cas_pio, sm_turbo, true));
	dma_channel_configure(dma_channel_turbo, &dma_c1, &cas_pio->txf[sm_turbo], &pio_e, 1, false);
	dma_channel_set_irq1_enabled(dma_channel_turbo, true);

	irq_set_exclusive_handler(DMA_IRQ_1, dma_handler);

	irq_set_enabled(DMA_IRQ_1, true);

	uart_init(uart1, current_options[clock_option_index] ? hsio_opt_to_baud_ntsc[0] : hsio_opt_to_baud_pal[0]);
	gpio_set_function(sio_tx_pin, GPIO_FUNC_UART);
	gpio_set_function(sio_rx_pin, GPIO_FUNC_UART);
	uart_set_hw_flow(uart1, false, false);
	uart_set_fifo_enabled(uart1, false);
	uart_set_format(uart1, 8, 1, UART_PARITY_NONE);

	// So far I was not able to make it work through the notification interrupt...
	// gpio_set_irq_enabled_with_callback(command_line_pin, GPIO_IRQ_EDGE_FALL, true, try_get_sio_command);
	main_sio_loop(sm, sm_turbo);
}

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
const size_t menu_entry_size = 9;

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
	print_text(menu_entries[i].str, i==cursor_position ? menu_entries[i].wd : 0);
}

const uint16_t cas_pg_width = 208;
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
	char *f = file_entries[fi].long_name;
	size_t ei = get_filename_ext(f) - 1;
	bool ext = (f[ei] == '.');
	int pe = 8;
	if (file_entries[fi].dir) {
		if(!ext) pe = 12;
		if(ei < pe) pe = ei;
		std::string s2(&f[ei]);
		text_location.x += pe*8*font_scale;
		print_text(s2, i == cursor_position ? s2.size() : 0);
		text_location.x -= pe*8*font_scale;
	} else {
		text_location.x += 8*8*font_scale;
		if(ext)
			print_text(std::string(&f[ei]), i == cursor_position ? 4 : 0);
		else {
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
	}else
		ptr_str_file_name = new std::string(f, ei);
	if(i == cursor_position) {
		print_text((*ptr_str_file_name).substr(0,pe), pe);
		if(ei > pe) {
			init_scroll_long_filename(ptr_str_file_name, text_location.x, text_location.y, pe, ei);
			pe = 0;
		}
	} else
		print_text(*ptr_str_file_name);
	if(pe)
		delete ptr_str_file_name;
}

void update_one_display_file2(int page_index, int shift_index, int i) {
	if(!curr_path[0]) {
		print_text(i ? sd_label : str_int_flash, (i == cursor_position) ? 11 : 0);
	} else if(!page_index && i < shift_index) {
		bool new_image_label = (!i && ft == file_type::disk);
		const std::string_view& s = new_image_label ? str_new_image : str_up_dir;
		if(new_image_label) {
			text_location.y -= 8*font_scale;
			Rect r(text_location.x,text_location.y,13*8*font_scale,8*font_scale);
			graphics.set_pen(BG); graphics.rectangle(r);
		}
		print_text(s, (i == cursor_position) ? s.size() : 0);
	}else
		update_one_display_file(i, i-shift_index);
}

void update_display_files(int page_index, int shift_index) {
	scroll_length = 0;
	text_location.x = (3*8+4)*font_scale;
	if(cursor_prev == -1) {
		Rect r(2*8*font_scale,8*font_scale,16*8*font_scale,(num_files ? files_per_page+1 : 2)*8*font_scale);
		graphics.set_pen(BG); graphics.rectangle(r);
		for(int i = 0; i < num_files_page; i++) {
			text_location.y = 8*(2+i)*font_scale;
			update_one_display_file2(page_index, shift_index, i);
		}
	}else{
		int i = cursor_prev;
		while(true) {
			text_location.y = 8*(2+i)*font_scale;
			Rect r(text_location.x,text_location.y,13*8*font_scale,8*font_scale);
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

bool mount_file(char *f, int file_entry_index) {
	int j;
	if(file_entry_index) {
		// Prevent the same file to mounted in two different drive slots
		// (It can work, but it is conceptually wrong to do it.)
		for(j=1; j<=4; j++) {
			if(j == file_entry_index)
				continue;
			if(!strcmp(mounts[j].mount_path, curr_path))
				return false;
		}
	}
	mutex_enter_blocking(&mount_lock);
	mounts[file_entry_index].mounted = true;
	mounts[file_entry_index].status = 0;
	strcpy((char *)mounts[file_entry_index].mount_path, curr_path);
	j = 0;
	int si = (ft == file_type::disk) ? 3 : 2;
	while(j<12) {
		mounts[file_entry_index].str[si+j] = (j < strlen(f) ? f[j] : ' ');
		j++;
	}
	mutex_exit(&mount_lock);
	return true;
}

void get_file(int file_entry_index) {
	int saved_cursor_position = cursor_position;

	cursor_prev = -1;
	cursor_position = (ft == file_type::disk && curr_path[0]) ? 1 : 0;
	int page_index = 0;

	main_buttons[0].str = &char_left;
	uint8_t last_sd_card_file = sd_card_present;
	while(true) {
		//loading_pg.init();
		int32_t r = read_directory(-1, 0);
		if(r < 0 && curr_path[0]) {
			//if(sd_card_present)
				curr_path[0] = 0;
			//else
			//	strcpy(curr_path, volume_names[0]);
			last_file_name[0] = 0;
			r = read_directory(-1, 0);
		}
		graphics.set_pen(BG); graphics.clear();
		if(r < 0) {
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
		int num_pages = (num_files+shift_index+files_per_page-1) / files_per_page;
		int last_page = (num_files+shift_index) % files_per_page;
		if(!last_page)
			last_page = files_per_page;
		num_files_page = (page_index == num_pages-1) ? last_page : files_per_page;
		if(num_files) {
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
				num_files_page = (page_index == num_pages-1) ? last_page : files_per_page;
			}
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
					if(cursor_position == files_per_page-1 && page_index < num_pages-1) {
						cursor_prev = -1;
						cursor_position = 0;
						page_index++;
						num_files_page = (page_index == num_pages-1) ? last_page : files_per_page;
						read_directory(page_index, num_files_page);
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
						cursor_position = files_per_page-1;
						page_index--;
						num_files_page = files_per_page;
						read_directory(page_index, page_index ? num_files_page : num_files_page-shift_index);
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
						//FATFS fatfs;
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
							Rect r(2*8*font_scale,8*font_scale,16*8*font_scale,(files_per_page+1)*8*font_scale);
							graphics.rectangle(r);
							char *f = &curr_path[i];
							text_location.x = 4*8*font_scale;
							text_location.y = 8*font_scale;
							print_text(f);
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
									if(!mount_file(f, file_entry_index))
										red_blinks = 2;

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
						if(!mount_file(f, file_entry_index))
							red_blinks = 2;
						curr_path[i] = 0;
						goto get_file_exit;
					}
				}
			} else if(button_b.read()) {
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
	print_text(opt_names[i], i==cursor_position ? 16 : 0);
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
	text_location.y = (2+(2+i)*10)*font_scale;
	if(i == option_count-1)
		text_location.y += 2*font_scale;
	if(erase) {
		Rect r(text_location.x,text_location.y,16*8*font_scale,8*font_scale);
		graphics.set_pen(BG); graphics.rectangle(r);
	}
	print_text(option_names[i], i==cursor_position ? option_names[i].size() : 0);
	if(i != option_count-1) {
		text_location.x += option_names[i].size()*8*font_scale;
		print_text(option_lists[i].short_names[current_options[i]], i==cursor_position ? 5 : 0);
	}
}

void update_options() {
	if(cursor_prev == -1) {
		graphics.set_pen(BG);
		graphics.clear();
		for(int i=0; i<option_count;i++)
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
		}else if(button_y.read() && cursor_position < option_count-1) {
			cursor_prev = cursor_position;
			cursor_position++;
			update_options();
		}else if(button_b.read()) {
			break;
		}else if(button_a.read()) {
			if(cursor_position == option_count-1) {
				save_config_flag = true;
				break;
			} else {
				int old_hsio_option = current_options[hsio_option_index];
				select_option(cursor_position);
				if(current_options[hsio_option_index] != old_hsio_option) {
					high_speed = -1;
					uart_set_baudrate(uart1, current_options[clock_option_index] ? hsio_opt_to_baud_ntsc[0] : hsio_opt_to_baud_pal[0]);
				}
				goto restart_options;
			}
		}
		st7789.update(&graphics);
		sleep_ms(20);
	}
	old_option_cursor = cursor_position == (option_count - 1) ? 0 : cursor_position;
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

#ifdef PICO_BOARD
	sprintf(temp_array, "HW: %s %dMB", PICO_BOARD, BOARD_SIZE);
#else
	sprintf(temp_array, "HW: Pico %dMB", BOARD_SIZE);
#endif
	text_location.x = str_x(strlen(temp_array));
	text_location.y += 10*font_scale;
	print_text(temp_array);
	st7789.update(&graphics);
	while(!(button_a.read() || button_b.read() || button_x.read() || button_y.read())) tight_loop_contents();
}

int main() {

	// Overclocking does not seem to be required
	// set_sys_clock_khz(250000, true);
	multicore_lockout_victim_init();
	led.set_rgb(0, 0, 0);
	led.set_brightness(128);
	st7789.set_backlight(255);

	graphics.set_font(&atari_font);
	graphics.set_pen(BG); graphics.clear();

	struct repeating_timer tmr;
	add_repeating_timer_ms(250, repeating_timer_led, NULL, &tmr);

	if(button_a.read())
		usb_drive();

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

	check_and_load_config(b_pressed);

	tud_mount_cb();
	f_mount(&fatfs[0], volume_names[0], 1);

	if(!try_mount_sd())
		blue_blinks = 4;

	if(!sd_card_present && curr_path[0] == '1')
		curr_path[0] = 0;
		// strcpy(curr_path, volume_names[0]);

	multicore_launch_core1(core1_entry);
	// This would give more scheduling priority to core 1 that
	// serves the SIO communication, but it does not seem to be
	// necessary
	// bus_ctrl_hw->priority = BUSCTRL_BUS_PRIORITY_PROC1_BITS;
	update_main_menu();
	st7789.update(&graphics);
	FSIZE_t last_cas_offset = -1;

	uint8_t last_sd_card_menu = sd_card_present;
	while(true) {
		if(last_sd_card_menu != sd_card_present) {
			last_sd_card_menu = sd_card_present;
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
					for(int i=si; i != li; i += di) {
						memcpy(&mounts[i].str[3], &mounts[i+di].str[3], 13);
						memcpy((void *)mounts[i].mount_path, (void *)mounts[i+di].mount_path, 256);
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
				mutex_enter_blocking(&mount_lock);
				if(mounts[d].mounted) {
					mounts[d].status = 0;
					mounts[d].mounted = false;
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
		FSIZE_t s = mounts[0].status;
		if(cursor_prev == -1 || (mounts[0].mounted && s != last_cas_offset)) {
			if(s < 0) s = 0;
			cas_pg.update(cas_pg_width*s/cas_size);
			last_cas_offset = s;
		}
		update_main_menu_buttons();

/*

		sprintf(txt_buf, " %d %d", f_mount_result, sd_card_present);
		text_location.x = 0;
		text_location.y = 0;
		Rect rt(text_location.x, text_location.y, 20*font_scale*8, font_scale*16);
		graphics.set_pen(BG);
		graphics.rectangle(rt);
		print_text(txt_buf);
//		text_location.y += 16;
//		print_text(&txt_buf[20]);
*/

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

void tud_umount_cb(void) {
}

void tud_suspend_cb(bool remote_wakeup_en) {
	// (void) remote_wakeup_en;
}

void tud_resume_cb(void) {
}

void tud_cdc_line_state_cb(uint8_t itf, bool dtr, bool rts) {
	//(void) itf;
	//(void) rts;
	//(void) dtr;
}

void tud_cdc_rx_cb(uint8_t itf) {
	//(void) itf;
}
