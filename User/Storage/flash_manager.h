#ifndef FLASH_MANAGER_H
#define FLASH_MANAGER_H

#include <stdint.h>

/* W25Q64 physical map. LOG_END_ADDR is exclusive; LOG_LAST_ADDR is inclusive. */
#define FLASH_TOTAL_SIZE             0x00800000UL
#define FLASH_PAGE_SIZE              256UL
#define FLASH_SECTOR_SIZE            4096UL
#define METADATA_A_ADDR              0x00000000UL
#define METADATA_B_ADDR              0x00001000UL
#define LOG_START_ADDR               0x00002000UL
#define LOG_END_ADDR                 FLASH_TOTAL_SIZE
#define LOG_LAST_ADDR                (LOG_END_ADDR - 1UL)
#define LOG_SIZE                     (LOG_END_ADDR - LOG_START_ADDR)

#define FLASH_LOG_RECORD_MAGIC       0x31474F4CUL /* "LOG1" in little endian */
#define FLASH_LOG_RECORD_SIZE        28UL
#define FLASH_LOG_CRC_OFFSET         24UL
#define FLASH_LOG_SECTOR_COUNT       (LOG_SIZE / FLASH_SECTOR_SIZE)
#define FLASH_RECORDS_PER_SECTOR     (FLASH_SECTOR_SIZE / FLASH_LOG_RECORD_SIZE)
#define FLASH_SECTOR_UNUSED_BYTES    \
    (FLASH_SECTOR_SIZE - FLASH_RECORDS_PER_SECTOR * FLASH_LOG_RECORD_SIZE)
#define FLASH_LOG_RECORD_CAPACITY    \
    (FLASH_LOG_SECTOR_COUNT * FLASH_RECORDS_PER_SECTOR)

#define FLASH_METADATA_MAGIC         0x4154454DUL /* bytes "META" */
#define FLASH_METADATA_VERSION       1UL
#define FLASH_METADATA_SIZE          36UL
#define FLASH_METADATA_CRC_OFFSET    32UL

#if (FLASH_TOTAL_SIZE != 0x00800000UL) || (FLASH_PAGE_SIZE != 256UL) || \
    (FLASH_SECTOR_SIZE != 4096UL)
#error "Flash Manager geometry must match W25Q64"
#endif
#if ((LOG_START_ADDR % FLASH_SECTOR_SIZE) != 0UL) || \
    ((LOG_SIZE % FLASH_SECTOR_SIZE) != 0UL) || \
    (FLASH_SECTOR_UNUSED_BYTES != 8UL)
#error "Invalid Phase 2 flash layout"
#endif

typedef enum
{
    FLASH_MANAGER_OK = 0,
    FLASH_MANAGER_ERROR,
    FLASH_MANAGER_TIMEOUT,
    FLASH_MANAGER_INVALID_PARAM,
    FLASH_MANAGER_NOT_INITIALIZED,
    FLASH_MANAGER_FLASH_ERROR,
    FLASH_MANAGER_UNSUPPORTED_DEVICE,
    FLASH_MANAGER_NOT_ERASED,
    FLASH_MANAGER_EMPTY,
    FLASH_MANAGER_INVALID_RECORD,
    FLASH_MANAGER_CRC_ERROR,
    FLASH_MANAGER_NO_METADATA,
    FLASH_MANAGER_RECOVERY_ERROR
} FlashManagerStatus_t;

typedef enum
{
    FLASH_METADATA_COPY_NONE = 0,
    FLASH_METADATA_COPY_A,
    FLASH_METADATA_COPY_B
} FlashMetadataCopy_t;

/* In-RAM representation only. Its compiler padding is never written to flash. */
typedef struct
{
    uint32_t magic;
    uint32_t sequence;
    uint32_t timestamp_ms;
    int16_t accel_x;
    int16_t accel_y;
    int16_t accel_z;
    int16_t gyro_x;
    int16_t gyro_y;
    int16_t gyro_z;
    uint32_t crc32;
} LogRecord_t;

