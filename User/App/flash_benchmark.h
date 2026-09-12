#ifndef FLASH_BENCHMARK_H
#define FLASH_BENCHMARK_H

#include <stdint.h>

#define FLASH_BENCHMARK_SIZE 4096UL

typedef enum
{
    FLASH_BENCHMARK_OK = 0,
    FLASH_BENCHMARK_ERROR,
    FLASH_BENCHMARK_TIMEOUT,
    FLASH_BENCHMARK_INVALID_PARAM,
    FLASH_BENCHMARK_FLASH_ERROR,
    FLASH_BENCHMARK_VERIFY_ERROR,
    FLASH_BENCHMARK_TIMER_ERROR
} FlashBenchmarkStatus_t;

typedef struct
{
    uint32_t sector_index;
    uint32_t address;
    uint32_t spi_clock_hz;
    uint32_t read_time_ms;
    uint32_t read_speed_kib_per_s;
    uint32_t write_time_ms;
    uint32_t write_speed_kib_per_s;
    uint32_t erase_time_ms;
} FlashBenchmarkResult_t;

/* Destructively benchmark one log-relative sector and erase it again before
 * returning. The caller must ensure that sector contains no retained record. */
FlashBenchmarkStatus_t FlashBenchmark_Run(
    uint32_t sector_index, FlashBenchmarkResult_t *result);

#endif
