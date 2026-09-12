#include "config.h"
#include "data_logger.h"
#include "mpu6050.h"
#include "system_time.h"
#include "wear_leveling.h"
#include "power_fail_test.h"
#include <stddef.h>

static uint8_t initialized;
static uint8_t session_ready;
static uint8_t sector_prepared;
static uint8_t logger_has_gaps;
static DataLoggerInfo_t logger;
static uint32_t next_sample_ms;
#if DATA_LOGGER_TEST_ENABLE
static uint8_t suppress_metadata_commit;
#endif

static DataLoggerStatus_t map_flash_status(FlashManagerStatus_t status)
{
    switch (status)
    {
        case FLASH_MANAGER_OK: return LOGGER_OK;
        case FLASH_MANAGER_TIMEOUT: return LOGGER_TIMEOUT;
        case FLASH_MANAGER_INVALID_PARAM: return LOGGER_INVALID_PARAM;
        case FLASH_MANAGER_CRC_ERROR: return LOGGER_CRC_ERROR;
        default: return LOGGER_FLASH_ERROR;
    }
}

static DataLoggerStatus_t map_sensor_status(MPU6050_Status_t status)
{
    switch (status)
    {
        case MPU6050_OK: return LOGGER_OK;
        case MPU6050_TIMEOUT: return LOGGER_TIMEOUT;
        case MPU6050_INVALID_PARAM: return LOGGER_INVALID_PARAM;
        default: return LOGGER_SENSOR_ERROR;
    }
}

static DataLoggerStatus_t map_wear_status(WearLevelingStatus_t status)
{
    switch (status)
    {
        case WEAR_LEVELING_OK: return LOGGER_OK;
        case WEAR_LEVELING_TIMEOUT: return LOGGER_TIMEOUT;
        case WEAR_LEVELING_INVALID_PARAM: return LOGGER_INVALID_PARAM;
        case WEAR_LEVELING_FLASH_ERROR: return LOGGER_FLASH_ERROR;
        default: return LOGGER_WEAR_LEVELING_ERROR;
    }
}

static void stop_with_error(DataLoggerStatus_t status)
{
    logger.state = DATA_LOGGER_STOPPED;
    logger.last_error = status;
}

static void get_flash_state(FlashManagerState_t *state)
{
    state->write_sector = logger.current_sector;
    state->write_slot = logger.current_slot;
    state->oldest_sector = logger.oldest_sector;
    state->oldest_slot = logger.oldest_slot;
    state->record_count = logger.record_count;
    state->next_sequence = logger.next_sequence;
}

static DataLoggerStatus_t commit_state(void)
{
    FlashManagerState_t state;
    get_flash_state(&state);
    return map_flash_status(FlashManager_CommitState(&state, NULL));
}

static DataLoggerStatus_t prepare_write_sector(void)
{
    DataLoggerStatus_t status;
    uint8_t replacing_oldest;
    uint32_t selected_sector;
    uint32_t next_sector;
    uint32_t removed_records = 0U;
    FlashManagerState_t reclaim_state;
    if (sector_prepared) { return LOGGER_OK; }
    replacing_oldest = (uint8_t)((logger.record_count != 0U) &&
                                 (logger.current_slot == 0U) &&
                                 (logger.current_sector ==
                                  logger.oldest_sector));
    if (replacing_oldest &&
        ((logger.current_sector != logger.oldest_sector) ||
         (logger.oldest_slot != 0U)))
    {
        return LOGGER_ERROR;
    }
    status = map_wear_status(WearLeveling_PeekNextSector(&selected_sector));
    if (status != LOGGER_OK) { return status; }
    if (selected_sector != logger.current_sector)
    {
        return LOGGER_WEAR_LEVELING_ERROR;
    }
    if (replacing_oldest)
    {
        status = map_wear_status(WearLeveling_GetNextSector(
                                     logger.current_sector, &next_sector));
        if (status != LOGGER_OK) { return status; }
#if DATA_LOGGER_TEST_ENABLE
        if (suppress_metadata_commit)
        {
            removed_records = FLASH_RECORDS_PER_SECTOR;
        }
        else
#endif
        {
            status = map_flash_status(FlashManager_CountSectorPrefix(
                         logger.current_sector,
                         logger.next_sequence - logger.record_count,
                         &removed_records));
            if (status != LOGGER_OK) { return status; }
        }
        if (removed_records > logger.record_count) { return LOGGER_ERROR; }
#if DATA_LOGGER_TEST_ENABLE
        if (!suppress_metadata_commit)
#endif
        {
            get_flash_state(&reclaim_state);
            reclaim_state.record_count -= removed_records;
            reclaim_state.oldest_sector =
                reclaim_state.record_count == 0U ?
                logger.current_sector : next_sector;
            status = map_flash_status(FlashManager_CommitState(
                                          &reclaim_state, NULL));
            if (status != LOGGER_OK) { return status; }
            logger.record_count = reclaim_state.record_count;
            logger.oldest_sector = reclaim_state.oldest_sector;
            logger.oldest_slot = 0U;
        }
    }
    status = map_wear_status(WearLeveling_PrepareNextSector(&selected_sector));
    if (status != LOGGER_OK) { return status; }
    if (selected_sector != logger.current_sector)
    {
        return LOGGER_WEAR_LEVELING_ERROR;
    }
    sector_prepared = 1U;
    if (replacing_oldest)
    {
#if DATA_LOGGER_TEST_ENABLE
        if (suppress_metadata_commit)
        {
            logger.record_count -= removed_records;
            logger.oldest_sector = logger.record_count == 0U ?
                                   logger.current_sector : next_sector;
            logger.oldest_slot = 0U;
        }
#else
        (void)removed_records;
#endif
    }
    return LOGGER_OK;
}

