#include "config.h"
#include "data_logger_test.h"
#include "mpu6050.h"
#include "wear_leveling.h"
#include "w25q64.h"
#include <stddef.h>

#define WEAR_TEST_COMPLETE_ROUNDS 3UL
#define WEAR_TEST_EXTRA_SECTORS   17UL

#if DATA_LOGGER_TEST_ENABLE
volatile uint32_t g_data_logger_test_command = DATA_LOGGER_TEST_IDLE;
volatile uint32_t g_data_logger_test_done;
volatile DataLoggerStatus_t g_data_logger_test_result = LOGGER_NOT_INITIALIZED;
volatile uint32_t g_data_logger_test_value;
volatile uint8_t g_data_logger_test_device_id;
volatile int16_t g_data_logger_test_accel_x;
volatile int16_t g_data_logger_test_accel_y;
volatile int16_t g_data_logger_test_accel_z;
volatile int16_t g_data_logger_test_gyro_x;
volatile int16_t g_data_logger_test_gyro_y;
volatile int16_t g_data_logger_test_gyro_z;
volatile LogRecord_t g_data_logger_test_record;
volatile uint32_t g_wear_test_query_sector;
volatile uint32_t g_wear_test_query_count;
volatile uint32_t g_metadata_test_active_copy;
volatile uint32_t g_metadata_test_generation;
volatile uint32_t g_metadata_test_write_sector;
volatile uint32_t g_metadata_test_write_slot;
volatile uint32_t g_metadata_test_record_count;
volatile uint32_t g_metadata_test_next_sequence;
volatile uint8_t g_metadata_test_valid;
volatile uint8_t g_metadata_test_used_log_scan;
volatile uint8_t g_metadata_test_has_gaps;

static DataLoggerStatus_t map_test_flash_status(FlashManagerStatus_t status)
{
    if (status == FLASH_MANAGER_OK) { return LOGGER_OK; }
    if (status == FLASH_MANAGER_TIMEOUT) { return LOGGER_TIMEOUT; }
    if (status == FLASH_MANAGER_INVALID_PARAM) { return LOGGER_INVALID_PARAM; }
    if (status == FLASH_MANAGER_CRC_ERROR) { return LOGGER_CRC_ERROR; }
    return LOGGER_FLASH_ERROR;
}

static void publish_recovery(const FlashManagerRecovery_t *recovery)
{
    g_metadata_test_active_copy = (uint32_t)recovery->active_copy;
    g_metadata_test_generation = recovery->generation;
    g_metadata_test_write_sector = recovery->state.write_sector;
    g_metadata_test_write_slot = recovery->state.write_slot;
    g_metadata_test_record_count = recovery->state.record_count;
    g_metadata_test_next_sequence = recovery->state.next_sequence;
    g_metadata_test_valid = recovery->metadata_valid;
    g_metadata_test_used_log_scan = recovery->used_log_scan;
    g_metadata_test_has_gaps = recovery->has_gaps;
}

static DataLoggerStatus_t corrupt_flash_byte(uint32_t address)
{
    uint8_t value;
    uint8_t bit;
    uint32_t offset;
    W25Q64_Status_t status;
    for (offset = 0U; offset < 4U; ++offset)
    {
        status = W25Q64_Read(address + offset, &value, 1U);
        if (status != W25Q64_OK) { return LOGGER_FLASH_ERROR; }
        for (bit = 0U; bit < 8U; ++bit)
        {
            if ((value & (uint8_t)(1U << bit)) != 0U)
            {
                value &= (uint8_t)~(uint8_t)(1U << bit);
                status = W25Q64_PageProgram(address + offset, &value, 1U);
                return status == W25Q64_OK ? LOGGER_OK : LOGGER_FLASH_ERROR;
            }
        }
    }
    return LOGGER_ERROR;
}

static void make_recovery_record(LogRecord_t *record, uint32_t sequence)
{
    record->magic = FLASH_LOG_RECORD_MAGIC;
    record->sequence = sequence;
    record->timestamp_ms = 1000U + sequence;
    record->accel_x = (int16_t)(0x1234 + sequence);
    record->accel_y = 2;
    record->accel_z = 3;
    record->gyro_x = 4;
    record->gyro_y = 5;
    record->gyro_z = 6;
    record->crc32 = 0U;
}

