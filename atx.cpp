/**
  (Quite) modified implementation from sdrive-max project
  Original author of the ATX code dated 21/01/2018 is Daniel Noguerol
  Check the sdrive-max project for details.
*/

#include <string.h>
#include "pico/time.h"
#include "pico/rand.h"

#include "atx.hpp"

#include "mounts.hpp"
#include "disk_counter.h"

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

static void getCurrentHeadPosition(head_position_t *hp) {
	uint64_t s = get_absolute_time();
	hp->stamp = s;
#ifdef PIO_DISK_COUNTER
	hp->angle = (uint16_t)(au_full_rotation-disk_counter-1);
#else
	hp->angle = (uint16_t)((to_us_since_boot(s) >> 3) % au_full_rotation);
#endif
}

// Some older approach to the rotational disk counter...
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

enum atx_density { atx_single, atx_medium, atx_double };

typedef struct __attribute__((__packed__)) {
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
} atxFileHeader;

typedef struct __attribute__((__packed__)) {
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
} atxTrackHeader;

typedef struct __attribute__((__packed__)) {
	uint32_t next;
	uint16_t type;
	uint16_t pad0;
} atxSectorListHeader;

typedef struct __attribute__((__packed__)) {
	uint8_t number;
	uint8_t status;
	uint16_t timev;
	uint32_t data;
} atxSectorHeader;

typedef struct __attribute__((__packed__)) {
	uint32_t size;
	uint8_t type;
	uint8_t sectorIndex;
	uint16_t data;
} atxTrackChunk;

bool loadAtxFile(FIL *fil, int atx_drive_number) {
	atxFileHeader *fileHeader;
	atxTrackHeader *trackHeader;
	uint bytes_read;

	if(f_read(fil, sector_buffer, sizeof(atxFileHeader), &bytes_read) != FR_OK || bytes_read != sizeof(atxFileHeader))
		return false;

	fileHeader = (atxFileHeader *) sector_buffer;
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
		if(f_read(fil, sector_buffer, sizeof(atxTrackHeader), &bytes_read) != FR_OK || bytes_read != sizeof(atxTrackHeader))
			break;
		trackHeader = (atxTrackHeader *) sector_buffer;
		gTrackInfo[atx_drive_number][track] = startOffset;
		startOffset += trackHeader->size;
	}
	return true;
}