static DataLoggerStatus_t append_sample(uint32_t timestamp_ms)
{
    MPU6050_Sample_t sample;
    LogRecord_t record;
    DataLoggerStatus_t status;
    uint32_t next_sector;

    status = map_sensor_status(MPU6050_ReadSample(&sample));
    if (status != LOGGER_OK) { return status; }
    status = prepare_write_sector();
    if (status != LOGGER_OK) { return status; }

    record.magic = FLASH_LOG_RECORD_MAGIC;
    record.sequence = logger.next_sequence;
    record.timestamp_ms = timestamp_ms;
    record.accel_x = sample.accel_x;
    record.accel_y = sample.accel_y;
    record.accel_z = sample.accel_z;
    record.gyro_x = sample.gyro_x;
    record.gyro_y = sample.gyro_y;
    record.gyro_z = sample.gyro_z;
    record.crc32 = 0U;
    status = map_flash_status(FlashManager_WriteRecord(logger.current_sector,
                                                        logger.current_slot,
                                                        &record));
    if (status != LOGGER_OK) { return status; }
    if (PowerFailTest_Hook(POWER_FAIL_POINT_RECORD_PROGRAMMED))
    {
        return LOGGER_ERROR;
    }

    ++logger.record_count;
    ++logger.next_sequence;
    logger.last_timestamp_ms = timestamp_ms;
    ++logger.current_slot;
    if (logger.current_slot >= FLASH_RECORDS_PER_SECTOR)
    {
        logger.current_slot = 0U;
        status = map_wear_status(WearLeveling_GetNextSector(
                                     logger.current_sector, &next_sector));
        if (status != LOGGER_OK) { return status; }
        logger.current_sector = next_sector;
        sector_prepared = 0U;
#if DATA_LOGGER_TEST_ENABLE
        if (!suppress_metadata_commit)
#endif
        {
            status = commit_state();
            if (status != LOGGER_OK) { return status; }
        }
    }
    return LOGGER_OK;
}

DataLoggerStatus_t DataLogger_Init(void)
{
    DataLoggerStatus_t result;
    FlashManagerRecovery_t recovery;
    uint32_t wear_sector;
    initialized = 0U;
    session_ready = 0U;
    sector_prepared = 0U;
    logger_has_gaps = 0U;
    logger.state = DATA_LOGGER_STOPPED;
    logger.last_error = LOGGER_NOT_INITIALIZED;
    logger.record_count = 0U;
    logger.next_sequence = 0U;
    logger.current_sector = 0U;
    logger.current_slot = 0U;
    logger.oldest_sector = 0U;
    logger.oldest_slot = 0U;
    logger.write_address = LOG_START_ADDR;
    logger.oldest_address = LOG_START_ADDR;
    logger.last_timestamp_ms = 0U;
    next_sample_ms = 0U;
#if DATA_LOGGER_TEST_ENABLE
    suppress_metadata_commit = 0U;
#endif

    result = map_flash_status(FlashManager_Init());
    if (result != LOGGER_OK)
    {
        logger.last_error = result;
        return result;
    }
    result = map_wear_status(WearLeveling_Init());
    if (result != LOGGER_OK)
    {
        logger.last_error = result;
        return result;
    }
    result = map_flash_status(FlashManager_Recover(&recovery));
    if (result != LOGGER_OK)
    {
        logger.last_error = result;
        return result;
    }
    logger.record_count = recovery.state.record_count;
    logger.next_sequence = recovery.state.next_sequence;
    logger.current_sector = recovery.state.write_sector;
    logger.current_slot = recovery.state.write_slot;
    logger.oldest_sector = recovery.state.oldest_sector;
    logger.oldest_slot = recovery.state.oldest_slot;
    logger.last_timestamp_ms = recovery.last_timestamp_ms;
    logger_has_gaps = recovery.has_gaps;
    session_ready = 1U;
    sector_prepared = (uint8_t)(logger.current_slot != 0U);
    wear_sector = logger.current_sector;
    if (sector_prepared)
    {
        result = map_wear_status(WearLeveling_GetNextSector(
                                     logger.current_sector, &wear_sector));
        if (result != LOGGER_OK)
        {
            logger.last_error = result;
            return result;
        }
    }
    result = map_wear_status(WearLeveling_RestoreNextSector(wear_sector));
    if (result != LOGGER_OK)
    {
        logger.last_error = result;
        return result;
    }
    result = map_sensor_status(MPU6050_Init());
    if (result != LOGGER_OK)
    {
        logger.last_error = result;
        return result;
    }
    initialized = 1U;
    logger.last_error = LOGGER_OK;
    return LOGGER_OK;
}