typedef struct
{
    uint32_t magic;
    uint32_t version;
    uint32_t generation;
    uint32_t write_sector;
    uint32_t write_offset;
    uint32_t oldest_sector;
    uint32_t record_count;
    uint32_t next_sequence;
    uint32_t crc32;
} FlashMetadata_t;

typedef struct
{
    uint32_t write_sector;
    uint32_t write_slot;
    uint32_t oldest_sector;
    uint32_t oldest_slot;
    uint32_t record_count;
    uint32_t next_sequence;
} FlashManagerState_t;

typedef struct
{
    FlashManagerState_t state;
    FlashMetadataCopy_t active_copy;
    uint32_t generation;
    uint32_t last_timestamp_ms;
    uint8_t metadata_valid;
    uint8_t used_log_scan;
    uint8_t has_gaps;
} FlashManagerRecovery_t;

/* Driver must already be initialized. Performs a read-only JEDEC check. */
FlashManagerStatus_t FlashManager_Init(void);

/* Pure checked address conversion. sector_index is relative to LOG_START_ADDR. */
FlashManagerStatus_t FlashManager_GetSectorAddress(uint32_t sector_index,
                                                   uint32_t *address);
FlashManagerStatus_t FlashManager_GetRecordAddress(uint32_t sector_index,
                                                   uint32_t slot_index,
                                                   uint32_t *address);

/* Explicit little-endian 28-byte format. Serialize calculates CRC and ignores
 * record->crc32. Deserialize validates erased state, magic and stored CRC.
 * Output record is modified only on success. */
FlashManagerStatus_t FlashManager_SerializeRecord(
    const LogRecord_t *record, uint8_t output[FLASH_LOG_RECORD_SIZE]);
FlashManagerStatus_t FlashManager_DeserializeRecord(
    const uint8_t input[FLASH_LOG_RECORD_SIZE], LogRecord_t *record);

/* Destructive operation limited to one log sector; metadata is inaccessible. */
FlashManagerStatus_t FlashManager_PrepareSector(uint32_t sector_index);

/* Target slot must be entirely 0xFF. No implicit erase and no overwrite.
 * A record never crosses a sector; the W25Q64 driver may split it across pages. */
FlashManagerStatus_t FlashManager_WriteRecord(uint32_t sector_index,
                                              uint32_t slot_index,
                                              const LogRecord_t *record);
FlashManagerStatus_t FlashManager_ReadRecord(uint32_t sector_index,
                                             uint32_t slot_index,
                                             LogRecord_t *record);

/* Deterministic 36-byte dual-metadata format. CRC excludes its own field. */
FlashManagerStatus_t FlashManager_SerializeMetadata(
    const FlashMetadata_t *metadata,
    uint8_t output[FLASH_METADATA_SIZE]);
FlashManagerStatus_t FlashManager_DeserializeMetadata(
    const uint8_t input[FLASH_METADATA_SIZE], FlashMetadata_t *metadata);
FlashManagerStatus_t FlashManager_ReadMetadata(FlashMetadataCopy_t copy,
                                               FlashMetadata_t *metadata);
/* Recover state from A/B and uncommitted tail; full log scan is fallback. */
FlashManagerStatus_t FlashManager_Recover(FlashManagerRecovery_t *recovery);
/* Alternate A/B, erase target, program, read back and verify before commit. */
FlashManagerStatus_t FlashManager_CommitState(const FlashManagerState_t *state,
                                              uint32_t *generation);
FlashManagerStatus_t FlashManager_GetLastRecovery(
    FlashManagerRecovery_t *recovery);
/* Recovery helpers for a sector whose corrupt tail has been abandoned. */
FlashManagerStatus_t FlashManager_FindRecordBySequence(
    const FlashManagerState_t *state, uint32_t sequence, LogRecord_t *record);
FlashManagerStatus_t FlashManager_CountSectorPrefix(uint32_t sector_index,
                                                    uint32_t first_sequence,
                                                    uint32_t *record_count);

#if defined(DATA_LOGGER_TEST_ENABLE) && DATA_LOGGER_TEST_ENABLE
/* Destructive metadata-sector test hook; performs no log-area erase. */
FlashManagerStatus_t FlashManager_TestResetMetadata(void);
#endif

#endif
