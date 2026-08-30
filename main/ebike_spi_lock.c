#include "ebike_spi_lock.h"

#include "freertos/semphr.h"

static SemaphoreHandle_t s_spi_bus_semaphore;

bool ebike_spi_lock_init(void)
{
    if (s_spi_bus_semaphore != NULL) return true;
    s_spi_bus_semaphore = xSemaphoreCreateBinary();
    if (s_spi_bus_semaphore == NULL) return false;
    xSemaphoreGive(s_spi_bus_semaphore);
    return true;
}

bool ebike_spi_lock_take(TickType_t timeout_ticks)
{
    return s_spi_bus_semaphore != NULL &&
           xSemaphoreTake(s_spi_bus_semaphore, timeout_ticks) == pdTRUE;
}

void ebike_spi_lock_give(void)
{
    if (s_spi_bus_semaphore != NULL) xSemaphoreGive(s_spi_bus_semaphore);
}

bool ebike_spi_lock_give_from_isr(void)
{
    if (s_spi_bus_semaphore == NULL) return false;
    BaseType_t higher_priority_task_woken = pdFALSE;
    xSemaphoreGiveFromISR(s_spi_bus_semaphore, &higher_priority_task_woken);
    return higher_priority_task_woken == pdTRUE;
}
