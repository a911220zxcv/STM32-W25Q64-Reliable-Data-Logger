#include "flash_manager.h"
#include "crc32.h"
#include "w25q64.h"
#include "config.h"
#include "power_fail_test.h"
#include <stddef.h>

#define RECORD_MAGIC_OFFSET       0UL
#define RECORD_SEQUENCE_OFFSET    4UL
#define RECORD_TIMESTAMP_OFFSET   8UL
#define RECORD_ACCEL_X_OFFSET     12UL
#define RECORD_ACCEL_Y_OFFSET     14UL
#define RECORD_ACCEL_Z_OFFSET     16UL
#define RECORD_GYRO_X_OFFSET      18UL
#define RECORD_GYRO_Y_OFFSET      20UL
#define RECORD_GYRO_Z_OFFSET      22UL

#define METADATA_MAGIC_OFFSET          0UL
#define METADATA_VERSION_OFFSET        4UL
#define METADATA_GENERATION_OFFSET     8UL
#define METADATA_WRITE_SECTOR_OFFSET  12UL
#define METADATA_WRITE_OFFSET_OFFSET  16UL
#define METADATA_OLDEST_SECTOR_OFFSET 20UL
#define METADATA_RECORD_COUNT_OFFSET  24UL
#define METADATA_NEXT_SEQUENCE_OFFSET 28UL

static uint8_t initialized;
static uint8_t metadata_loaded;
static FlashMetadataCopy_t active_metadata_copy;
static uint32_t active_generation;
static FlashManagerRecovery_t last_recovery;
static uint8_t recovery_sector_buffer[FLASH_SECTOR_SIZE];
static uint32_t cached_recovery_sector = FLASH_LOG_SECTOR_COUNT;

static void write_u16_le(uint8_t *output, uint16_t value)
{
    output[0] = (uint8_t)value;
    output[1] = (uint8_t)(value >> 8U);
}

static void write_u32_le(uint8_t *output, uint32_t value)
{
    output[0] = (uint8_t)value;
    output[1] = (uint8_t)(value >> 8U);
    output[2] = (uint8_t)(value >> 16U);
    output[3] = (uint8_t)(value >> 24U);
}

static uint16_t read_u16_le(const uint8_t *input)
{
    return (uint16_t)((uint16_t)input[0] | ((uint16_t)input[1] << 8U));
}

static int16_t read_i16_le(const uint8_t *input)
{
    uint16_t value = read_u16_le(input);
    if (value <= 0x7FFFU) { return (int16_t)value; }
    return (int16_t)((int32_t)value - 65536L);
}

static uint32_t read_u32_le(const uint8_t *input)
{
    return (uint32_t)input[0] |
           ((uint32_t)input[1] << 8U) |
           ((uint32_t)input[2] << 16U) |
           ((uint32_t)input[3] << 24U);
}

static FlashManagerStatus_t map_flash_status(W25Q64_Status_t status)
{
    switch (status)
    {
        case W25Q64_OK: return FLASH_MANAGER_OK;
        case W25Q64_TIMEOUT: return FLASH_MANAGER_TIMEOUT;
        case W25Q64_INVALID_PARAM: return FLASH_MANAGER_INVALID_PARAM;
        case W25Q64_NOT_INITIALIZED: return FLASH_MANAGER_NOT_INITIALIZED;
        case W25Q64_UNSUPPORTED_DEVICE: return FLASH_MANAGER_UNSUPPORTED_DEVICE;
        default: return FLASH_MANAGER_FLASH_ERROR;
    }
}

static uint8_t is_erased(const uint8_t *data, uint32_t length)
{
    uint32_t index;
    for (index = 0U; index < length; ++index)
    {
        if (data[index] != 0xFFU) { return 0U; }
    }
    return 1U;
}

static uint8_t is_newer_u32(uint32_t candidate, uint32_t reference)
{
    return (uint8_t)((int32_t)(candidate - reference) > 0);
}

static uint32_t next_position(uint32_t position)
{
    ++position;
    return position < FLASH_LOG_RECORD_CAPACITY ? position : 0U;
}

static uint32_t previous_position(uint32_t position)
{
    return position == 0U ? FLASH_LOG_RECORD_CAPACITY - 1U : position - 1U;
}

static void position_to_state(uint32_t position, uint32_t *sector,
                              uint32_t *slot)
{
    *sector = position / FLASH_RECORDS_PER_SECTOR;
    *slot = position % FLASH_RECORDS_PER_SECTOR;
}

static uint32_t state_position(uint32_t sector, uint32_t slot)
{
    return sector * FLASH_RECORDS_PER_SECTOR + slot;
}

static uint32_t physical_distance(uint32_t oldest_position,
                                  uint32_t write_position)
{
    return write_position >= oldest_position ?
           write_position - oldest_position :
           FLASH_LOG_RECORD_CAPACITY - oldest_position + write_position;
}

static uint32_t state_physical_span(const FlashManagerState_t *state)
{
    uint32_t distance = physical_distance(
        state_position(state->oldest_sector, state->oldest_slot),
        state_position(state->write_sector, state->write_slot));
    if ((distance == 0U) && (state->record_count != 0U))
    {
        return FLASH_LOG_RECORD_CAPACITY;
    }
    return distance;
}

