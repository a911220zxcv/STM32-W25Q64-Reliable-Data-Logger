#include "config.h"
#include "wear_leveling.h"
#include "flash_manager.h"
#include "power_fail_test.h"
#include <limits.h>
#include <stddef.h>

static uint32_t erase_counts[FLASH_LOG_SECTOR_COUNT];
static uint64_t total_erases;
static uint32_t next_erase_sector;
static WearLevelingStats_t cached_stats;
static uint8_t initialized;

static WearLevelingStatus_t map_flash_status(FlashManagerStatus_t status)
{
    switch (status)
    {
        case FLASH_MANAGER_OK: return WEAR_LEVELING_OK;
        case FLASH_MANAGER_TIMEOUT: return WEAR_LEVELING_TIMEOUT;
        case FLASH_MANAGER_INVALID_PARAM: return WEAR_LEVELING_INVALID_PARAM;
        case FLASH_MANAGER_NOT_INITIALIZED:
            return WEAR_LEVELING_NOT_INITIALIZED;
        default: return WEAR_LEVELING_FLASH_ERROR;
    }
}

static uint32_t next_sector_value(uint32_t sector_index)
{
    ++sector_index;
    return sector_index < FLASH_LOG_SECTOR_COUNT ? sector_index : 0U;
}

static void refresh_stats(void)
{
    uint32_t sector;
    uint32_t count;
    cached_stats.minimum_erase_count = UINT32_MAX;
    cached_stats.maximum_erase_count = 0U;
    cached_stats.sectors_at_minimum = 0U;
    cached_stats.sectors_at_maximum = 0U;
    for (sector = 0U; sector < FLASH_LOG_SECTOR_COUNT; ++sector)
    {
        count = erase_counts[sector];
        if (count < cached_stats.minimum_erase_count)
        {
            cached_stats.minimum_erase_count = count;
            cached_stats.sectors_at_minimum = 1U;
        }
        else if (count == cached_stats.minimum_erase_count)
        {
            ++cached_stats.sectors_at_minimum;
        }
        if (count > cached_stats.maximum_erase_count)
        {
            cached_stats.maximum_erase_count = count;
            cached_stats.sectors_at_maximum = 1U;
        }
        else if (count == cached_stats.maximum_erase_count)
        {
            ++cached_stats.sectors_at_maximum;
        }
    }
    cached_stats.total_erases = total_erases;
    cached_stats.erase_count_spread = cached_stats.maximum_erase_count -
                                      cached_stats.minimum_erase_count;
    cached_stats.next_sector = next_erase_sector;
}

WearLevelingStatus_t WearLeveling_Init(void)
{
    uint32_t sector;
    for (sector = 0U; sector < FLASH_LOG_SECTOR_COUNT; ++sector)
    {
        erase_counts[sector] = 0U;
    }
    total_erases = 0U;
    next_erase_sector = 0U;
    refresh_stats();
    initialized = 1U;
    return WEAR_LEVELING_OK;
}

WearLevelingStatus_t WearLeveling_PrepareNextSector(uint32_t *sector_index)
{
    FlashManagerStatus_t flash_status;
    uint32_t selected;
    if (sector_index == NULL) { return WEAR_LEVELING_INVALID_PARAM; }
    if (!initialized) { return WEAR_LEVELING_NOT_INITIALIZED; }
    selected = next_erase_sector;
    if (erase_counts[selected] == UINT32_MAX)
    {
        return WEAR_LEVELING_COUNTER_OVERFLOW;
    }
    flash_status = FlashManager_PrepareSector(selected);
    if (flash_status != FLASH_MANAGER_OK) { return map_flash_status(flash_status); }
    if (PowerFailTest_Hook(POWER_FAIL_POINT_LOG_SECTOR_ERASED))
    {
        return WEAR_LEVELING_ERROR;
    }
    ++erase_counts[selected];
    ++total_erases;
    next_erase_sector = next_sector_value(selected);
    refresh_stats();
    *sector_index = selected;
    return WEAR_LEVELING_OK;
}