int8_t transferAtxSector(int atx_drive_number, uint16_t num, uint8_t *status, bool op_write, bool op_verify) {
	atxTrackHeader *trackHeader;
	atxSectorListHeader *slHeader;
	atxSectorHeader *sectorHeader;
	atxTrackChunk *extSectorData;

	uint16_t i;
	const size_t si = 256;
	int8_t r = 1;

	uint16_t atx_sector_size = disk_headers[atx_drive_number].atr_header.sec_size;
	bool is1050 = (disk_headers[atx_drive_number].atr_header.temp3 & 0x40) ? false : true;

	// calculate track and relative sector number from the absolute sector number
	uint8_t tgtTrackNumber = (num - 1) / atx_track_size[atx_drive_number];
	uint8_t tgtSectorNumber = (num - 1) % atx_track_size[atx_drive_number] + 1;

	// set initial status (in case the target sector is not found)
	*status = mask_fdc_missing;

	// delay for track stepping if needed
	if (gCurrentHeadTrack[atx_drive_number] != tgtTrackNumber) {
		int diff = tgtTrackNumber - gCurrentHeadTrack[atx_drive_number];
		if (diff > 0)
			diff += (is1050 ? 1 : 0);
		else
			diff = -diff;
		sleep_us(is1050 ? (diff*us_track_step_1050 + us_head_settle_1050) : (diff*us_track_step_810 + us_head_settle_810));
	}

	gCurrentHeadTrack[atx_drive_number] = tgtTrackNumber;


	// sample current head position
	head_position_t headPosition;
	getCurrentHeadPosition(&headPosition);

	uint16_t sectorCount = 0;

	// get the track header
	uint32_t currentFileOffset = gTrackInfo[atx_drive_number][tgtTrackNumber];
	// exit, if track not present
	if (currentFileOffset) {
		if(mounted_file_transfer(atx_drive_number+1, currentFileOffset, sizeof(atxTrackHeader), false, si) == FR_OK) {
			trackHeader = (atxTrackHeader *)&sector_buffer[si];
			sectorCount = trackHeader->sectorCount;
		}else
			r = -1;
	}

	// For "healthy" ATX files the first check should probably be always fail
	if (trackHeader->trackNumber != tgtTrackNumber || atx_density[atx_drive_number] != ((trackHeader->flags & 0x2) ? atx_medium : atx_single))
		sectorCount = 0;

	uint32_t track_header_size = trackHeader->headerSize;

	if(sectorCount) {
		currentFileOffset += track_header_size;
		if (mounted_file_transfer(atx_drive_number+1, currentFileOffset, sizeof(atxSectorListHeader), false, si) == FR_OK) {
			slHeader = (atxSectorListHeader *)&sector_buffer[si];
			// sector list header is variable length, so skip any extra header bytes that may be present
			currentFileOffset += slHeader->next - sectorCount * sizeof(atxSectorHeader);
		} else {
			sectorCount = 0;
			r = -1;
		}
	}

	uint32_t tgtSectorOffset; // the offset of the target sector data
	uint32_t writeStatusOffset; // for the write operation, remember where to update the status bit
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
		// but read all sector headers first not to bang the media (SD card)
		// repeatadely (it would mess the ATX timing)
		if (sectorCount) {
			if(mounted_file_transfer(atx_drive_number+1, currentFileOffset, sectorCount*sizeof(atxSectorHeader), false, si) == FR_OK) {
				for (i=0; i < sectorCount; i++) {
					sectorHeader = (atxSectorHeader *)&sector_buffer[si+i*sizeof(atxSectorHeader)];
					// if the sector is not flagged as missing and its number matches the one we're looking for...
					if (sectorHeader->number == tgtSectorNumber) {
						if(sectorHeader->status & mask_fdc_missing) {
							write_status |= sectorHeader->status;
							currentFileOffset += sizeof(atxSectorHeader);
							continue;
						}
						// check if it's the next sector that the head would encounter angularly...
						int tt = sectorHeader->timev - headPosition.angle;
						if (!tgtSectorOffset || (tt > 0 && pTT < 0) || (tt > 0 && pTT > 0 && tt < pTT) || (tt < 0 && pTT < 0 && tt < pTT)) {
							pTT = tt;
							*status = sectorHeader->status;
							writeStatusOffset = currentFileOffset + 1;
							tgtSectorIndex = i;
							tgtSectorOffset = sectorHeader->data;
						}
					}
					currentFileOffset += sizeof(atxSectorHeader);
				}
			}else
				r = -1;
		}
		uint16_t act_sector_size = atx_sector_size;
		ext_sector_size = 0;
		if (*status & mask_extended_data) {
			// NOTE! the first part of the trackHeader data (stored in sector_buffer) is by now overwritten,
			// but the headerSize field is far enough into the struct to stay untouched!
			currentFileOffset = gTrackInfo[atx_drive_number][tgtTrackNumber] + track_header_size;
			do {
				if (mounted_file_transfer(atx_drive_number+1, currentFileOffset, sizeof(atxTrackChunk), false, si) != FR_OK) {
					r = -1;
					break;
				}
				extSectorData = (atxTrackChunk *) &sector_buffer[si];
				if (extSectorData->size) {
					// if the target sector has a weak data flag, grab the start weak offset within the sector data
					// otherwise check for the extended sector length and update ext_sector_size accordingly
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
			if(mounted_file_transfer(atx_drive_number+1, gTrackInfo[atx_drive_number][tgtTrackNumber] + tgtSectorOffset, atx_sector_size, op_write, 0) != FR_OK) {
				r = -1;
				tgtSectorOffset = 0;
			} else if(op_verify) {
				if(mounted_file_transfer(atx_drive_number+1, gTrackInfo[atx_drive_number][tgtTrackNumber] + tgtSectorOffset, atx_sector_size, false, 256) != FR_OK) {
					tgtSectorOffset = 0;
					r = -1;
				}else if(memcmp(sector_buffer, &sector_buffer[256], atx_sector_size))
					tgtSectorOffset = 0;
			}

			// This calculation is an educated guess based on all the different ATX implementations
			uint16_t au_one_sector_read = (23+act_sector_size)*(atx_density[atx_drive_number] == atx_single ? 8 : 4)+2;
			// We will need to circulate around the disk one more time if we are re-reading the just written sector
			if(op_verify)
				au_one_sector_read += au_full_rotation;
			sleep_until(delayed_by_us(headPosition.stamp, (au_one_sector_read + pTT + (pTT > 0 ? 0 : au_full_rotation))*8));

			if(*status)
				// This is according to Altirra, but it breaks DjayBee's test J in 1050 mode?!
				//sleep_us(is1050 ? (us_track_step_1050+us_head_settle_1050) : (au_full_rotation*8));
				// This is what seems to work:
				sleep_us(au_full_rotation*8);
		} else {
			// No matching sector found at all or the track does not match the disk density
			sleep_until(delayed_by_ms(headPosition.stamp, is1050 ? ms_2fake_rot_1050 : ms_3fake_rot_810));
			if(is1050 || retries == 2)
				// Repositioning the head for the target track
				if(!is1050)
					sleep_us((43+tgtTrackNumber)*us_track_step_810+us_head_settle_810);
				else if(tgtTrackNumber)
					sleep_us((2*tgtTrackNumber+1)*us_track_step_1050+us_head_settle_1050);
		}

		getCurrentHeadPosition(&headPosition);

		if(!*status || (op_write && tgtSectorOffset) || r < 0)
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

	if (tgtSectorOffset && !*status && r >= 0)
		r = 0;

	if(!op_write) {
		// if a weak offset is defined, randomize the appropriate data
		if (weakOffset > -1)
			for (i = (uint16_t) weakOffset; i < atx_sector_size; i++)
				sector_buffer[i] = (uint8_t) get_rand_32();
		sleep_until(delayed_by_us(headPosition.stamp, is1050 ? us_cs_calculation_1050 : us_cs_calculation_810));
		// This is probably equivalent in this case, some testing still needs to be done
		// to see which one works better for both the internal Flash and SD cards.
		//sleep_us(is1050 ? us_cs_calculation_1050 : us_cs_calculation_810);
	}else if(tgtSectorOffset) {
		if(writeStatusOffset) {
			sector_buffer[si] = *status;
			if(mounted_file_transfer(atx_drive_number+1, writeStatusOffset, 1, true, si) != FR_OK) {
				r = -1;
				ext_sector_size = 0;
			}
		}
		if(ext_sector_size > atx_sector_size)
			ext_sector_size = ext_sector_size - atx_sector_size;
		else
			ext_sector_size = 0;
		if((*status & mask_fdc_dlost) && ext_sector_size) {
			memset(&sector_buffer[si], 0xFF, 128);
			currentFileOffset = gTrackInfo[atx_drive_number][tgtTrackNumber] + tgtSectorOffset + atx_sector_size;
			while(ext_sector_size) {
				if(mounted_file_transfer(atx_drive_number+1, currentFileOffset, 128, true, si) != FR_OK) {
					r = -1;
					break;
				}
				currentFileOffset += 128;
				ext_sector_size -= 128;
			}
		}
	}

	// the Atari expects an inverted FDC status byte
	*status = ~(*status);

	return r;
}