static uint8_t state_has_gaps(const FlashManagerState_t *state)
{
    return (uint8_t)(state_physical_span(state) != state->record_count);
}

static uint8_t state_is_valid(const FlashManagerState_t *state)
{
    uint32_t write_position;
    uint32_t oldest_position;
    uint32_t distance;
    if ((state == NULL) ||
        (state->write_sector >= FLASH_LOG_SECTOR_COUNT) ||
        (state->write_slot >= FLASH_RECORDS_PER_SECTOR) ||
        (state->oldest_sector >= FLASH_LOG_SECTOR_COUNT) ||
        (state->oldest_slot != 0U) ||
        (state->record_count > FLASH_LOG_RECORD_CAPACITY))
    {
        return 0U;
    }
    write_position = state_position(state->write_sector, state->write_slot);
    oldest_position = state_position(state->oldest_sector, state->oldest_slot);
    distance = physical_distance(oldest_position, write_position);
    if (state->record_count == 0U)
    {
        return (uint8_t)(distance == 0U);
    }
    if (distance == 0U)
    {
        return 1U;
    }
    return (uint8_t)(state->record_count <= distance);
}

static FlashManagerStatus_t read_position(uint32_t position,
                                          LogRecord_t *record)
{
    uint32_t sector;
    uint32_t slot;
    uint32_t address;
    FlashManagerStatus_t status;
    if ((record == NULL) || (position >= FLASH_LOG_RECORD_CAPACITY))
    {
        return FLASH_MANAGER_INVALID_PARAM;
    }
    position_to_state(position, &sector, &slot);
    if (cached_recovery_sector != sector)
    {
        status = FlashManager_GetSectorAddress(sector, &address);
        if (status != FLASH_MANAGER_OK) { return status; }
        status = map_flash_status(W25Q64_Read(address, recovery_sector_buffer,
                                               sizeof(recovery_sector_buffer)));
        if (status != FLASH_MANAGER_OK) { return status; }
        cached_recovery_sector = sector;
    }
    return FlashManager_DeserializeRecord(
               recovery_sector_buffer + slot * FLASH_LOG_RECORD_SIZE, record);
}

static FlashManagerStatus_t metadata_address(FlashMetadataCopy_t copy,
                                             uint32_t *address)
{
    if (address == NULL) { return FLASH_MANAGER_INVALID_PARAM; }
    if (copy == FLASH_METADATA_COPY_A) { *address = METADATA_A_ADDR; }
    else if (copy == FLASH_METADATA_COPY_B) { *address = METADATA_B_ADDR; }
    else { return FLASH_MANAGER_INVALID_PARAM; }
    return FLASH_MANAGER_OK;
}

static uint8_t metadata_fields_valid(const FlashMetadata_t *metadata)
{
    FlashManagerState_t state;
    if ((metadata == NULL) || (metadata->magic != FLASH_METADATA_MAGIC) ||
        (metadata->version != FLASH_METADATA_VERSION) ||
        ((metadata->write_offset % FLASH_LOG_RECORD_SIZE) != 0U))
    {
        return 0U;
    }
    state.write_sector = metadata->write_sector;
    state.write_slot = metadata->write_offset / FLASH_LOG_RECORD_SIZE;
    state.oldest_sector = metadata->oldest_sector;
    state.oldest_slot = 0U;
    state.record_count = metadata->record_count;
    state.next_sequence = metadata->next_sequence;
    return state_is_valid(&state);
}

FlashManagerStatus_t FlashManager_Init(void)
{
    uint32_t id;
    FlashManagerStatus_t status;
    initialized = 0U;
    metadata_loaded = 0U;
    active_metadata_copy = FLASH_METADATA_COPY_NONE;
    active_generation = 0U;
    cached_recovery_sector = FLASH_LOG_SECTOR_COUNT;
    status = map_flash_status(W25Q64_ReadJEDECID(&id));
    if (status != FLASH_MANAGER_OK) { return status; }
    if (id != W25Q64_EXPECTED_JEDEC_ID)
    {
        return FLASH_MANAGER_UNSUPPORTED_DEVICE;
    }
    initialized = 1U;
    return FLASH_MANAGER_OK;
}

FlashManagerStatus_t FlashManager_GetSectorAddress(uint32_t sector_index,
                                                   uint32_t *address)
{
    if ((address == NULL) || (sector_index >= FLASH_LOG_SECTOR_COUNT))
    {
        return FLASH_MANAGER_INVALID_PARAM;
    }
    *address = LOG_START_ADDR + sector_index * FLASH_SECTOR_SIZE;
    return FLASH_MANAGER_OK;
}

