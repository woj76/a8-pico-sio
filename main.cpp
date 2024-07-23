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

#include <string.h>
#include <cstdlib>

#include "pico/multicore.h"
#include "pico/util/queue.h"
#include "pico/time.h"
#include "hardware/uart.h"
#include "hardware/pio.h"
#include "hardware/dma.h"
#include "hardware/irq.h"
#include "hardware/regs/busctrl.h"
#include "hardware/structs/bus_ctrl.h"
#include "tusb.h"
#include "fatfs_disk.h"
#include "ff.h"

#include "libraries/pico_display_2/pico_display_2.hpp"
#include "drivers/st7789/st7789.hpp"
#include "libraries/pico_graphics/pico_graphics.hpp"
#include "drivers/rgbled/rgbled.hpp"
#include "drivers/button/button.hpp"

#include "font_atari_data.hpp"
#include "pin_io.pio.h"

#define PICO_UART

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
const std::string str_config2 = "Config";

std::string char_empty = " ";
std::string char_up = "!";
std::string char_down = "\"";
std::string char_left = "#";
std::string char_right = "$";
std::string char_inject = "%";
std::string char_eject = "&";
std::string char_play = "'";
std::string char_stop = "(";

using namespace pimoroni;

ST7789 st7789(320, 240, ROTATE_0, false, get_spi_pins(BG_SPI_FRONT));
PicoGraphics_PenP4 graphics(st7789.width, st7789.height, nullptr);

uint32_t inline str_x(uint32_t l) { return (st7789.width - l*8*font_scale)/2; }
uint32_t inline str_y(uint32_t h) { return (st7789.height - h*8*font_scale)/2; }

Point text_location(str_x(str_press_a_4.length()), str_y(5)-4*font_scale);

RGBLED led(PicoDisplay2::LED_R, PicoDisplay2::LED_G, PicoDisplay2::LED_B, Polarity::ACTIVE_LOW, 25);

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

const std::string option_names[] = {
	"Mount RdWr:",
	"Host Clock:",
	"HSIO Speed:",
	"XEX Loader:",
	"Turbo Data:",
	"Turbo When:",
	"PWM Invert:",
	"   >> Save <<   "
};
const int option_count = 8;

typedef struct {
	const int count;
	const char **short_names;
	const char **long_names;
} option_list;

const char *mount_option_names_short[] = {"  OFF", "   ON"};
const char *mount_option_names_long[] = {"Read-only", "Read/Write"};
const char *clock_option_names_short[] = {"  PAL", " NTSC"};
const char *clock_option_names_long[] = {"PAL at 1.77MHz", "NTSC at 1.79MHz"};
const char *hsio_option_names_short[] = {"  $28", "   $6", "   $5","   $4", "   $3","   $2", "   $1", "   $0"};
const char *hsio_option_names_long[] = {"$28 ~19 kbit/s", " $6 ~68 kbit/s", " $5 ~74 kbit/s"," $4 ~81 kbit/s", " $3 ~90 kbit/s", " $2 ~99 kbit/s", " $1 ~111 kbit/s", " $0 ~127 kbit/s"};
const char *xex_option_names_short[] = {" $700", " $500", " $600", " $800"};
const char *xex_option_names_long[] = {"Loader at $700", "Loader at $500", "Loader at $600", "Loader at $800"};
const char *turbo1_option_names_short[] = {"  SIO", " J2P4", " PROC", "  INT", " J2P1"};
const char *turbo1_option_names_long[] = {"SIO Data In", "Joy2 Port Pin 4", "SIO Proceed", "SIO Interrupt", "Joy2 Port Pin 1"};
const char *turbo2_option_names_short[] = {" COMM", " J2P3", "  SIO", " NONE"};
const char *turbo2_option_names_long[] = {"SIO Command", "Joy2 Port Pin 3", "SIO Data Out", "None / Motor"};
const char *turbo3_option_names_long[] = {"Normal", "Inverted"};

const int mount_option_index = 0;
const int clock_option_index = 1;
const int hsio_option_index = 2;
const int xex_option_index = 3;
const int turbo1_option_index = 4;
const int turbo2_option_index = 5;
const int turbo3_option_index = 6;

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
		.count = 8,
		.short_names = hsio_option_names_short,
		.long_names = hsio_option_names_long
	},
	{
		.count = 4,
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
	size_t i = l;
	while(i > 0 && filename[--i] != '.');
	return (i == 0 || l-i > 4) ? l : i+1;
}

enum file_type {none, boot, disk, casette};
file_type ft = file_type::none;

