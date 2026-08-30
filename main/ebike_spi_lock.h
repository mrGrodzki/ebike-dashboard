#pragma once

#include <stdbool.h>

#include "freertos/FreeRTOS.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * LCD and microSD share SPI2. The LCD finishes queued DMA transfers in an ISR,
 * so this must be a binary semaphore rather than a FreeRTOS mutex.
 */
bool ebike_spi_lock_init(void);
bool ebike_spi_lock_take(TickType_t timeout_ticks);
void ebike_spi_lock_give(void);
bool ebike_spi_lock_give_from_isr(void);

#ifdef __cplusplus
}
#endif