FlashManagerStatus_t FlashManager_GetRecordAddress(uint32_t sector_index,
                                                   uint32_t slot_index,
                                                   uint32_t *address)
{
    FlashManagerStatus_t status;
    uint32_t sector_address;
    if ((address == NULL) || (slot_index >= FLASH_RECORDS_PER_SECTOR))
    {
        return FLASH_MANAGER_INVALID_PARAM;
    }
    status = FlashManager_GetSectorAddress(sector_index, &sector_address);
    if (status != FLASH_MANAGER_OK) { return status; }
    *address = sector_address + slot_index * FLASH_LOG_RECORD_SIZE;
    return FLASH_MANAGER_OK;
}

FlashManagerStatus_t FlashManager_SerializeRecord(
    const LogRecord_t *record, uint8_t output[FLASH_LOG_RECORD_SIZE])
{
    uint32_t crc;
    if ((record == NULL) || (output == NULL))
    {
        return FLASH_MANAGER_INVALID_PARAM;
    }
    if (record->magic != FLASH_LOG_RECORD_MAGIC)
    {
        return FLASH_MANAGER_INVALID_RECORD;
    }
    write_u32_le(output + RECORD_MAGIC_OFFSET, record->magic);
    write_u32_le(output + RECORD_SEQUENCE_OFFSET, record->sequence);
    write_u32_le(output + RECORD_TIMESTAMP_OFFSET, record->timestamp_ms);
    write_u16_le(output + RECORD_ACCEL_X_OFFSET, (uint16_t)record->accel_x);
    write_u16_le(output + RECORD_ACCEL_Y_OFFSET, (uint16_t)record->accel_y);
    write_u16_le(output + RECORD_ACCEL_Z_OFFSET, (uint16_t)record->accel_z);
    write_u16_le(output + RECORD_GYRO_X_OFFSET, (uint16_t)record->gyro_x);
    write_u16_le(output + RECORD_GYRO_Y_OFFSET, (uint16_t)record->gyro_y);
    write_u16_le(output + RECORD_GYRO_Z_OFFSET, (uint16_t)record->gyro_z);
    crc = CRC32_Calculate(output, FLASH_LOG_CRC_OFFSET);
    write_u32_le(output + FLASH_LOG_CRC_OFFSET, crc);
    return FLASH_MANAGER_OK;
}

FlashManagerStatus_t FlashManager_DeserializeRecord(
    const uint8_t input[FLASH_LOG_RECORD_SIZE], LogRecord_t *record)
{
    LogRecord_t result;
    uint32_t stored_crc;
    uint32_t calculated_crc;
    if ((input == NULL) || (record == NULL))
    {
        return FLASH_MANAGER_INVALID_PARAM;
    }
    if (is_erased(input, FLASH_LOG_RECORD_SIZE))
    {
        return FLASH_MANAGER_EMPTY;
    }
    result.magic = read_u32_le(input + RECORD_MAGIC_OFFSET);
    if (result.magic != FLASH_LOG_RECORD_MAGIC)
    {
        return FLASH_MANAGER_INVALID_RECORD;
    }
    stored_crc = read_u32_le(input + FLASH_LOG_CRC_OFFSET);
    calculated_crc = CRC32_Calculate(input, FLASH_LOG_CRC_OFFSET);
    if (stored_crc != calculated_crc)
    {
        return FLASH_MANAGER_CRC_ERROR;
    }
    result.sequence = read_u32_le(input + RECORD_SEQUENCE_OFFSET);
    result.timestamp_ms = read_u32_le(input + RECORD_TIMESTAMP_OFFSET);
    result.accel_x = read_i16_le(input + RECORD_ACCEL_X_OFFSET);
    result.accel_y = read_i16_le(input + RECORD_ACCEL_Y_OFFSET);
    result.accel_z = read_i16_le(input + RECORD_ACCEL_Z_OFFSET);
    result.gyro_x = read_i16_le(input + RECORD_GYRO_X_OFFSET);
    result.gyro_y = read_i16_le(input + RECORD_GYRO_Y_OFFSET);
    result.gyro_z = read_i16_le(input + RECORD_GYRO_Z_OFFSET);
    result.crc32 = stored_crc;
    *record = result;
    return FLASH_MANAGER_OK;
}

FlashManagerStatus_t FlashManager_PrepareSector(uint32_t sector_index)
{
    uint32_t address;
    FlashManagerStatus_t status;
    if (!initialized) { return FLASH_MANAGER_NOT_INITIALIZED; }
    status = FlashManager_GetSectorAddress(sector_index, &address);
    if (status != FLASH_MANAGER_OK) { return status; }
    status = map_flash_status(W25Q64_SectorErase(address));
    if (status == FLASH_MANAGER_OK)
    {
        cached_recovery_sector = FLASH_LOG_SECTOR_COUNT;
    }
    return status;
}

