#include "flash_benchmark.h"
#include "flash_manager.h"
#include "system_time.h"
#include "w25q64.h"
#include <stddef.h>

#define BENCHMARK_PATTERN_SEED 0x5AU

/* One sector buffer keeps the sequential read in one SPI transaction while
 * staying within the STM32F103C8 20 KiB SRAM budget. */
static uint8_t benchmark_buffer[FLASH_BENCHMARK_SIZE];

static FlashBenchmarkStatus_t map_w25q64_status(W25Q64_Status_t status)
{
    if (status == W25Q64_OK) { return FLASH_BENCHMARK_OK; }
    if (status == W25Q64_TIMEOUT) { return FLASH_BENCHMARK_TIMEOUT; }
    if (status == W25Q64_INVALID_PARAM) { return FLASH_BENCHMARK_INVALID_PARAM; }
    return FLASH_BENCHMARK_FLASH_ERROR;
}

static FlashBenchmarkStatus_t map_manager_status(FlashManagerStatus_t status)
{
    if (status == FLASH_MANAGER_OK) { return FLASH_BENCHMARK_OK; }
    if (status == FLASH_MANAGER_TIMEOUT) { return FLASH_BENCHMARK_TIMEOUT; }
    if (status == FLASH_MANAGER_INVALID_PARAM) {
        return FLASH_BENCHMARK_INVALID_PARAM;
    }
    return FLASH_BENCHMARK_FLASH_ERROR;
}

static uint8_t pattern_byte(uint32_t offset)
{
    uint32_t mixed = offset * 37UL + (offset >> 3U) + BENCHMARK_PATTERN_SEED;
    return (uint8_t)(mixed ^ (mixed >> 8U));
}

static uint32_t speed_kib_per_second(uint32_t elapsed_ms)
{
    /* Exactly 4 KiB are transferred: 4 * 1000 / elapsed_ms. */
    return (4000UL + elapsed_ms / 2UL) / elapsed_ms;
}

FlashBenchmarkStatus_t FlashBenchmark_Run(
    uint32_t sector_index, FlashBenchmarkResult_t *result)
{
    FlashBenchmarkResult_t measured;
    FlashBenchmarkStatus_t status;
    FlashManagerStatus_t manager_status;
    W25Q64_Status_t flash_status;
    uint32_t page_offset;
    uint32_t index;
    uint32_t start_ms;
    uint32_t elapsed_ms;
    uint8_t cleanup_required = 0U;

    if (result == NULL) { return FLASH_BENCHMARK_INVALID_PARAM; }
    manager_status = FlashManager_GetSectorAddress(sector_index,
                                                   &measured.address);
    if (manager_status != FLASH_MANAGER_OK) {
        return map_manager_status(manager_status);
    }
    measured.sector_index = sector_index;
    flash_status = W25Q64_GetSPIClockHz(&measured.spi_clock_hz);
    if (flash_status != W25Q64_OK) { return map_w25q64_status(flash_status); }

    start_ms = SystemTime_GetMs();
    manager_status = FlashManager_PrepareSector(sector_index);
    elapsed_ms = SystemTime_GetMs() - start_ms;
    if (manager_status != FLASH_MANAGER_OK) {
        return map_manager_status(manager_status);
    }
    cleanup_required = 1U;
    measured.erase_time_ms = elapsed_ms;
    if (elapsed_ms == 0U) {
        status = FLASH_BENCHMARK_TIMER_ERROR;
        goto cleanup;
    }

    start_ms = SystemTime_GetMs();
    for (page_offset = 0U; page_offset < FLASH_BENCHMARK_SIZE;
         page_offset += FLASH_PAGE_SIZE)
    {
        for (index = 0U; index < FLASH_PAGE_SIZE; ++index) {
            benchmark_buffer[index] = pattern_byte(page_offset + index);
        }
        flash_status = W25Q64_PageProgram(measured.address + page_offset,
                                         benchmark_buffer,
                                         FLASH_PAGE_SIZE);
        if (flash_status != W25Q64_OK) {
            status = map_w25q64_status(flash_status);
            goto cleanup;
        }
    }
    elapsed_ms = SystemTime_GetMs() - start_ms;
    measured.write_time_ms = elapsed_ms;
    if (elapsed_ms == 0U) {
        status = FLASH_BENCHMARK_TIMER_ERROR;
        goto cleanup;
    }
    measured.write_speed_kib_per_s = speed_kib_per_second(elapsed_ms);

    start_ms = SystemTime_GetMs();
    flash_status = W25Q64_Read(measured.address, benchmark_buffer,
                              FLASH_BENCHMARK_SIZE);
    elapsed_ms = SystemTime_GetMs() - start_ms;
    if (flash_status != W25Q64_OK) {
        status = map_w25q64_status(flash_status);
        goto cleanup;
    }
    measured.read_time_ms = elapsed_ms;
    if (elapsed_ms == 0U) {
        status = FLASH_BENCHMARK_TIMER_ERROR;
        goto cleanup;
    }
    measured.read_speed_kib_per_s = speed_kib_per_second(elapsed_ms);
    for (index = 0U; index < FLASH_BENCHMARK_SIZE; ++index) {
        if (benchmark_buffer[index] != pattern_byte(index)) {
            status = FLASH_BENCHMARK_VERIFY_ERROR;
            goto cleanup;
        }
    }
    status = FLASH_BENCHMARK_OK;

cleanup:
    if (cleanup_required) {
        manager_status = FlashManager_PrepareSector(sector_index);
        if (manager_status != FLASH_MANAGER_OK) {
            return map_manager_status(manager_status);
        }
    }
    if (status == FLASH_BENCHMARK_OK) { *result = measured; }
    return status;
}