static DataLoggerStatus_t prepare_recovery_fixture(void)
{
    FlashManagerState_t state;
    LogRecord_t record;
    FlashManagerStatus_t status;
    uint32_t sequence;
    uint32_t address;
    DataLoggerStatus_t logger_status = DataLogger_Stop();
    if (logger_status != LOGGER_OK) { return logger_status; }
    status = FlashManager_PrepareSector(0U);
    if (status != FLASH_MANAGER_OK) { return map_test_flash_status(status); }
    status = FlashManager_TestResetMetadata();
    if (status != FLASH_MANAGER_OK) { return map_test_flash_status(status); }
    state.write_sector = 0U;
    state.write_slot = 0U;
    state.oldest_sector = 0U;
    state.oldest_slot = 0U;
    state.record_count = 0U;
    state.next_sequence = 0U;
    status = FlashManager_CommitState(&state, NULL);
    if (status != FLASH_MANAGER_OK) { return map_test_flash_status(status); }
    for (sequence = 0U; sequence < 4U; ++sequence)
    {
        make_recovery_record(&record, sequence);
        status = FlashManager_WriteRecord(0U, sequence, &record);
        if (status != FLASH_MANAGER_OK) { return map_test_flash_status(status); }
    }
    status = FlashManager_GetRecordAddress(0U, 3U, &address);
    if (status != FLASH_MANAGER_OK) { return map_test_flash_status(status); }
    return corrupt_flash_byte(address + 12U);
}

DataLoggerStatus_t DataLoggerTest_Sensor(void)
{
    MPU6050_Sample_t sample;
    uint8_t device_id;
    MPU6050_Status_t sensor_status;
    sensor_status = MPU6050_ReadDeviceId(&device_id);
    if (sensor_status != MPU6050_OK) { return LOGGER_SENSOR_ERROR; }
    g_data_logger_test_device_id = device_id;
    if (device_id != MPU6050_EXPECTED_WHO_AM_I)
    {
        return LOGGER_SENSOR_ERROR;
    }
    sensor_status = MPU6050_ReadSample(&sample);
    if (sensor_status != MPU6050_OK) { return LOGGER_SENSOR_ERROR; }
    g_data_logger_test_accel_x = sample.accel_x;
    g_data_logger_test_accel_y = sample.accel_y;
    g_data_logger_test_accel_z = sample.accel_z;
    g_data_logger_test_gyro_x = sample.gyro_x;
    g_data_logger_test_gyro_y = sample.gyro_y;
    g_data_logger_test_gyro_z = sample.gyro_z;
    return LOGGER_OK;
}

DataLoggerStatus_t DataLoggerTest_Start(void)
{
    DataLoggerStatus_t status = DataLogger_Clear();
    if (status != LOGGER_OK) { return status; }
    return DataLogger_Start();
}

DataLoggerStatus_t DataLoggerTest_StopAndVerify(void)
{
    DataLoggerInfo_t info;
    LogRecord_t record;
    DataLoggerStatus_t status = DataLogger_Stop();
    if (status != LOGGER_OK) { return status; }
    status = DataLogger_GetStatus(&info);
    if (status != LOGGER_OK) { return status; }
    if (info.record_count == 0U) { return LOGGER_NOT_READY; }
    status = DataLogger_ReadRecord(info.record_count - 1U, &record);
    if (status != LOGGER_OK) { return status; }
    if ((record.magic != FLASH_LOG_RECORD_MAGIC) ||
        (record.sequence != info.next_sequence - 1U) ||
        (record.timestamp_ms != info.last_timestamp_ms))
    {
        return LOGGER_ERROR;
    }
    g_data_logger_test_record = record;
    g_data_logger_test_value = info.record_count;
    return LOGGER_OK;
}