FlashManagerStatus_t FlashManager_WriteRecord(uint32_t sector_index,
                                              uint32_t slot_index,
                                              const LogRecord_t *record)
{
    uint8_t encoded[FLASH_LOG_RECORD_SIZE];
    uint8_t existing[FLASH_LOG_RECORD_SIZE];
    uint32_t address;
    FlashManagerStatus_t status;
    if (!initialized) { return FLASH_MANAGER_NOT_INITIALIZED; }
    status = FlashManager_GetRecordAddress(sector_index, slot_index, &address);
    if (status != FLASH_MANAGER_OK) { return status; }
    status = FlashManager_SerializeRecord(record, encoded);
    if (status != FLASH_MANAGER_OK) { return status; }
    status = map_flash_status(W25Q64_Read(address, existing, sizeof(existing)));
    if (status != FLASH_MANAGER_OK) { return status; }
    if (!is_erased(existing, sizeof(existing)))
    {
        return FLASH_MANAGER_NOT_ERASED;
    }
    status = map_flash_status(W25Q64_Write(address, encoded, sizeof(encoded)));
    if (status == FLASH_MANAGER_OK)
    {
        cached_recovery_sector = FLASH_LOG_SECTOR_COUNT;
    }
    return status;
}

FlashManagerStatus_t FlashManager_ReadRecord(uint32_t sector_index,
                                             uint32_t slot_index,
                                             LogRecord_t *record)
{
    uint8_t encoded[FLASH_LOG_RECORD_SIZE];
    uint32_t address;
    FlashManagerStatus_t status;
    if (!initialized) { return FLASH_MANAGER_NOT_INITIALIZED; }
    if (record == NULL) { return FLASH_MANAGER_INVALID_PARAM; }
    status = FlashManager_GetRecordAddress(sector_index, slot_index, &address);
    if (status != FLASH_MANAGER_OK) { return status; }
    status = map_flash_status(W25Q64_Read(address, encoded, sizeof(encoded)));
    if (status != FLASH_MANAGER_OK) { return status; }
    return FlashManager_DeserializeRecord(encoded, record);
}

FlashManagerStatus_t FlashManager_SerializeMetadata(
    const FlashMetadata_t *metadata, uint8_t output[FLASH_METADATA_SIZE])
{
    FlashManagerState_t state;
    uint32_t crc;
    if ((metadata == NULL) || (output == NULL))
    {
        return FLASH_MANAGER_INVALID_PARAM;
    }
    state.write_sector = metadata->write_sector;
    state.write_slot = metadata->write_offset / FLASH_LOG_RECORD_SIZE;
    state.oldest_sector = metadata->oldest_sector;
    state.oldest_slot = 0U;
    state.record_count = metadata->record_count;
    state.next_sequence = metadata->next_sequence;
    if ((metadata->magic != FLASH_METADATA_MAGIC) ||
        (metadata->version != FLASH_METADATA_VERSION) ||
        ((metadata->write_offset % FLASH_LOG_RECORD_SIZE) != 0U) ||
        !state_is_valid(&state))
    {
        return FLASH_MANAGER_INVALID_PARAM;
    }
    write_u32_le(output + METADATA_MAGIC_OFFSET, metadata->magic);
    write_u32_le(output + METADATA_VERSION_OFFSET, metadata->version);
    write_u32_le(output + METADATA_GENERATION_OFFSET, metadata->generation);
    write_u32_le(output + METADATA_WRITE_SECTOR_OFFSET,
                 metadata->write_sector);
    write_u32_le(output + METADATA_WRITE_OFFSET_OFFSET,
                 metadata->write_offset);
    write_u32_le(output + METADATA_OLDEST_SECTOR_OFFSET,
                 metadata->oldest_sector);
    write_u32_le(output + METADATA_RECORD_COUNT_OFFSET,
                 metadata->record_count);
    write_u32_le(output + METADATA_NEXT_SEQUENCE_OFFSET,
                 metadata->next_sequence);
    crc = CRC32_Calculate(output, FLASH_METADATA_CRC_OFFSET);
    write_u32_le(output + FLASH_METADATA_CRC_OFFSET, crc);
    return FLASH_MANAGER_OK;
}

FlashManagerStatus_t FlashManager_DeserializeMetadata(
    const uint8_t input[FLASH_METADATA_SIZE], FlashMetadata_t *metadata)
{
    FlashMetadata_t result;
    uint32_t calculated_crc;
    if ((input == NULL) || (metadata == NULL))
    {
        return FLASH_MANAGER_INVALID_PARAM;
    }
    if (is_erased(input, FLASH_METADATA_SIZE))
    {
        return FLASH_MANAGER_NO_METADATA;
    }
    result.magic = read_u32_le(input + METADATA_MAGIC_OFFSET);
    result.version = read_u32_le(input + METADATA_VERSION_OFFSET);
    result.generation = read_u32_le(input + METADATA_GENERATION_OFFSET);
    result.write_sector = read_u32_le(input + METADATA_WRITE_SECTOR_OFFSET);
    result.write_offset = read_u32_le(input + METADATA_WRITE_OFFSET_OFFSET);
    result.oldest_sector = read_u32_le(input + METADATA_OLDEST_SECTOR_OFFSET);
    result.record_count = read_u32_le(input + METADATA_RECORD_COUNT_OFFSET);
    result.next_sequence = read_u32_le(input + METADATA_NEXT_SEQUENCE_OFFSET);
    result.crc32 = read_u32_le(input + FLASH_METADATA_CRC_OFFSET);
    calculated_crc = CRC32_Calculate(input, FLASH_METADATA_CRC_OFFSET);
    if (result.crc32 != calculated_crc) { return FLASH_MANAGER_CRC_ERROR; }
    if (!metadata_fields_valid(&result))
    {
        return FLASH_MANAGER_INVALID_RECORD;
    }
    *metadata = result;
    return FLASH_MANAGER_OK;
}

