#include "disk_counter.hpp"

#ifdef PIO_DISK_COUNTER

#include "hardware/clocks.h"
#include "hardware/pio.h"
#include "hardware/dma.h"
#include "hardware/irq.h"

#include "atx.hpp"

#include "disk_counter.pio.h"

#define disk_pio pio1

volatile uint32_t disk_counter;

static int disk_dma_channel;

static void disk_dma_handler() {
	if(dma_hw->ints0 & (1u << disk_dma_channel)) {
		dma_hw->ints0 = 1u << disk_dma_channel;
		dma_channel_start(disk_dma_channel);
	}
}

void init_disk_counter() {
	uint disk_pio_offset = pio_add_program(disk_pio, &disk_counter_program);
	float disk_clk_divider = (float)clock_get_hz(clk_sys)/1000000;
	uint disk_sm = pio_claim_unused_sm(disk_pio, true);
	pio_sm_config disk_sm_config = disk_counter_program_get_default_config(disk_pio_offset);
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
}

#endif