bool is_valid_file(char *filename) {
	size_t i = get_filename_ext(filename);
	switch(ft) {
		case file_type::casette:
			return strcasecmp(&filename[i], "CAS") == 0;
		case file_type::boot:
		case file_type::disk:
			return strcasecmp(&filename[i], "ATR") == 0 || strcasecmp(&filename[i], "ATX") == 0 || (ft == file_type::boot ? strcasecmp(&filename[i], "XEX") == 0 : 0);
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
	FATFS fatfs; FILINFO fno; DIR dir;
	size_t current_file_name_index = 0;
	uint8_t ret = 1;

	num_files = 0;
	if (f_mount(&fatfs, "", 1) == FR_OK) {
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
// const std::string str_rewind = "Rewind";

const int menu_to_mount[] = {-1,1,2,3,4,-1,-1,0};
const file_type menu_to_type[] = {
	file_type::none,
	// TODO With e.g. U1MB theoretically all D: are bootable, but then the (kboot?) XEX loader
	// needs to account for the drive number.
	file_type::boot, file_type::disk, file_type::disk, file_type::disk,
	file_type::none, file_type::none,
	file_type::casette
};

// TODO which of these actually need to be volatile?

typedef struct {
	std::string *str;
	volatile char *mount_path;
	volatile bool mounted;
	/* status:
		if 0 the file header needs to be processed, if this fails:
			set mounted to false
			mark on screen with a suitable icon
		opening a new file / injecting sets status to 0
		for C: current index to the file being read to use with f_lseek
		for Dx: 0 yet to be opened, >0 sector size of the mounted ATR and ATR header verified

	*/
	volatile FSIZE_t status;
} mounts_type;

volatile char d1_mount[256] = {0};
volatile char d2_mount[256] = {0};
volatile char d3_mount[256] = {0};
volatile char d4_mount[256] = {0};
volatile char c_mount[256] = {0};

volatile mounts_type mounts[] = {
	{.str=&str_cas, .mount_path=c_mount, .mounted=false, .status = 0},
	{.str=&str_d1, .mount_path=d1_mount, .mounted=false, .status = 0},
	{.str=&str_d2, .mount_path=d2_mount, .mounted=false, .status = 0},
	{.str=&str_d3, .mount_path=d3_mount, .mounted=false, .status = 0},
	{.str=&str_d4, .mount_path=d4_mount, .mounted=false, .status = 0}
};

/*
  // Init serial port to 19200 baud
  // Blocking lock for FS access?
  // go through mounts that are true and status 0: try to validate the file and set status accordingly

  // otherwise, check if command line low and serial read waiting, act accordingly
*/

const size_t sector_buffer_size = 512;
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
// uint16_t dsk_baudrate = 19200;
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
	// atx_header_type atx_header;
} disk_header_type;

disk_header_type disk_headers[4];

const uint8_t disk_type_atr = 1;
const uint8_t disk_type_xex = 2;
const uint8_t disk_type_atx = 3;


uint8_t inline locate_percom(int drive_number) {
	int i=0;
	uint8_t r=0x80;
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
	int i = disk_headers[drive_number-1].atr_header.temp3 & 0x3;
	if(percom_table[i*13] != sector_buffer[0] || !memcmp(&percom_table[i*13+2], &sector_buffer[2], 6))
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

// const uint turbo_data_pin = 25; // LED=25 for testing
const uint joy2_p3_pin = 2;
const uint joy2_p4_pin = 3;
const uint joy2_p1_pin = 22;
const uint normal_motor_pin = 10;
const uint command_line_pin = 11;
const uint proceed_pin = 21;
const uint interrupt_pin = 26;
const uint sio_tx_pin = 4;
const uint sio_rx_pin = 5;

// Conventional SIO, but also Turbo D & Turbo 6000
const uint32_t normal_motor_pin_mask = (1u << normal_motor_pin);
const uint32_t normal_motor_value_on = (1u << normal_motor_pin);

// Turbo 2000 KSO - Joy 2 port
const uint32_t kso_motor_pin_mask = (1u << joy2_p3_pin);
const uint32_t kso_motor_value_on = (0u << joy2_p3_pin);

// Turbo 2001 / 2000F and sorts over SIO data
const uint32_t comm_motor_pin_mask = (1u << command_line_pin) | (1u << normal_motor_pin);
const uint32_t comm_motor_value_on = (0u << command_line_pin) | (1u << normal_motor_pin);

// Turbo Blizzard - SIO Data Out
const uint32_t sio_motor_pin_mask = (1u << sio_rx_pin) | (1u << normal_motor_pin);
const uint32_t sio_motor_value_on = (0u << sio_rx_pin) | (1u << normal_motor_pin);

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
int dma_channel, dma_channel_turbo;

queue_t pio_queue;
// 10*8 is not enough for Turbo D 9000, but going wild here costs memory, each item is 4 bytes
// 16*8 also fails sometimes with the 1MHz base clock
const int pio_queue_size = 32*8;

uint32_t pio_e;

void dma_handler() {
	int dc = dma_block_turbo ? dma_channel_turbo : dma_channel;
	if(dma_going)
		dma_hw->ints0 = 1u << dc;
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

// byte0 = drive
// byte4 = correct cs
// byte1 = read sector 'R'
// byte2+3 = sector number

const uint8_t hsio_opt_to_index[] = {0x28, 6, 5, 4, 3, 2, 1, 0};
const uint hsio_opt_to_baud_ntsc[] = {19040, 68838, 74575, 81354, 89490, 99433, 111862, 127842};
const uint hsio_opt_to_baud_pal[] = {18866, 68210, 73894, 80611, 88672, 98525, 110840, 126675};
// PAL/NTSC average
// const uint hsio_opt_to_baud[] = {18953, 68524, 74234, 80983, 89081, 98979, 111351, 127258};
uint8_t high_speed = 0;
uint8_t current_speed_index = 0;

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

bool try_get_sio_command() {
	// Assumption - casette motor is on the active command line
	// is much more likely due to turbo activation and casette transfer
	// should not be interrupted
	if(gpio_get(command_line_pin) || gpio_get(normal_motor_pin))
		return false;
	uint8_t status_byte = 0;
	bool r = true;
	int i=0;
	memset(&sio_command, 0x00, 4);
	sio_command.checksum = 0xFF;
	//if(uart_is_readable(uart1))
	// TODO the 2.5 ms timeout dep current SIO speed?
	while(i<5 && uart_is_readable_within_us(uart1, 2500) && !uart_get_hw(uart1)->rsr)
		((uint8_t *)&sio_command)[i++] = (uint8_t)uart_get_hw(uart1)->dr;
	if(uart_get_hw(uart1)->rsr || i != 5) {
		hw_clear_bits(&uart_get_hw(uart1)->rsr, UART_UARTRSR_BITS);
		r = false;
		status_byte |= 0x1; // Command frame error
	}
	if(sio_checksum((uint8_t *)&sio_command, 4) != sio_command.checksum) {
		r = false;
		status_byte |= 0x2; // Checksum error
	}
	if(sio_command.device_id >= 0x31 && sio_command.device_id <= 0x34)
		disk_headers[sio_command.device_id-0x31].atr_header.temp1 = status_byte;
	else
		r = false;
	if(status_byte && current_options[hsio_option_index] && high_speed) {
		current_speed_index ^= current_options[hsio_option_index];
		uart_set_baudrate(uart1, current_options[clock_option_index] ? hsio_opt_to_baud_ntsc[current_speed_index] : hsio_opt_to_baud_pal[current_speed_index]);
	}
	absolute_time_t t = make_timeout_time_ms(1); // According to Avery's manual 950us
	while (!gpio_get(command_line_pin) && absolute_time_diff_us(get_absolute_time(), t) < 0)
		tight_loop_contents();
	return r && gpio_get(command_line_pin);
}

uint8_t check_drive_and_sector_status(int drive_number, FSIZE_t *offset, FSIZE_t *to_read, bool op_write=false) {
	if(!mounts[drive_number].mounted) {
		disk_headers[drive_number-1].atr_header.temp2 = 0xFF;
		return 'N';
	}
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
	while(i<to_read && uart_is_readable_within_us(uart1, 2500))
		sector_buffer[i++] = (uint8_t)uart_get_hw(uart1)->dr;
	if(i!=to_read || !uart_is_readable_within_us(uart1, 2500)) {
		status |= 0x01;
		r = 'N';
	}else if(uart_get_hw(uart1)->dr != sio_checksum(sector_buffer, to_read)) {
		status |= 0x02;
		r = 'N';
	}
	disk_headers[drive_number-1].atr_header.temp1 = status;
	return r;
}

FRESULT mounted_file_transfer(int drive_number, FSIZE_t offset, FSIZE_t to_transfer, bool op_write, size_t t_offset = 0) {
	FATFS fatfs;
	FIL fil;
	FRESULT f_op_stat;
	uint bytes_transferred;
	uint8_t *data = &sector_buffer[t_offset];
	do {
		if((f_op_stat = f_mount(&fatfs, "", 1)) != FR_OK)
			break;
		if((f_op_stat = f_open(&fil, (const char *)mounts[drive_number].mount_path, op_write ? FA_WRITE : FA_READ)) != FR_OK)
			break;
		if((f_op_stat = f_lseek(&fil, offset)) != FR_OK)
			break;
		f_op_stat = op_write ?
			f_write(&fil, data, to_transfer, &bytes_transferred) :
			f_read(&fil, data, to_transfer, &bytes_transferred);
		if(f_op_stat != FR_OK)
			break;
		if(bytes_transferred != to_transfer || !mounts[drive_number].mounted)
			f_op_stat = FR_INT_ERR;
	}while(false);
	f_close(&fil);
	f_mount(0, "", 1);
	return f_op_stat;
}

void main_sio_loop(uint sm, uint sm_turbo) {
	FATFS fatfs;
	FIL fil;
	FILINFO fil_info;
	FSIZE_t offset;
	FSIZE_t to_read;
	FRESULT f_op_stat;
	int i;
	uint bytes_read;

	while(true) {
		for(i=0; i<5; i++) {
			if(!mounts[i].mounted || mounts[i].status)
				continue;
			if(f_mount(&fatfs, "", 1) != FR_OK)
				break;
			if(f_stat((const char *)mounts[i].mount_path, &fil_info) != FR_OK)
				break;
			if(f_open(&fil, (const char *)mounts[i].mount_path, FA_READ) == FR_OK) {
				if(!i) {
					check_turbo_conf();
					cas_sample_duration = timing_base_clock/600;
					cas_size = f_size(&fil);
					if(f_read(&fil, &cas_header, sizeof(cas_header_type), &bytes_read) == FR_OK &&
							bytes_read == sizeof(cas_header_type) &&
							cas_header.signature == cas_header_FUJI)
						mounts[i].status = cas_read_forward(&fil, cas_header.chunk_length + sizeof(cas_header_type));
					if(!mounts[i].status) {
						mounts[i].mounted = false;
						led.set_rgb(255,0,0);
					}else{
						led.set_rgb(0,255,0);
					}
				} else {
					// disk_headers[x].atr_header.{sec_size,pars, pars_high, magic}
					// TODO split this into ATR, ATX, and XEX
					// how to hold the file type information efficiently
					// read two bytes first?
					// ATX starts with AT8X
					// XEX starts with 0xFF 0xFF
					disk_headers[i-1].atr_header.temp4 = 0;
					if(f_read(&fil, sector_buffer, 4, &bytes_read) == FR_OK && bytes_read == 4) {
						if(*(uint16_t *)sector_buffer == 0x0296) // ATR magic
							disk_headers[i-1].atr_header.temp4 = disk_type_atr;
						else if(*(uint16_t *)sector_buffer == 0xFFFF)
							disk_headers[i-1].atr_header.temp4 = disk_type_xex;
						else if(*(uint32_t *)sector_buffer == 0x58385441) // AT8X
							disk_headers[i-1].atr_header.temp4 = disk_type_atx;
						f_lseek(&fil, 0);
					}
					switch(disk_headers[i-1].atr_header.temp4) {
						case disk_type_atr:
							if(f_read(&fil, &disk_headers[i-1].atr_header, sizeof(atr_header_type), &bytes_read) == FR_OK && bytes_read == sizeof(atr_header_type)) {
								mounts[i].status = (disk_headers[i-1].atr_header.pars | ((disk_headers[i-1].atr_header.pars_high << 16) & 0xFF0000)) << 4;
								disk_headers[i-1].atr_header.temp2 = 0xFF;
								// Whatever the ATR flag says, if the file itself is read-only,
								disk_headers[i-1].atr_header.temp3 = locate_percom(i);
								if(!current_options[mount_option_index] || (fil_info.fattrib & AM_RDO) || (disk_headers[i-1].atr_header.temp3 & 0x80))
									disk_headers[i-1].atr_header.flags |= 0x1;
								led.set_rgb(0,255,0);
							} else {
								disk_headers[i-1].atr_header.temp4 = 0;
							}
							break;
						case disk_type_xex:
							// Sectors occupied by the file itself
							disk_headers[i-1].atr_header.pars = (f_size(&fil)+124)/125;
							// This incidentally can be a proper SD or ED disk
							mounts[i].status = (disk_headers[i-1].atr_header.pars+3+0x170)*128;
							disk_headers[i-1].atr_header.sec_size = 128;
							disk_headers[i-1].atr_header.flags = 0x1;
							disk_headers[i-1].atr_header.temp2 = 0xFF;
							break;
						case disk_type_atx:
							mounts[i].status = 0xFFFF; // TODO
							disk_headers[i-1].atr_header.sec_size = 0xFFFF; // TODO
							disk_headers[i-1].atr_header.flags = 0x1;
							disk_headers[i-1].atr_header.temp2 = 0xFF;
							break;
						default:
							break;
					}
					if(!disk_headers[i-1].atr_header.temp4){
						led.set_rgb(255,0,0);
						mounts[i].status = 0;
						mounts[i].mounted = false;
					}
				}
				f_close(&fil);
			}
			f_mount(0, "", 1);
		}

		// uart_is_readable(uart1);
		// uart_write_blocking(uart1, uint8_t *, size t);
		// uart_read_blocking(uart1, uint8_t *, size t);
		// *dst++ = (uint8_t) uart_get_hw(uart)->dr;
		// uart_is_readable_within_us(uart_inst_t *uart, uint32_t us);
		if(try_get_sio_command()) {
			sleep_us(100); // Needed for BiboDos according to atari_drive_emulator.c
#ifdef PICO_UART
			gpio_set_function(sio_tx_pin, GPIO_FUNC_UART);
#endif
			memset(sector_buffer, 0, sector_buffer_size);
			int drive_number = sio_command.device_id-0x30;
			f_op_stat = FR_OK;
			to_read = 0;
			uint8_t r = 'A';
			switch(sio_command.command_id) {
				case 'S': // get status
					uart_putc_raw(uart1, r);
					to_read = 4;
					// TODO motor on only after first actual disk operation?
					// for not mounted drives report motor on but no disk (drive always present,
					// not only when the disk is in), or???
					sector_buffer[0] = disk_headers[drive_number-1].atr_header.temp1;
					sector_buffer[1] = disk_headers[drive_number-1].atr_header.temp2;
					if(mounts[drive_number].mounted) {
						sector_buffer[0] |= 0x10; // motor on
						if(disk_headers[drive_number-1].atr_header.flags & 0x1)
							sector_buffer[0] |= 0x08; // write protect
						if(disk_headers[drive_number-1].atr_header.sec_size == 256)
							sector_buffer[0] |= 0x20;
						else if(mounts[drive_number].status == 0x20800) // 1040*128
							sector_buffer[0] |= 0x80; // medium density
					} else {
						sector_buffer[1] &= 0x7F; // disk removed
					}
					sector_buffer[2] = 0xe0;
					sector_buffer[3] = 0x0;
					break;
				case 'N': // read percom
					// Only writable (according to our rules) disks react to PERCOM command frames
					if(!mounts[drive_number].mounted || (disk_headers[drive_number-1].atr_header.flags & 0x1))
						r = 'N';
					uart_putc_raw(uart1, r);
					if(r == 'N') break;
					memcpy(sector_buffer, &percom_table[(disk_headers[drive_number-1].atr_header.temp3 & 0x3)*13], 8);
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
							if((f_op_stat = mounted_file_transfer(drive_number, sizeof(atr_header_type)+offset, to_read , false)) != FR_OK)
								// ~0x10 record not found? Should this even be reported?
								disk_headers[drive_number-1].atr_header.temp2 &= 0xEF;
							else
								disk_headers[drive_number-1].atr_header.temp2 = 0xFF;
							break;
						case disk_type_xex:
							memset(sector_buffer, 0, 128);
							// TODO
							if(sio_command.sector_number >= 0x171) {
									bytes_read = to_read;
									offset = (sio_command.sector_number-0x171);
									to_read = 125; // TODO
									offset = sio_command.sector_number;
							} else {
								// TODO
								// XEX and sector <= 0x170
							}
							break;
						case disk_type_atx:
							break;
						default:
							break;
					}
					break;
				case 'O': // write percom
					// Only writable (according to our rules) disks react to PERCOM command frames
					if(!mounts[drive_number].mounted || (disk_headers[drive_number-1].atr_header.flags & 0x1))
						r = 'N';
					uart_putc_raw(uart1, r);
					if(r == 'N') break;
					to_read = 12;
					r = try_receive_data(drive_number, to_read);
					if(r == 'A' && !compare_percom(drive_number))
						r = 'N';
					sleep_us(850);
					uart_putc_raw(uart1, r);
					if(r == 'N') break;
					 // Mark that PERCOM has been written successfully
					disk_headers[drive_number-1].atr_header.temp3 |= 0x04;
					to_read = 0;
					break;
				case 'P': // put sector
				case 'W': // write (+verify) sector
					r = check_drive_and_sector_status(drive_number, &offset, &to_read, true);
					uart_putc_raw(uart1, r);
					if(r == 'N') break;
					r = try_receive_data(drive_number, to_read);
					sleep_us(850);
					uart_putc_raw(uart1, r);
					if(r == 'N') break;
					if((f_op_stat = mounted_file_transfer(drive_number, sizeof(atr_header_type)+offset, to_read, true)) != FR_OK) {
						disk_headers[drive_number-1].atr_header.temp1 |= 0x4; // write error
						break;
					} else if (sio_command.command_id == 'W') {
						if((f_op_stat = mounted_file_transfer(drive_number, sizeof(atr_header_type)+offset, to_read, false, to_read)) != FR_OK)
							break;
						if(memcmp(sector_buffer, &sector_buffer[to_read], to_read)) {
							f_op_stat = FR_INT_ERR;
							break;
						}
					}
					to_read = 0;
					break;
				case '!':
				case '"':
					i = sio_command.command_id - 0x21;
					if(!mounts[drive_number].mounted ||
						(disk_headers[drive_number-1].atr_header.flags & 0x1) ||
						(i != (disk_headers[drive_number-1].atr_header.temp3 & 0x3)
							&& !(disk_headers[drive_number-1].atr_header.temp3 & 0x4)))
						r = 'N';
					uart_putc_raw(uart1, r);
					if(r == 'N') break;
					to_read = disk_headers[drive_number-1].atr_header.sec_size;
					memset(sector_buffer, 0, to_read);
					offset = mounts[drive_number].status >> 7;
					for(i=0; i<offset; i++)
						if((f_op_stat = mounted_file_transfer(drive_number, sizeof(atr_header_type)+i*128, 128, true)) != FR_OK)
							break;
					sector_buffer[0] = sector_buffer[1] = 0xFF;
					break;
				case '?': // get speed index
					uart_putc_raw(uart1, r);
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
				sleep_us(300); // Again, timeout mentioned by atari_drive_emulator.c
				uart_putc_raw(uart1, f_op_stat == FR_OK ? 'C' : 'E');
				if(to_read) {
					uart_tx_wait_blocking(uart1);
					sleep_us(150); // Another one from atari_drive_emulator.c
					uart_write_blocking(uart1, sector_buffer, to_read);
					uart_putc_raw(uart1, sio_checksum(sector_buffer, to_read));
				}
				uart_tx_wait_blocking(uart1);
			}
			if(sio_command.command_id == '?') {
				if(current_options[hsio_option_index]) {
					high_speed = 1;
					current_speed_index = current_options[hsio_option_index];
				}else{
					high_speed = 0;
				}
				uart_set_baudrate(uart1, current_options[clock_option_index] ? hsio_opt_to_baud_ntsc[current_speed_index] : hsio_opt_to_baud_pal[current_speed_index]);
			}
		} else if(mounts[0].mounted && (offset = mounts[0].status) && offset < cas_size &&
			(cas_block_turbo ? turbo_motor_value_on : normal_motor_value_on) ==
				(gpio_get_all() & (cas_block_turbo ? turbo_motor_pin_mask : normal_motor_pin_mask))) {
			to_read = std::min(cas_header.chunk_length-cas_block_index, (cas_block_turbo ? 128 : 256)*cas_block_multiple);
			if(mounted_file_transfer(0, offset, to_read, false) != FR_OK) {
				mounts[0].mounted = false;
				mounts[0].status = 0;
				continue;
			}
			offset += to_read;
			cas_block_index += to_read;
			mounts[0].status = offset;
#ifdef PICO_UART
			gpio_set_function(sio_tx_pin, GPIO_FUNC_SIO);
#endif
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
					if(f_mount(&fatfs, "", 1) == FR_OK && f_open(&fil, (const char *)mounts[0].mount_path, FA_READ) == FR_OK) {
						offset = cas_read_forward(&fil, offset);
					}
					f_close(&fil);
					f_mount(0, "", 1);
					mounts[0].status = offset;
					if(!offset)
						mounts[0].mounted = false;
				}
				if(cas_header.signature == cas_header_pwmc || cas_header.signature == cas_header_data || (cas_header.signature == cas_header_fsk && silence_duration) || dma_block_turbo^cas_block_turbo) {
					while(!pio_sm_is_tx_fifo_empty(cas_pio, dma_block_turbo ? sm_turbo : sm))
						tight_loop_contents();
					sleep_ms(10);
				}
			}
		}
	}
}

void core1_entry() {
	//timing_base_clock = clock_get_hz(clk_sys);
	timing_base_clock = 1000000;
	max_clock_ms = 0x7FFFFFFF/(timing_base_clock/1000)/1000*1000;

	gpio_init(joy2_p3_pin); gpio_set_dir(joy2_p3_pin, GPIO_IN); gpio_pull_up(joy2_p3_pin);
	gpio_init(normal_motor_pin); gpio_set_dir(normal_motor_pin, GPIO_IN); gpio_pull_down(normal_motor_pin);
	gpio_init(command_line_pin); gpio_set_dir(command_line_pin, GPIO_IN); gpio_pull_up(command_line_pin);
	// gpio_set_function(turbo_data_pin, GPIO_FUNC_SIO);

	queue_init(&pio_queue, sizeof(int32_t), pio_queue_size);

	pio_offset = pio_add_program(cas_pio, &pin_io_program);
 	float clk_divider = (float)clock_get_hz(clk_sys)/timing_base_clock;

	sm = pio_claim_unused_sm(cas_pio, true);
	pio_gpio_init(cas_pio, sio_tx_pin);
	pio_sm_set_consecutive_pindirs(cas_pio, sm, sio_tx_pin, 1, true);
	pio_sm_config c = pin_io_program_get_default_config(pio_offset);
	sm_config_set_fifo_join(&c, PIO_FIFO_JOIN_TX);
	sm_config_set_clkdiv(&c, clk_divider);
	// sm_config_set_set_pins(&c, uart1_tx_pin, 1);
	sm_config_set_out_pins(&c, sio_tx_pin, 1);
	sm_config_set_out_shift(&c, true, true, 32);
	pio_sm_init(cas_pio, sm, pio_offset, &c);
	pio_sm_set_enabled(cas_pio, sm, true);

	sm_turbo = pio_claim_unused_sm(cas_pio, true);

	sm_config_turbo = pin_io_program_get_default_config(pio_offset);
	sm_config_set_fifo_join(&sm_config_turbo, PIO_FIFO_JOIN_TX);
	// if(set_clock_div)
	sm_config_set_clkdiv(&sm_config_turbo, clk_divider);
	sm_config_set_out_shift(&sm_config_turbo, true, true, 32);
	// sm_config_set_set_pins(&c1, turbo_data_pin, 1);
	check_turbo_conf();

	dma_channel = dma_claim_unused_channel(true);
	dma_channel_config dma_c = dma_channel_get_default_config(dma_channel);
	channel_config_set_transfer_data_size(&dma_c, DMA_SIZE_32);
	channel_config_set_read_increment(&dma_c, false);
	channel_config_set_dreq(&dma_c, pio_get_dreq(cas_pio, sm, true));
	dma_channel_configure(dma_channel, &dma_c, &cas_pio->txf[sm], &pio_e, 1, false);
	dma_channel_set_irq0_enabled(dma_channel, true);

	dma_channel_turbo = dma_claim_unused_channel(true);
	dma_channel_config dma_c1 = dma_channel_get_default_config(dma_channel_turbo);
	channel_config_set_transfer_data_size(&dma_c1, DMA_SIZE_32);
	channel_config_set_read_increment(&dma_c1, false);
	channel_config_set_dreq(&dma_c1, pio_get_dreq(cas_pio, sm_turbo, true));
	dma_channel_configure(dma_channel_turbo, &dma_c1, &cas_pio->txf[sm_turbo], &pio_e, 1, false);
	dma_channel_set_irq0_enabled(dma_channel_turbo, true);

	irq_set_exclusive_handler(DMA_IRQ_0, dma_handler);
	irq_set_enabled(DMA_IRQ_0, true);

	//gpio_init(sio_rx_pin); gpio_set_dir(sio_rx_pin, GPIO_IN); gpio_pull_up(sio_rx_pin);
	//gpio_init(sio_tx_pin); gpio_set_dir(sio_tx_pin, GPIO_OUT); gpio_put(sio_tx_pin, 1);
#ifdef PICO_UART
	uart_init(uart1, current_options[clock_option_index] ? hsio_opt_to_baud_ntsc[0] : hsio_opt_to_baud_pal[0]);
	gpio_set_function(sio_tx_pin, GPIO_FUNC_UART);
	gpio_set_function(sio_rx_pin, GPIO_FUNC_UART);
	uart_set_hw_flow(uart1, false, false);
	uart_set_fifo_enabled(uart1, true);
	uart_set_format(uart1, 8, 1, UART_PARITY_NONE);
#endif

	main_sio_loop(sm, sm_turbo);
}

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
	{.str = &str_cas,.x=3*8*font_scale,.y=(10*8)*font_scale,.wd=str_cas.length()}
	// TODO Make this a nice About... screen
	//{.str = (std::string*)&str_rewind,.x=7*8*font_scale,.y=(13*8+4)*font_scale,.wd=str_rewind.length()},
};
const size_t menu_entry_size = 8;

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
	}else{
		main_buttons[0].str = &char_empty;
	}
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

	while(true) {
		loading_pg.init();

		struct repeating_timer timer;
		add_repeating_timer_ms(1000/60, repeating_timer_directory, NULL, &timer);

		uint8_t r = read_directory();
		if(!r && curr_path[0]) {
			curr_path[0] = 0;
			r = read_directory();
		}
		// sleep_ms(5000); // For testing
		cancel_repeating_timer(&timer);

		graphics.set_pen(BG); graphics.clear();
		if(!r) {
			text_location.x = str_x(str_no_media.length());
			text_location.y = 7*8*font_scale;
			print_text(str_no_media, str_no_media.length());
			update_buttons(nomedia_buttons, nomedia_buttons_size);
			st7789.update(&graphics);
			while(!button_b.read())
				tight_loop_contents();
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
		int y1 = main_buttons[2].y+10*font_scale, y2 = main_buttons[3].y-2*font_scale;
		int scroll_block_len = (y2-y1) / (num_files / 11 + (num_files % 11 ? 1 : 0));
		Rect scroll_bar(st7789.width - 7*font_scale, y1, 4*font_scale, y2-y1);
		graphics.set_pen(BG); graphics.rectangle(scroll_bar);
		scroll_bar.h = scroll_block_len;
		graphics.set_pen(WHITE); graphics.rectangle(scroll_bar);
		st7789.update(&graphics);
		while(true) {
			if(button_y.read()) {
				if(top_index+cursor_position < num_files+shift_index-1) {
					if(cursor_position == 10) {
						cursor_prev = -1;
						cursor_position = 0;
						top_index += 11;
						graphics.set_pen(BG); graphics.rectangle(scroll_bar);
						scroll_bar.y += scroll_block_len;
						graphics.set_pen(WHITE); graphics.rectangle(scroll_bar);
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
						graphics.set_pen(BG); graphics.rectangle(scroll_bar);
						scroll_bar.y -= scroll_block_len;
						graphics.set_pen(WHITE); graphics.rectangle(scroll_bar);
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
						mounts[file_entry_index].status = 0;
						strcpy((char *)mounts[file_entry_index].mount_path, &curr_path[0]);
						strcpy((char *)&(mounts[file_entry_index].mount_path[i]), f);
						i = 0;
						int si = (ft == file_type::disk || ft == file_type::boot) ? 3 : 2;
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

void update_selection_entry(const char **opt_names, int i, bool erase) {
	text_location.x = 2*8*font_scale;
	text_location.y = (2+i)*12*font_scale;
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

	const char **opt_names = option_lists[opt_num].long_names;
	int opt_count = option_lists[opt_num].count;
	update_selections(opt_names, opt_count);

	text_location.x = str_x(option_names[opt_num].length());
	text_location.y = 4*font_scale;
	print_text(option_names[opt_num]);
	Rect r(text_location.x,text_location.y+10*font_scale,option_names[opt_num].length()*8*font_scale,2*font_scale);
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
	text_location.y = (2+i)*12*font_scale;
	//if(i == option_count-1)
	//	text_location.y += 4*font_scale;
	if(erase) {
		Rect r(text_location.x,text_location.y,16*8*font_scale,8*font_scale);
		graphics.set_pen(BG); graphics.rectangle(r);
	}
	print_text(option_names[i], i==cursor_position ? option_names[i].length() : 0);
	if(i != option_count-1) {
		text_location.x += option_names[i].length()*8*font_scale;
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
	text_location.x = str_x(str_config2.length());
	text_location.y = 4*font_scale;
	print_text(str_config2);
	Rect r(text_location.x,text_location.y+10*font_scale,str_config2.length()*8*font_scale,2*font_scale);
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
				// TODO Save config
				break;
			} else {
				select_option(cursor_position);
				// TODO process the new option set?
				// For turbo this is done elsewhere
				// HSIO is done
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

int main() {
	// stdio_init_all();
	// set_sys_clock_khz(250000, true);
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

	ProgressBar pg(200, text_location.y+16*font_scale, true);
	pg.init();
	st7789.update(&graphics);
	uint32_t boot_time;
	do {
		boot_time = to_ms_since_boot(get_absolute_time());
		pg.update(200*boot_time/usb_boot_delay);
		st7789.update(&graphics);
		if(button_a.read())
			usb_drive();
		sleep_ms(1000/60);
	}while(boot_time <= usb_boot_delay);

	tud_mount_cb();
	multicore_launch_core1(core1_entry);
	// bus_ctrl_hw->priority = BUSCTRL_BUS_PRIORITY_PROC1_BITS;
	update_main_menu();
	st7789.update(&graphics);
	FSIZE_t last_cas_offset = -1;

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
				// config, rotate up, rotate down
				if(cursor_position == 0) {
					change_options();
				}
				update_main_menu();
			}else{
				get_file(menu_to_type[cursor_position], d);
				update_main_menu();
			}

		}else if(button_b.read()) {
			if(d != -1) {
				if(mounts[d].mounted) {
					mounts[d].status = 0;
					mounts[d].mounted = false;
				}else{
					if(mounts[d].mount_path[0]) {
						mounts[d].mounted = true;
						mounts[d].status = 0; // TODO probably obsolete
						if(!d)
							last_cas_offset = -1;
					}
				}
			}
		}
		FSIZE_t s = mounts[0].status;
		if(cursor_prev == -1 || (mounts[0].mounted && s != last_cas_offset)) {
			if(s == -1) s = 0;
			cas_pg.update(cas_pg_width*s/cas_size);
			last_cas_offset = s;
		}
		update_main_menu_buttons();
		st7789.update(&graphics);
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