FlashManagerStatus_t FlashManager_ReadMetadata(FlashMetadataCopy_t copy,
                                               FlashMetadata_t *metadata)
{
    uint8_t encoded[FLASH_METADATA_SIZE];
    uint32_t address;
    FlashManagerStatus_t status;
    if (!initialized) { return FLASH_MANAGER_NOT_INITIALIZED; }
    if (metadata == NULL) { return FLASH_MANAGER_INVALID_PARAM; }
    status = metadata_address(copy, &address);
    if (status != FLASH_MANAGER_OK) { return status; }
    status = map_flash_status(W25Q64_Read(address, encoded, sizeof(encoded)));
    if (status != FLASH_MANAGER_OK) { return status; }
    return FlashManager_DeserializeMetadata(encoded, metadata);
}

static uint8_t is_content_status(FlashManagerStatus_t status)
{
    return (uint8_t)((status == FLASH_MANAGER_EMPTY) ||
                     (status == FLASH_MANAGER_INVALID_RECORD) ||
                     (status == FLASH_MANAGER_CRC_ERROR));
}

FlashManagerStatus_t FlashManager_FindRecordBySequence(
    const FlashManagerState_t *state, uint32_t sequence, LogRecord_t *record)
{
    LogRecord_t candidate;
    FlashManagerStatus_t status;
    uint32_t position;
    uint32_t remaining;
    if (!initialized) { return FLASH_MANAGER_NOT_INITIALIZED; }
    if ((record == NULL) || !state_is_valid(state) ||
        (state->record_count == 0U))
    {
        return FLASH_MANAGER_INVALID_PARAM;
    }
    position = state_position(state->oldest_sector, state->oldest_slot);
    remaining = state_physical_span(state);
    cached_recovery_sector = FLASH_LOG_SECTOR_COUNT;
    while (remaining != 0U)
    {
        status = read_position(position, &candidate);
        if ((status == FLASH_MANAGER_OK) &&
            (candidate.sequence == sequence))
        {
            *record = candidate;
            return FLASH_MANAGER_OK;
        }
        if ((status != FLASH_MANAGER_OK) && !is_content_status(status))
        {
            return status;
        }
        position = next_position(position);
        --remaining;
    }
    return FLASH_MANAGER_INVALID_RECORD;
}

FlashManagerStatus_t FlashManager_CountSectorPrefix(uint32_t sector_index,
                                                    uint32_t first_sequence,
                                                    uint32_t *record_count)
{
    LogRecord_t record;
    FlashManagerStatus_t status;
    uint32_t slot;
    uint32_t count = 0U;
    if (!initialized) { return FLASH_MANAGER_NOT_INITIALIZED; }
    if ((record_count == NULL) ||
        (sector_index >= FLASH_LOG_SECTOR_COUNT))
    {
        return FLASH_MANAGER_INVALID_PARAM;
    }
    cached_recovery_sector = FLASH_LOG_SECTOR_COUNT;
    for (slot = 0U; slot < FLASH_RECORDS_PER_SECTOR; ++slot)
    {
        status = read_position(state_position(sector_index, slot), &record);
        if ((status == FLASH_MANAGER_OK) &&
            (record.sequence == first_sequence + count))
        {
            ++count;
        }
        else if ((status != FLASH_MANAGER_OK) && !is_content_status(status))
        {
            return status;
        }
    }
    *record_count = count;
    return FLASH_MANAGER_OK;
}

static FlashManagerStatus_t finalize_recovered_state(
    FlashManagerRecovery_t *recovery)
{
    LogRecord_t latest;
    FlashManagerStatus_t status;
    if (!state_is_valid(&recovery->state))
    {
        return FLASH_MANAGER_RECOVERY_ERROR;
    }
    if (recovery->state.record_count == 0U)
    {
        recovery->last_timestamp_ms = 0U;
        return FLASH_MANAGER_OK;
    }
    status = FlashManager_FindRecordBySequence(
                 &recovery->state, recovery->state.next_sequence - 1U,
                 &latest);
    if (status != FLASH_MANAGER_OK)
    {
        return is_content_status(status) ?
               FLASH_MANAGER_RECOVERY_ERROR : status;
    }
    recovery->last_timestamp_ms = latest.timestamp_ms;
    recovery->has_gaps = state_has_gaps(&recovery->state);
    return FLASH_MANAGER_OK;
}

static void abandon_occupied_tail(FlashManagerRecovery_t *recovery,
                                  uint32_t position)
{
    uint32_t sector;
    uint32_t slot;
    position_to_state(position, &sector, &slot);
    if (slot != 0U)
    {
        ++sector;
        if (sector >= FLASH_LOG_SECTOR_COUNT) { sector = 0U; }
        recovery->state.write_sector = sector;
        recovery->state.write_slot = 0U;
        recovery->has_gaps = 1U;
        if (recovery->state.record_count == 0U)
        {
            recovery->state.oldest_sector = sector;
            recovery->state.oldest_slot = 0U;
        }
    }
    else
    {
        recovery->state.write_sector = sector;
        recovery->state.write_slot = 0U;
    }
}

