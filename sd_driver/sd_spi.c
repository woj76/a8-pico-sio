/*
 * This file is part of the a8-pico-sio project --
 * An Atari 8-bit SIO drive and (turbo) tape emulator for
 * Raspberry Pi Pico, see
 *
 *         https://github.com/woj76/a8-pico-sio
 *
 * For information on what / whose work it is based on, check below and the
 * corresponding project repository.
 */

/* This file or its parts come originally from the no-OS-FatFS-SD-SPI-RPi-Pico
 * project, see https://github.com/carlk3/no-OS-FatFS-SD-SPI-RPi-Pico */

/* sd_spi.c
Copyright 2021 Carl John Kugler III

Licensed under the Apache License, Version 2.0 (the License); you may not use
this file except in compliance with the License. You may obtain a copy of the
License at

   http://www.apache.org/licenses/LICENSE-2.0
Unless required by applicable law or agreed to in writing, software distributed
under the License is distributed on an AS IS BASIS, WITHOUT WARRANTIES OR
CONDITIONS OF ANY KIND, either express or implied. See the License for the
specific language governing permissions and limitations under the License.
*/

/* Standard includes. */
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "hardware/gpio.h"

#include "sd_card.h"
#include "sd_spi.h"
#include "spi.h"

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-variable"

void sd_spi_go_high_frequency(sd_card_t *pSD) {
	spi_set_baudrate(pSD->spi->hw_inst, pSD->spi->baud_rate);
}
void sd_spi_go_low_frequency(sd_card_t *pSD) {
	spi_set_baudrate(pSD->spi->hw_inst, 400 * 1000); // Actual frequency: 398089
}

#pragma GCC diagnostic pop

static void sd_spi_lock(sd_card_t *pSD) {
	spi_lock(pSD->spi);
}

static void sd_spi_unlock(sd_card_t *pSD) {
	spi_unlock(pSD->spi);
}

// Would do nothing if pSD->ss_gpio were set to GPIO_FUNC_SPI.
static void sd_spi_select(sd_card_t *pSD) {
	//gpio_put(pSD->ss_gpio, 0);
	// A fill byte seems to be necessary, sometimes:
	uint8_t fill = SPI_FILL_CHAR;
	spi_write_blocking(pSD->spi->hw_inst, &fill, 1);
	LED_ON();
}

static void sd_spi_deselect(sd_card_t *pSD) {
	//gpio_put(pSD->ss_gpio, 1);
	LED_OFF();
	uint8_t fill = SPI_FILL_CHAR;
	spi_write_blocking(pSD->spi->hw_inst, &fill, 1);
}

/* Some SD cards want to be deselected between every bus transaction */
/*
void sd_spi_deselect_pulse(sd_card_t *pSD) {
	sd_spi_deselect(pSD);
	sd_spi_select(pSD);
}
*/

void sd_spi_acquire(sd_card_t *pSD) {
	sd_spi_lock(pSD);
	sd_spi_select(pSD);
}

void sd_spi_release(sd_card_t *pSD) {
	sd_spi_deselect(pSD);
	sd_spi_unlock(pSD);
}

bool sd_spi_transfer(sd_card_t *pSD, const uint8_t *tx, uint8_t *rx, size_t length) {
	return spi_transfer(pSD->spi, tx, rx, length);
}

uint8_t sd_spi_write(sd_card_t *pSD, const uint8_t value) {
	uint8_t received = SPI_FILL_CHAR;
	spi_transfer(pSD->spi, &value, &received, 1);
	return received;
}

void sd_spi_send_initializing_sequence(sd_card_t * pSD) {
	//bool old_ss = gpio_get(pSD->ss_gpio);
	//gpio_put(pSD->ss_gpio, 1);
	uint8_t ones[10];
	memset(ones, 0xFF, sizeof ones);
	absolute_time_t timeout_time = make_timeout_time_ms(1);
	do {
		sd_spi_transfer(pSD, ones, NULL, sizeof ones);
	} while (0 < absolute_time_diff_us(get_absolute_time(), timeout_time));
	//gpio_put(pSD->ss_gpio, old_ss);
}

/* [] END OF FILE */