DataLoggerStatus_t DataLoggerTest_CircularWrap(void)
{
    DataLoggerInfo_t info;
    LogRecord_t record;
    DataLoggerStatus_t status = DataLogger_TestCircularWrap();
    if (status != LOGGER_OK) { return status; }
    status = DataLogger_GetStatus(&info);
    if (status != LOGGER_OK) { return status; }
    status = DataLogger_ReadRecord(info.record_count - 1U, &record);
    if (status != LOGGER_OK) { return status; }
    g_data_logger_test_record = record;
    g_data_logger_test_value = info.record_count;
    return LOGGER_OK;
}

DataLoggerStatus_t DataLoggerTest_WearDistribution(void)
{
    WearLevelingStats_t stats;
    WearLevelingStatus_t wear_status;
    uint32_t count;
    wear_status = WearLeveling_TestSimulate(WEAR_TEST_COMPLETE_ROUNDS,
                                            WEAR_TEST_EXTRA_SECTORS);
    if (wear_status != WEAR_LEVELING_OK)
    {
        return LOGGER_WEAR_LEVELING_ERROR;
    }
    wear_status = WearLeveling_GetStats(&stats);
    if (wear_status != WEAR_LEVELING_OK)
    {
        return LOGGER_WEAR_LEVELING_ERROR;
    }
    if ((stats.total_erases != 6155U) ||
        (stats.minimum_erase_count != 3U) ||
        (stats.maximum_erase_count != 4U) ||
        (stats.erase_count_spread != WEAR_LEVELING_MAX_IDEAL_SPREAD) ||
        (stats.sectors_at_maximum != WEAR_TEST_EXTRA_SECTORS) ||
        (stats.sectors_at_minimum != FLASH_LOG_SECTOR_COUNT -
                                     WEAR_TEST_EXTRA_SECTORS) ||
        (stats.next_sector != WEAR_TEST_EXTRA_SECTORS))
    {
        return LOGGER_WEAR_LEVELING_ERROR;
    }
    wear_status = WearLeveling_ValidateDistribution(
                      WEAR_LEVELING_MAX_IDEAL_SPREAD);
    if (wear_status != WEAR_LEVELING_OK)
    {
        return LOGGER_WEAR_LEVELING_ERROR;
    }
    wear_status = WearLeveling_GetSectorEraseCount(
                      g_wear_test_query_sector, &count);
    if (wear_status == WEAR_LEVELING_OK) { g_wear_test_query_count = count; }
    g_data_logger_test_value = stats.erase_count_spread;
    return LOGGER_OK;
}

DataLoggerStatus_t DataLoggerTest_MetadataRedundancy(void)
{
    FlashManagerState_t state;
    FlashManagerRecovery_t recovery;
    FlashManagerStatus_t status;
    DataLoggerStatus_t result = DataLogger_Stop();
    if (result != LOGGER_OK) { return result; }
    result = map_test_flash_status(FlashManager_PrepareSector(0U));
    if (result != LOGGER_OK) { return result; }
    result = map_test_flash_status(FlashManager_TestResetMetadata());
    if (result != LOGGER_OK) { return result; }
    state.write_sector = 0U;
    state.write_slot = 0U;
    state.oldest_sector = 0U;
    state.oldest_slot = 0U;
    state.record_count = 0U;
    state.next_sequence = 0U;
    status = FlashManager_CommitState(&state, NULL);
    if (status != FLASH_MANAGER_OK) { return map_test_flash_status(status); }
    status = FlashManager_CommitState(&state, NULL);
    if (status != FLASH_MANAGER_OK) { return map_test_flash_status(status); }
    result = corrupt_flash_byte(METADATA_B_ADDR + FLASH_METADATA_CRC_OFFSET);
    if (result != LOGGER_OK) { return result; }
    status = FlashManager_Init();
    if (status != FLASH_MANAGER_OK) { return map_test_flash_status(status); }
    status = FlashManager_Recover(&recovery);
    if (status != FLASH_MANAGER_OK) { return map_test_flash_status(status); }
    publish_recovery(&recovery);
    if ((recovery.active_copy != FLASH_METADATA_COPY_A) ||
        (recovery.generation != 1U) || !recovery.metadata_valid ||
        (recovery.state.record_count != 0U))
    {
        return LOGGER_ERROR;
    }
    return LOGGER_OK;
}