static FlashManagerStatus_t scan_tail(FlashManagerRecovery_t *recovery)
{
    LogRecord_t record;
    FlashManagerStatus_t status;
    uint32_t position = state_position(recovery->state.write_sector,
                                       recovery->state.write_slot);
    uint32_t scans = 0U;
    uint32_t old_sequence;

    cached_recovery_sector = FLASH_LOG_SECTOR_COUNT;
    recovery->used_log_scan = 1U;
    if ((recovery->state.record_count == FLASH_LOG_RECORD_CAPACITY) &&
        (recovery->state.write_slot == 0U) &&
        (recovery->state.write_sector == recovery->state.oldest_sector))
    {
        status = read_position(position, &record);
        old_sequence = recovery->state.next_sequence -
                       FLASH_LOG_RECORD_CAPACITY;
        if ((status == FLASH_MANAGER_OK) && (record.sequence == old_sequence))
        {
            return finalize_recovered_state(recovery);
        }
        if ((status != FLASH_MANAGER_OK) && !is_content_status(status))
        {
            return status;
        }
        recovery->state.record_count -= FLASH_RECORDS_PER_SECTOR;
        recovery->state.oldest_sector = recovery->state.write_sector + 1U;
        if (recovery->state.oldest_sector >= FLASH_LOG_SECTOR_COUNT)
        {
            recovery->state.oldest_sector = 0U;
        }
    }

    while ((scans < FLASH_LOG_RECORD_CAPACITY) &&
           (recovery->state.record_count < FLASH_LOG_RECORD_CAPACITY))
    {
        status = read_position(position, &record);
        if (status == FLASH_MANAGER_OK)
        {
            if (record.sequence != recovery->state.next_sequence)
            {
                abandon_occupied_tail(recovery, position);
                return finalize_recovered_state(recovery);
            }
            ++recovery->state.next_sequence;
            ++recovery->state.record_count;
            position = next_position(position);
            ++scans;
            continue;
        }
        if (status == FLASH_MANAGER_EMPTY) { break; }
        if (is_content_status(status))
        {
            abandon_occupied_tail(recovery, position);
            return finalize_recovered_state(recovery);
        }
        return status;
    }
    position_to_state(position, &recovery->state.write_sector,
                      &recovery->state.write_slot);
    return finalize_recovered_state(recovery);
}

static FlashManagerStatus_t scan_entire_log(FlashManagerRecovery_t *recovery)
{
    LogRecord_t record;
    FlashManagerStatus_t status;
    uint32_t position;
    uint32_t newest_position = 0U;
    uint32_t oldest_position;
    uint32_t cursor;
    uint32_t expected;
    uint32_t newest_sequence = 0U;
    uint32_t count;
    uint32_t next_sector;
    uint8_t found = 0U;

    cached_recovery_sector = FLASH_LOG_SECTOR_COUNT;
    recovery->used_log_scan = 1U;
    for (position = 0U; position < FLASH_LOG_RECORD_CAPACITY; ++position)
    {
        status = read_position(position, &record);
        if (status == FLASH_MANAGER_OK)
        {
            if (!found || is_newer_u32(record.sequence, expected))
            {
                found = 1U;
                expected = record.sequence;
                newest_sequence = record.sequence;
                newest_position = position;
                recovery->last_timestamp_ms = record.timestamp_ms;
            }
        }
        else if (!is_content_status(status)) { return status; }
    }
    if (!found)
    {
        recovery->state.write_sector = 0U;
        recovery->state.write_slot = 0U;
        recovery->state.oldest_sector = 0U;
        recovery->state.oldest_slot = 0U;
        recovery->state.record_count = 0U;
        recovery->state.next_sequence = 0U;
        recovery->last_timestamp_ms = 0U;
        recovery->has_gaps = 0U;
        return FLASH_MANAGER_OK;
    }

    count = 1U;
    oldest_position = newest_position;
    cursor = previous_position(newest_position);
    expected = newest_sequence - 1U;
    for (position = 1U; position < FLASH_LOG_RECORD_CAPACITY; ++position)
    {
        status = read_position(cursor, &record);
        if ((status == FLASH_MANAGER_OK) && (record.sequence == expected))
        {
            oldest_position = cursor;
            --expected;
            ++count;
        }
        else if ((status != FLASH_MANAGER_OK) && !is_content_status(status))
        {
            return status;
        }
        cursor = previous_position(cursor);
    }
    recovery->state.next_sequence = newest_sequence + 1U;
    position = next_position(newest_position);
    oldest_position -= oldest_position % FLASH_RECORDS_PER_SECTOR;
    position_to_state(position, &recovery->state.write_sector,
                      &recovery->state.write_slot);
    position_to_state(oldest_position, &recovery->state.oldest_sector,
                      &recovery->state.oldest_slot);
    recovery->state.record_count = count;

    status = read_position(position, &record);
    if ((status != FLASH_MANAGER_EMPTY) &&
        (position % FLASH_RECORDS_PER_SECTOR != 0U))
    {
        if ((status != FLASH_MANAGER_OK) && !is_content_status(status))
        {
            return status;
        }
        next_sector = newest_position / FLASH_RECORDS_PER_SECTOR + 1U;
        if (next_sector >= FLASH_LOG_SECTOR_COUNT) { next_sector = 0U; }
        recovery->state.write_sector = next_sector;
        recovery->state.write_slot = 0U;
        recovery->has_gaps = 1U;
    }
    return finalize_recovered_state(recovery);
}