DataLoggerStatus_t DataLogger_Start(void)
{
    if (!initialized) { return LOGGER_NOT_INITIALIZED; }
    if (logger.state == DATA_LOGGER_RUNNING) { return LOGGER_OK; }
    if (!session_ready) { return LOGGER_NOT_READY; }
    logger.state = DATA_LOGGER_RUNNING;
    logger.last_error = LOGGER_OK;
    next_sample_ms = SystemTime_GetMs() + LOGGER_SAMPLE_PERIOD_MS;
    return LOGGER_OK;
}

DataLoggerStatus_t DataLogger_Stop(void)
{
    if (!initialized) { return LOGGER_NOT_INITIALIZED; }
    logger.state = DATA_LOGGER_STOPPED;
    logger.last_error = LOGGER_OK;
    return LOGGER_OK;
}

DataLoggerStatus_t DataLogger_Process(void)
{
    DataLoggerStatus_t status;
    uint32_t now;

    if (!initialized) { return LOGGER_NOT_INITIALIZED; }
    if (logger.state != DATA_LOGGER_RUNNING) { return logger.last_error; }
    now = SystemTime_GetMs();
    if ((int32_t)(now - next_sample_ms) < 0) { return LOGGER_OK; }
    /* Late main-loop calls intentionally skip catch-up bursts. */
    next_sample_ms = now + LOGGER_SAMPLE_PERIOD_MS;

    status = append_sample(now);
    if (status != LOGGER_OK)
    {
        stop_with_error(status);
        return status;
    }

    logger.last_error = LOGGER_OK;
    return LOGGER_OK;
}

DataLoggerStatus_t DataLogger_GetStatus(DataLoggerInfo_t *info)
{
    DataLoggerInfo_t result;
    FlashManagerStatus_t flash_status;
    if (info == NULL) { return LOGGER_INVALID_PARAM; }
    if (!initialized) { return LOGGER_NOT_INITIALIZED; }
    result = logger;
    flash_status = FlashManager_GetRecordAddress(logger.current_sector,
                                                  logger.current_slot,
                                                  &result.write_address);
    if (flash_status != FLASH_MANAGER_OK) { return map_flash_status(flash_status); }
    flash_status = FlashManager_GetRecordAddress(logger.oldest_sector,
                                                  logger.oldest_slot,
                                                  &result.oldest_address);
    if (flash_status != FLASH_MANAGER_OK) { return map_flash_status(flash_status); }
    *info = result;
    return LOGGER_OK;
}

DataLoggerStatus_t DataLogger_Clear(void)
{
    DataLoggerStatus_t status;
    uint32_t selected_sector;
    if (!initialized) { return LOGGER_NOT_INITIALIZED; }
    logger.state = DATA_LOGGER_STOPPED;
    session_ready = 0U;
    sector_prepared = 0U;
    status = map_wear_status(WearLeveling_PrepareNextSector(&selected_sector));
    if (status != LOGGER_OK)
    {
        logger.last_error = status;
        return status;
    }
    logger.record_count = 0U;
    logger.next_sequence = 0U;
    logger.current_sector = selected_sector;
    logger.current_slot = 0U;
    logger.oldest_sector = selected_sector;
    logger.oldest_slot = 0U;
    logger.write_address = LOG_START_ADDR + selected_sector * FLASH_SECTOR_SIZE;
    logger.oldest_address = logger.write_address;
    logger.last_timestamp_ms = 0U;
    logger_has_gaps = 0U;
    logger.last_error = LOGGER_OK;
    sector_prepared = 1U;
    status = commit_state();
    if (status != LOGGER_OK)
    {
        logger.last_error = status;
        return status;
    }
    /* Clear is rare: update the second copy too, so either A or B alone
     * represents the new empty boundary after a later single-copy failure. */
    status = commit_state();
    if (status != LOGGER_OK)
    {
        logger.last_error = status;
        return status;
    }
    session_ready = 1U;
    return LOGGER_OK;
}