DataLoggerStatus_t DataLoggerTest_TailRecovery(uint8_t recover_now)
{
    FlashManagerRecovery_t recovery;
    FlashManagerStatus_t status;
    DataLoggerStatus_t result = prepare_recovery_fixture();
    if (result != LOGGER_OK) { return result; }
    if (!recover_now) { return LOGGER_OK; }
    status = FlashManager_Init();
    if (status != FLASH_MANAGER_OK) { return map_test_flash_status(status); }
    status = FlashManager_Recover(&recovery);
    if (status != FLASH_MANAGER_OK) { return map_test_flash_status(status); }
    publish_recovery(&recovery);
    if ((recovery.active_copy != FLASH_METADATA_COPY_A) ||
        (recovery.generation != 1U) || !recovery.metadata_valid ||
        !recovery.used_log_scan || !recovery.has_gaps ||
        (recovery.state.write_sector != 1U) ||
        (recovery.state.write_slot != 0U) ||
        (recovery.state.oldest_sector != 0U) ||
        (recovery.state.record_count != 3U) ||
        (recovery.state.next_sequence != 3U) ||
        (recovery.last_timestamp_ms != 1002U))
    {
        return LOGGER_ERROR;
    }
    return LOGGER_OK;
}

DataLoggerStatus_t DataLoggerTest_PowerCycleVerify(void)
{
    FlashManagerRecovery_t recovery;
    FlashManagerStatus_t status = FlashManager_Init();
    if (status != FLASH_MANAGER_OK) { return map_test_flash_status(status); }
    status = FlashManager_Recover(&recovery);
    if (status != FLASH_MANAGER_OK) { return map_test_flash_status(status); }
    publish_recovery(&recovery);
    if ((recovery.active_copy != FLASH_METADATA_COPY_A) ||
        (recovery.generation != 1U) || !recovery.metadata_valid ||
        !recovery.used_log_scan || !recovery.has_gaps ||
        (recovery.state.write_sector != 1U) ||
        (recovery.state.write_slot != 0U) ||
        (recovery.state.record_count != 3U) ||
        (recovery.state.next_sequence != 3U) ||
        (recovery.last_timestamp_ms != 1002U))
    {
        return LOGGER_ERROR;
    }
    return LOGGER_OK;
}

void DataLoggerTest_Process(void)
{
    uint32_t command = g_data_logger_test_command;
    DataLoggerStatus_t status;
    uint32_t count;
    if (WearLeveling_GetSectorEraseCount(g_wear_test_query_sector, &count) ==
        WEAR_LEVELING_OK)
    {
        g_wear_test_query_count = count;
    }
    if (command == DATA_LOGGER_TEST_IDLE) { return; }
    g_data_logger_test_command = DATA_LOGGER_TEST_IDLE;
    g_data_logger_test_done = 0U;
    switch (command)
    {
        case DATA_LOGGER_TEST_SENSOR:
            status = DataLoggerTest_Sensor();
            break;
        case DATA_LOGGER_TEST_START:
            status = DataLoggerTest_Start();
            break;
        case DATA_LOGGER_TEST_STOP_VERIFY:
            status = DataLoggerTest_StopAndVerify();
            break;
        case DATA_LOGGER_TEST_CIRCULAR_WRAP:
            status = DataLoggerTest_CircularWrap();
            break;
        case DATA_LOGGER_TEST_WEAR_DISTRIBUTION:
            status = DataLoggerTest_WearDistribution();
            break;
        case DATA_LOGGER_TEST_METADATA_REDUNDANCY:
            status = DataLoggerTest_MetadataRedundancy();
            break;
        case DATA_LOGGER_TEST_TAIL_RECOVERY:
            status = DataLoggerTest_TailRecovery(1U);
            break;
        case DATA_LOGGER_TEST_POWER_CYCLE_PREPARE:
            status = DataLoggerTest_TailRecovery(0U);
            break;
        case DATA_LOGGER_TEST_POWER_CYCLE_VERIFY:
            status = DataLoggerTest_PowerCycleVerify();
            break;
        default:
            status = LOGGER_INVALID_PARAM;
            break;
    }
    g_data_logger_test_result = status;
    g_data_logger_test_done = command;
}
#endif