static void metadata_to_recovery(const FlashMetadata_t *metadata,
                                 FlashMetadataCopy_t copy,
                                 FlashManagerRecovery_t *recovery)
{
    recovery->state.write_sector = metadata->write_sector;
    recovery->state.write_slot = metadata->write_offset /
                                 FLASH_LOG_RECORD_SIZE;
    recovery->state.oldest_sector = metadata->oldest_sector;
    recovery->state.oldest_slot = 0U;
    recovery->state.record_count = metadata->record_count;
    recovery->state.next_sequence = metadata->next_sequence;
    recovery->active_copy = copy;
    recovery->generation = metadata->generation;
    recovery->last_timestamp_ms = 0U;
    recovery->metadata_valid = 1U;
    recovery->used_log_scan = 0U;
    recovery->has_gaps = state_has_gaps(&recovery->state);
}

FlashManagerStatus_t FlashManager_Recover(FlashManagerRecovery_t *recovery)
{
    FlashMetadata_t metadata_a;
    FlashMetadata_t metadata_b;
    FlashManagerRecovery_t result;
    FlashManagerStatus_t status_a;
    FlashManagerStatus_t status_b;
    FlashManagerStatus_t status;
    uint8_t valid_a;
    uint8_t valid_b;
    if (!initialized) { return FLASH_MANAGER_NOT_INITIALIZED; }
    if (recovery == NULL) { return FLASH_MANAGER_INVALID_PARAM; }

    status_a = FlashManager_ReadMetadata(FLASH_METADATA_COPY_A, &metadata_a);
    status_b = FlashManager_ReadMetadata(FLASH_METADATA_COPY_B, &metadata_b);
    valid_a = (uint8_t)(status_a == FLASH_MANAGER_OK);
    valid_b = (uint8_t)(status_b == FLASH_MANAGER_OK);
    if (!valid_a && !is_content_status(status_a) &&
        (status_a != FLASH_MANAGER_NO_METADATA))
    {
        return status_a;
    }
    if (!valid_b && !is_content_status(status_b) &&
        (status_b != FLASH_MANAGER_NO_METADATA))
    {
        return status_b;
    }

    if (valid_a || valid_b)
    {
        if (valid_a && (!valid_b ||
            is_newer_u32(metadata_a.generation, metadata_b.generation)))
        {
            metadata_to_recovery(&metadata_a, FLASH_METADATA_COPY_A, &result);
        }
        else
        {
            metadata_to_recovery(&metadata_b, FLASH_METADATA_COPY_B, &result);
        }
        active_metadata_copy = result.active_copy;
        active_generation = result.generation;
        status = scan_tail(&result);
        if ((status != FLASH_MANAGER_OK) &&
            (status != FLASH_MANAGER_RECOVERY_ERROR))
        {
            return status;
        }
        if (status == FLASH_MANAGER_RECOVERY_ERROR)
        {
            result.metadata_valid = 0U;
            status = scan_entire_log(&result);
            if (status != FLASH_MANAGER_OK) { return status; }
        }
    }
    else
    {
        result.active_copy = FLASH_METADATA_COPY_NONE;
        result.generation = 0U;
        result.metadata_valid = 0U;
        result.used_log_scan = 1U;
        result.last_timestamp_ms = 0U;
        active_metadata_copy = FLASH_METADATA_COPY_NONE;
        active_generation = 0U;
        status = scan_entire_log(&result);
        if (status != FLASH_MANAGER_OK) { return status; }
    }
    metadata_loaded = 1U;
    last_recovery = result;
    *recovery = result;
    return FLASH_MANAGER_OK;
}

