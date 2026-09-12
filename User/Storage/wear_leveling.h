#ifndef WEAR_LEVELING_H
#define WEAR_LEVELING_H

#include <stdint.h>

#define WEAR_LEVELING_MAX_IDEAL_SPREAD 1UL

typedef enum
{
    WEAR_LEVELING_OK = 0,
    WEAR_LEVELING_ERROR,
    WEAR_LEVELING_TIMEOUT,
    WEAR_LEVELING_INVALID_PARAM,
    WEAR_LEVELING_NOT_INITIALIZED,
    WEAR_LEVELING_FLASH_ERROR,
    WEAR_LEVELING_COUNTER_OVERFLOW,
    WEAR_LEVELING_IMBALANCED
} WearLevelingStatus_t;

typedef struct
{
    uint64_t total_erases;
    uint32_t minimum_erase_count;
    uint32_t maximum_erase_count;
    uint32_t erase_count_spread;
    uint32_t sectors_at_minimum;
    uint32_t sectors_at_maximum;
    uint32_t next_sector;
} WearLevelingStats_t;

/* Reset debug-only RAM statistics and start round-robin selection at sector 0. */
WearLevelingStatus_t WearLeveling_Init(void);
/* Erase the next round-robin log sector. Counter and cursor change only after
 * FlashManager_PrepareSector succeeds. Output is modified only on success. */
WearLevelingStatus_t WearLeveling_PrepareNextSector(uint32_t *sector_index);
WearLevelingStatus_t WearLeveling_PeekNextSector(uint32_t *sector_index);
/* Restore volatile allocator position from recovered logger metadata. */
WearLevelingStatus_t WearLeveling_RestoreNextSector(uint32_t sector_index);
/* Pure checked circular sector increment used by the logger pointers. */
WearLevelingStatus_t WearLeveling_GetNextSector(uint32_t sector_index,
                                                uint32_t *next_sector);
WearLevelingStatus_t WearLeveling_GetSectorEraseCount(uint32_t sector_index,
                                                      uint32_t *erase_count);
WearLevelingStatus_t WearLeveling_GetStats(WearLevelingStats_t *stats);
WearLevelingStatus_t WearLeveling_ValidateDistribution(uint32_t maximum_spread);

#if defined(DATA_LOGGER_TEST_ENABLE) && DATA_LOGGER_TEST_ENABLE
/* RAM-only acceleration for hardware acceptance; performs no Flash erase. */
WearLevelingStatus_t WearLeveling_TestSimulate(uint32_t complete_rounds,
                                               uint32_t extra_sectors);
/* Used only to reach the physical end boundary in the Phase 4 test. */
WearLevelingStatus_t WearLeveling_TestSetNextSector(uint32_t sector_index);
#endif

#endif