WearLevelingStatus_t WearLeveling_PeekNextSector(uint32_t *sector_index)
{
    if (sector_index == NULL) { return WEAR_LEVELING_INVALID_PARAM; }
    if (!initialized) { return WEAR_LEVELING_NOT_INITIALIZED; }
    *sector_index = next_erase_sector;
    return WEAR_LEVELING_OK;
}

WearLevelingStatus_t WearLeveling_RestoreNextSector(uint32_t sector_index)
{
    if (!initialized) { return WEAR_LEVELING_NOT_INITIALIZED; }
    if (sector_index >= FLASH_LOG_SECTOR_COUNT)
    {
        return WEAR_LEVELING_INVALID_PARAM;
    }
    next_erase_sector = sector_index;
    cached_stats.next_sector = next_erase_sector;
    return WEAR_LEVELING_OK;
}

WearLevelingStatus_t WearLeveling_GetNextSector(uint32_t sector_index,
                                                uint32_t *next_sector)
{
    if ((next_sector == NULL) || (sector_index >= FLASH_LOG_SECTOR_COUNT))
    {
        return WEAR_LEVELING_INVALID_PARAM;
    }
    *next_sector = next_sector_value(sector_index);
    return WEAR_LEVELING_OK;
}

WearLevelingStatus_t WearLeveling_GetSectorEraseCount(uint32_t sector_index,
                                                      uint32_t *erase_count)
{
    if ((erase_count == NULL) || (sector_index >= FLASH_LOG_SECTOR_COUNT))
    {
        return WEAR_LEVELING_INVALID_PARAM;
    }
    if (!initialized) { return WEAR_LEVELING_NOT_INITIALIZED; }
    *erase_count = erase_counts[sector_index];
    return WEAR_LEVELING_OK;
}

WearLevelingStatus_t WearLeveling_GetStats(WearLevelingStats_t *stats)
{
    if (stats == NULL) { return WEAR_LEVELING_INVALID_PARAM; }
    if (!initialized) { return WEAR_LEVELING_NOT_INITIALIZED; }
    *stats = cached_stats;
    return WEAR_LEVELING_OK;
}

WearLevelingStatus_t WearLeveling_ValidateDistribution(uint32_t maximum_spread)
{
    WearLevelingStats_t stats;
    WearLevelingStatus_t status = WearLeveling_GetStats(&stats);
    if (status != WEAR_LEVELING_OK) { return status; }
    return stats.erase_count_spread <= maximum_spread ?
           WEAR_LEVELING_OK : WEAR_LEVELING_IMBALANCED;
}

#if DATA_LOGGER_TEST_ENABLE
WearLevelingStatus_t WearLeveling_TestSimulate(uint32_t complete_rounds,
                                               uint32_t extra_sectors)
{
    uint32_t sector;
    if (!initialized) { return WEAR_LEVELING_NOT_INITIALIZED; }
    if ((extra_sectors > FLASH_LOG_SECTOR_COUNT) ||
        (complete_rounds == UINT32_MAX))
    {
        return WEAR_LEVELING_INVALID_PARAM;
    }
    for (sector = 0U; sector < FLASH_LOG_SECTOR_COUNT; ++sector)
    {
        erase_counts[sector] = complete_rounds;
    }
    for (sector = 0U; sector < extra_sectors; ++sector)
    {
        ++erase_counts[sector];
    }
    total_erases = (uint64_t)complete_rounds *
                   (uint64_t)FLASH_LOG_SECTOR_COUNT + extra_sectors;
    next_erase_sector = extra_sectors < FLASH_LOG_SECTOR_COUNT ?
                        extra_sectors : 0U;
    refresh_stats();
    return WEAR_LEVELING_OK;
}

WearLevelingStatus_t WearLeveling_TestSetNextSector(uint32_t sector_index)
{
    return WearLeveling_RestoreNextSector(sector_index);
}
#endif
