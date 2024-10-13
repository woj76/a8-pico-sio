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

/* spi.c
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

// #include <assert.h>
#include <stdbool.h>
//
#include "pico/stdlib.h"
#include "pico/mutex.h"
#include "pico/sem.h"

#include "spi.h"


// SPI Transfer: Read & Write (simultaneously) on SPI bus
//   If the data that will be received is not important, pass NULL as rx.
//   If the data that will be transmitted is not important,
//     pass NULL as tx and then the SPI_FILL_CHAR is sent out as each data
//     element.
bool spi_transfer(spi_t *spi_p, const uint8_t *tx, uint8_t *rx, size_t length) {

    // tx write increment is already false
    if (tx) {
        channel_config_set_read_increment(&spi_p->tx_dma_cfg, true);
    } else {
        static const uint8_t dummy = SPI_FILL_CHAR;
        tx = &dummy;
        channel_config_set_read_increment(&spi_p->tx_dma_cfg, false);
    }

    // rx read increment is already false
    if (rx) {
        channel_config_set_write_increment(&spi_p->rx_dma_cfg, true);
    } else {
        static uint8_t dummy = 0xA5;
        rx = &dummy;
        channel_config_set_write_increment(&spi_p->rx_dma_cfg, false);
    }

    dma_channel_configure(spi_p->tx_dma, &spi_p->tx_dma_cfg,
                          &spi_get_hw(spi_p->hw_inst)->dr,  // write address
                          tx,                              // read address
                          length,  // element count (each element is of
                                   // size transfer_data_size)
                          false);  // start
    dma_channel_configure(spi_p->rx_dma, &spi_p->rx_dma_cfg,
                          rx,                              // write address
                          &spi_get_hw(spi_p->hw_inst)->dr,  // read address
                          length,  // element count (each element is of
                                   // size transfer_data_size)
                          false);  // start

    // start them exactly simultaneously to avoid races (in extreme cases
    // the FIFO could overflow)
    dma_start_channel_mask((1u << spi_p->tx_dma) | (1u << spi_p->rx_dma));

    absolute_time_t t = get_absolute_time();
    while (dma_channel_is_busy(spi_p->rx_dma) && absolute_time_diff_us(t, get_absolute_time()) < 1000000)
	    tight_loop_contents();
    return !dma_channel_is_busy(spi_p->rx_dma);
}

void spi_lock(spi_t *spi_p) {
    mutex_enter_blocking(&spi_p->mutex);
}

void spi_unlock(spi_t *spi_p) {
    mutex_exit(&spi_p->mutex);
}

bool my_spi_init(spi_t *spi_p) {
    auto_init_mutex(my_spi_init_mutex);
    mutex_enter_blocking(&my_spi_init_mutex);
    if (!spi_p->initialized) {

        if (!mutex_is_initialized(&spi_p->mutex)) mutex_init(&spi_p->mutex);
        spi_lock(spi_p);

        // Default:
        if (!spi_p->baud_rate)
            spi_p->baud_rate = 10 * 1000 * 1000;

        /* Configure component */
        // Enable SPI at 100 kHz and connect to GPIOs
        spi_init(spi_p->hw_inst, 100 * 1000);
        spi_set_format(spi_p->hw_inst, 8, SPI_CPOL_0, SPI_CPHA_0, SPI_MSB_FIRST);

        gpio_set_function(spi_p->miso_gpio, GPIO_FUNC_SPI);
        gpio_set_function(spi_p->mosi_gpio, GPIO_FUNC_SPI);
        gpio_set_function(spi_p->sck_gpio, GPIO_FUNC_SPI);

        // ss_gpio is initialized in sd_init_driver()

        // Slew rate limiting levels for GPIO outputs.
        // enum gpio_slew_rate { GPIO_SLEW_RATE_SLOW = 0, GPIO_SLEW_RATE_FAST = 1 }
        // void gpio_set_slew_rate (uint gpio,enum gpio_slew_rate slew)
        // Default appears to be GPIO_SLEW_RATE_SLOW.

        // Drive strength levels for GPIO outputs.
        // enum gpio_drive_strength { GPIO_DRIVE_STRENGTH_2MA = 0, GPIO_DRIVE_STRENGTH_4MA = 1, GPIO_DRIVE_STRENGTH_8MA = 2,
        // GPIO_DRIVE_STRENGTH_12MA = 3 }
        // enum gpio_drive_strength gpio_get_drive_strength (uint gpio)
/*
        if (spi_p->set_drive_strength) {
            gpio_set_drive_strength(spi_p->mosi_gpio, spi_p->mosi_gpio_drive_strength);
            gpio_set_drive_strength(spi_p->sck_gpio, spi_p->sck_gpio_drive_strength);
        }
*/

	gpio_set_drive_strength(spi_p->mosi_gpio, sd_gpio_drive_strength);
	gpio_set_drive_strength(spi_p->sck_gpio, sd_gpio_drive_strength);

        // SD cards' DO MUST be pulled up.
        // This is done in hardware with a 4K7 resistor
        // gpio_pull_up(spi_p->miso_gpio);

        // Grab some unused dma channels
        spi_p->tx_dma = dma_claim_unused_channel(true);
        spi_p->rx_dma = dma_claim_unused_channel(true);

        spi_p->tx_dma_cfg = dma_channel_get_default_config(spi_p->tx_dma);
        spi_p->rx_dma_cfg = dma_channel_get_default_config(spi_p->rx_dma);
        channel_config_set_transfer_data_size(&spi_p->tx_dma_cfg, DMA_SIZE_8);
        channel_config_set_transfer_data_size(&spi_p->rx_dma_cfg, DMA_SIZE_8);

        // We set the outbound DMA to transfer from a memory buffer to the SPI
        // transmit FIFO paced by the SPI TX FIFO DREQ The default is for the
        // read address to increment every element (in this case 1 byte -
        // DMA_SIZE_8) and for the write address to remain unchanged.
        channel_config_set_dreq(&spi_p->tx_dma_cfg, spi_get_index(spi_p->hw_inst)
                                                       ? DREQ_SPI1_TX
                                                       : DREQ_SPI0_TX);
        channel_config_set_write_increment(&spi_p->tx_dma_cfg, false);

        // We set the inbound DMA to transfer from the SPI receive FIFO to a
        // memory buffer paced by the SPI RX FIFO DREQ We coinfigure the read
        // address to remain unchanged for each element, but the write address
        // to increment (so data is written throughout the buffer)
        channel_config_set_dreq(&spi_p->rx_dma_cfg, spi_get_index(spi_p->hw_inst)
                                                       ? DREQ_SPI1_RX
                                                       : DREQ_SPI0_RX);
        channel_config_set_read_increment(&spi_p->rx_dma_cfg, false);

        LED_INIT();
        spi_p->initialized = true;
        spi_unlock(spi_p);
    }
    mutex_exit(&my_spi_init_mutex);
    return true;
}