DataLoggerStatus_t DataLogger_ReadRecord(uint32_t index, LogRecord_t *record)
{
    FlashManagerState_t state;
    uint32_t sequence;
    uint32_t position;
    uint32_t sector;
    uint32_t slot;
    if (record == NULL) { return LOGGER_INVALID_PARAM; }
    if (!initialized) { return LOGGER_NOT_INITIALIZED; }
    if (index >= logger.record_count) { return LOGGER_INVALID_PARAM; }
    if (logger_has_gaps)
    {
        get_flash_state(&state);
        sequence = logger.next_sequence - logger.record_count + index;
        return map_flash_status(FlashManager_FindRecordBySequence(
                                    &state, sequence, record));
    }
    position = logger.oldest_sector * FLASH_RECORDS_PER_SECTOR +
               logger.oldest_slot + index;
    if (position >= FLASH_LOG_RECORD_CAPACITY)
    {
        position -= FLASH_LOG_RECORD_CAPACITY;
    }
    sector = position / FLASH_RECORDS_PER_SECTOR;
    slot = position % FLASH_RECORDS_PER_SECTOR;
    return map_flash_status(FlashManager_ReadRecord(sector, slot, record));
}

#if DATA_LOGGER_TEST_ENABLE
DataLoggerStatus_t DataLogger_TestSetupReclaim(void)
{
    LogRecord_t record;
    FlashManagerStatus_t flash_status;
    WearLevelingStatus_t wear_status;
    uint32_t sequence;
    if (!initialized) { return LOGGER_NOT_INITIALIZED; }
    logger.state = DATA_LOGGER_STOPPED;
    flash_status = FlashManager_TestResetMetadata();
    if (flash_status != FLASH_MANAGER_OK)
    {
        return map_flash_status(flash_status);
    }
    flash_status = FlashManager_PrepareSector(0U);
    if (flash_status != FLASH_MANAGER_OK)
    {
        return map_flash_status(flash_status);
    }
    for (sequence = 100U; sequence < 103U; ++sequence)
    {
        record.magic = FLASH_LOG_RECORD_MAGIC;
        record.sequence = sequence;
        record.timestamp_ms = sequence;
        record.accel_x = (int16_t)sequence;
        record.accel_y = 2;
        record.accel_z = 3;
        record.gyro_x = 4;
        record.gyro_y = 5;
        record.gyro_z = 6;
        record.crc32 = 0U;
        flash_status = FlashManager_WriteRecord(0U, sequence - 100U,
                                                 &record);
        if (flash_status != FLASH_MANAGER_OK)
        {
            return map_flash_status(flash_status);
        }
    }
    wear_status = WearLeveling_Init();
    if (wear_status != WEAR_LEVELING_OK)
    {
        return map_wear_status(wear_status);
    }
    wear_status = WearLeveling_RestoreNextSector(0U);
    if (wear_status != WEAR_LEVELING_OK)
    {
        return map_wear_status(wear_status);
    }
    logger.record_count = 3U;
    logger.next_sequence = 103U;
    logger.current_sector = 0U;
    logger.current_slot = 0U;
    logger.oldest_sector = 0U;
    logger.oldest_slot = 0U;
    logger.last_timestamp_ms = 102U;
    logger.last_error = LOGGER_OK;
    logger_has_gaps = 1U;
    session_ready = 1U;
    sector_prepared = 0U;
    return commit_state();
}

DataLoggerStatus_t DataLogger_TestPrepareWriteSector(void)
{
    if (!initialized) { return LOGGER_NOT_INITIALIZED; }
    return prepare_write_sector();
}

