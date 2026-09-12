#ifndef DATA_LOGGER_TEST_H
#define DATA_LOGGER_TEST_H

#include "data_logger.h"

typedef enum
{
    DATA_LOGGER_TEST_IDLE = 0,
    DATA_LOGGER_TEST_SENSOR = 1,
    DATA_LOGGER_TEST_START = 2,
    DATA_LOGGER_TEST_STOP_VERIFY = 3,
    DATA_LOGGER_TEST_CIRCULAR_WRAP = 4,
    DATA_LOGGER_TEST_WEAR_DISTRIBUTION = 5,
    DATA_LOGGER_TEST_METADATA_REDUNDANCY = 6,
    DATA_LOGGER_TEST_TAIL_RECOVERY = 7,
    DATA_LOGGER_TEST_POWER_CYCLE_PREPARE = 8,
    DATA_LOGGER_TEST_POWER_CYCLE_VERIFY = 9
} DataLoggerTestCommand_t;

extern volatile uint32_t g_data_logger_test_command;
extern volatile uint32_t g_data_logger_test_done;
extern volatile DataLoggerStatus_t g_data_logger_test_result;
extern volatile uint32_t g_data_logger_test_value;
extern volatile uint8_t g_data_logger_test_device_id;
extern volatile int16_t g_data_logger_test_accel_x;
extern volatile int16_t g_data_logger_test_accel_y;
extern volatile int16_t g_data_logger_test_accel_z;
extern volatile int16_t g_data_logger_test_gyro_x;
extern volatile int16_t g_data_logger_test_gyro_y;
extern volatile int16_t g_data_logger_test_gyro_z;
extern volatile LogRecord_t g_data_logger_test_record;
extern volatile uint32_t g_wear_test_query_sector;
extern volatile uint32_t g_wear_test_query_count;
extern volatile uint32_t g_metadata_test_active_copy;
extern volatile uint32_t g_metadata_test_generation;
extern volatile uint32_t g_metadata_test_write_sector;
extern volatile uint32_t g_metadata_test_write_slot;
extern volatile uint32_t g_metadata_test_record_count;
extern volatile uint32_t g_metadata_test_next_sequence;
extern volatile uint8_t g_metadata_test_valid;
extern volatile uint8_t g_metadata_test_used_log_scan;
extern volatile uint8_t g_metadata_test_has_gaps;

DataLoggerStatus_t DataLoggerTest_Sensor(void);
DataLoggerStatus_t DataLoggerTest_Start(void);
DataLoggerStatus_t DataLoggerTest_StopAndVerify(void);
DataLoggerStatus_t DataLoggerTest_CircularWrap(void);
DataLoggerStatus_t DataLoggerTest_WearDistribution(void);
DataLoggerStatus_t DataLoggerTest_MetadataRedundancy(void);
DataLoggerStatus_t DataLoggerTest_TailRecovery(uint8_t recover_now);
DataLoggerStatus_t DataLoggerTest_PowerCycleVerify(void);
void DataLoggerTest_Process(void);

#endif