FlashManagerStatus_t FlashManager_CommitState(const FlashManagerState_t *state,
                                              uint32_t *generation)
{
    uint8_t encoded[FLASH_METADATA_SIZE];
    uint8_t verify[FLASH_METADATA_SIZE];
    FlashMetadata_t metadata;
    FlashMetadata_t decoded;
    FlashMetadataCopy_t target;
    FlashManagerStatus_t status;
    uint32_t address;
    if (!initialized) { return FLASH_MANAGER_NOT_INITIALIZED; }
    if ((state == NULL) || !state_is_valid(state))
    {
        return FLASH_MANAGER_INVALID_PARAM;
    }
    if (!metadata_loaded) { return FLASH_MANAGER_RECOVERY_ERROR; }
    target = active_metadata_copy == FLASH_METADATA_COPY_A ?
             FLASH_METADATA_COPY_B : FLASH_METADATA_COPY_A;
    metadata.magic = FLASH_METADATA_MAGIC;
    metadata.version = FLASH_METADATA_VERSION;
    metadata.generation = active_generation + 1U;
    metadata.write_sector = state->write_sector;
    metadata.write_offset = state->write_slot * FLASH_LOG_RECORD_SIZE;
    metadata.oldest_sector = state->oldest_sector;
    metadata.record_count = state->record_count;
    metadata.next_sequence = state->next_sequence;
    metadata.crc32 = 0U;
    status = FlashManager_SerializeMetadata(&metadata, encoded);
    if (status != FLASH_MANAGER_OK) { return status; }
    status = metadata_address(target, &address);
    if (status != FLASH_MANAGER_OK) { return status; }
    status = map_flash_status(W25Q64_SectorErase(address));
    if (status != FLASH_MANAGER_OK) { return status; }
    if (PowerFailTest_Hook(POWER_FAIL_POINT_METADATA_ERASED))
    {
        return FLASH_MANAGER_RECOVERY_ERROR;
    }
    if (PowerFailTest_IsArmed(POWER_FAIL_POINT_METADATA_PARTIAL_PROGRAMMED))
    {
        uint32_t partial_size = FLASH_METADATA_SIZE / 2U;
        status = map_flash_status(W25Q64_Write(address, encoded, partial_size));
        if (status != FLASH_MANAGER_OK) { return status; }
        if (PowerFailTest_Hook(POWER_FAIL_POINT_METADATA_PARTIAL_PROGRAMMED))
        {
            return FLASH_MANAGER_RECOVERY_ERROR;
        }
        status = map_flash_status(W25Q64_Write(address + partial_size,
                                               encoded + partial_size,
                                               sizeof(encoded) - partial_size));
    }
    else
    {
        status = map_flash_status(W25Q64_Write(address, encoded,
                                               sizeof(encoded)));
    }
    if (status != FLASH_MANAGER_OK) { return status; }
    if (PowerFailTest_Hook(POWER_FAIL_POINT_METADATA_PROGRAMMED))
    {
        return FLASH_MANAGER_RECOVERY_ERROR;
    }
    status = map_flash_status(W25Q64_Read(address, verify, sizeof(verify)));
    if (status != FLASH_MANAGER_OK) { return status; }
    status = FlashManager_DeserializeMetadata(verify, &decoded);
    if (status != FLASH_MANAGER_OK) { return status; }
    if ((decoded.generation != metadata.generation) ||
        (decoded.write_sector != metadata.write_sector) ||
        (decoded.write_offset != metadata.write_offset) ||
        (decoded.oldest_sector != metadata.oldest_sector) ||
        (decoded.record_count != metadata.record_count) ||
        (decoded.next_sequence != metadata.next_sequence))
    {
        return FLASH_MANAGER_RECOVERY_ERROR;
    }
    active_metadata_copy = target;
    active_generation = metadata.generation;
    last_recovery.state = *state;
    last_recovery.active_copy = target;
    last_recovery.generation = active_generation;
    last_recovery.metadata_valid = 1U;
    last_recovery.has_gaps = state_has_gaps(state);
    if (generation != NULL) { *generation = active_generation; }
    return FLASH_MANAGER_OK;
}

FlashManagerStatus_t FlashManager_GetLastRecovery(
    FlashManagerRecovery_t *recovery)
{
    if (recovery == NULL) { return FLASH_MANAGER_INVALID_PARAM; }
    if (!metadata_loaded) { return FLASH_MANAGER_RECOVERY_ERROR; }
    *recovery = last_recovery;
    return FLASH_MANAGER_OK;
}

#if DATA_LOGGER_TEST_ENABLE
FlashManagerStatus_t FlashManager_TestResetMetadata(void)
{
    FlashManagerStatus_t status;
    if (!initialized) { return FLASH_MANAGER_NOT_INITIALIZED; }
    status = map_flash_status(W25Q64_SectorErase(METADATA_A_ADDR));
    if (status != FLASH_MANAGER_OK) { return status; }
    status = map_flash_status(W25Q64_SectorErase(METADATA_B_ADDR));
    if (status != FLASH_MANAGER_OK) { return status; }
    metadata_loaded = 1U;
    active_metadata_copy = FLASH_METADATA_COPY_NONE;
    active_generation = 0U;
    last_recovery.state.write_sector = 0U;
    last_recovery.state.write_slot = 0U;
    last_recovery.state.oldest_sector = 0U;
    last_recovery.state.oldest_slot = 0U;
    last_recovery.state.record_count = 0U;
    last_recovery.state.next_sequence = 0U;
    last_recovery.active_copy = FLASH_METADATA_COPY_NONE;
    last_recovery.generation = 0U;
    last_recovery.last_timestamp_ms = 0U;
    last_recovery.metadata_valid = 0U;
    last_recovery.used_log_scan = 0U;
    last_recovery.has_gaps = 0U;
    return FLASH_MANAGER_OK;
}
#endif