DataLoggerStatus_t DataLogger_TestCircularWrap(void)
{
    LogRecord_t marker;
    LogRecord_t newest;
    DataLoggerStatus_t status;
    FlashManagerStatus_t flash_status;
    WearLevelingStatus_t wear_status;
    uint32_t selected_sector;
    uint32_t last_sector = FLASH_LOG_SECTOR_COUNT - 1U;

    if (!initialized) { return LOGGER_NOT_INITIALIZED; }
    logger.state = DATA_LOGGER_STOPPED;
    session_ready = 0U;
    sector_prepared = 0U;
    logger_has_gaps = 0U;
    wear_status = WearLeveling_Init();
    if (wear_status != WEAR_LEVELING_OK) { return map_wear_status(wear_status); }
    wear_status = WearLeveling_PrepareNextSector(&selected_sector);
    if ((wear_status != WEAR_LEVELING_OK) || (selected_sector != 0U))
    {
        return wear_status == WEAR_LEVELING_OK ?
               LOGGER_WEAR_LEVELING_ERROR : map_wear_status(wear_status);
    }
    marker.magic = FLASH_LOG_RECORD_MAGIC;
    marker.sequence = 1U;
    marker.timestamp_ms = 1U;
    marker.accel_x = 1;
    marker.accel_y = 1;
    marker.accel_z = 1;
    marker.gyro_x = 1;
    marker.gyro_y = 1;
    marker.gyro_z = 1;
    marker.crc32 = 0U;
    flash_status = FlashManager_WriteRecord(0U, 1U, &marker);
    if (flash_status != FLASH_MANAGER_OK) { return map_flash_status(flash_status); }
    wear_status = WearLeveling_PrepareNextSector(&selected_sector);
    if ((wear_status != WEAR_LEVELING_OK) || (selected_sector != 1U))
    {
        return wear_status == WEAR_LEVELING_OK ?
               LOGGER_WEAR_LEVELING_ERROR : map_wear_status(wear_status);
    }
    marker.sequence = FLASH_RECORDS_PER_SECTOR;
    flash_status = FlashManager_WriteRecord(1U, 0U, &marker);
    if (flash_status != FLASH_MANAGER_OK) { return map_flash_status(flash_status); }
    wear_status = WearLeveling_TestSetNextSector(last_sector);
    if (wear_status != WEAR_LEVELING_OK) { return map_wear_status(wear_status); }
    wear_status = WearLeveling_PrepareNextSector(&selected_sector);
    if ((wear_status != WEAR_LEVELING_OK) ||
        (selected_sector != last_sector))
    {
        return wear_status == WEAR_LEVELING_OK ?
               LOGGER_WEAR_LEVELING_ERROR : map_wear_status(wear_status);
    }

    logger.record_count = FLASH_LOG_RECORD_CAPACITY - 1U;
    logger.next_sequence = FLASH_LOG_RECORD_CAPACITY - 1U;
    logger.current_sector = last_sector;
    logger.current_slot = FLASH_RECORDS_PER_SECTOR - 1U;
    logger.oldest_sector = 0U;
    logger.oldest_slot = 0U;
    logger.last_timestamp_ms = 0U;
    logger.last_error = LOGGER_OK;
    session_ready = 1U;
    sector_prepared = 1U;

#if DATA_LOGGER_TEST_ENABLE
    suppress_metadata_commit = 1U;
#endif
    status = append_sample(SystemTime_GetMs());
    if (status != LOGGER_OK)
    {
#if DATA_LOGGER_TEST_ENABLE
        suppress_metadata_commit = 0U;
#endif
        return status;
    }
    status = append_sample(SystemTime_GetMs() + 1U);
#if DATA_LOGGER_TEST_ENABLE
    suppress_metadata_commit = 0U;
#endif
    if (status != LOGGER_OK) { return status; }
    if ((logger.current_sector != 0U) || (logger.current_slot != 1U) ||
        (logger.oldest_sector != 1U) || (logger.oldest_slot != 0U) ||
        (logger.record_count != FLASH_LOG_RECORD_CAPACITY -
                                FLASH_RECORDS_PER_SECTOR + 1U) ||
        (logger.next_sequence != FLASH_LOG_RECORD_CAPACITY + 1U))
    {
        return LOGGER_ERROR;
    }
    flash_status = FlashManager_ReadRecord(0U, 1U, &marker);
    if (flash_status != FLASH_MANAGER_EMPTY) { return LOGGER_ERROR; }
    status = DataLogger_ReadRecord(0U, &marker);
    if ((status != LOGGER_OK) ||
        (marker.sequence != FLASH_RECORDS_PER_SECTOR))
    {
        return LOGGER_ERROR;
    }
    status = DataLogger_ReadRecord(logger.record_count - 1U, &newest);
    if (status != LOGGER_OK) { return status; }
    if ((newest.sequence != FLASH_LOG_RECORD_CAPACITY) ||
        (newest.timestamp_ms != logger.last_timestamp_ms))
    {
        return LOGGER_ERROR;
    }
    return LOGGER_OK;
}
#endif
