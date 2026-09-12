#include "stm32f10x.h"
#include "config.h"
#include "w25q64.h"
#include "flash_manager.h"
#include "system_time.h"
#include "mpu6050.h"
#include "data_logger.h"
#include "wear_leveling.h"
#include "cli.h"
#if FLASH_TEST_ENABLE
#include "flash_test.h"
#endif
#if FLASH_MANAGER_TEST_ENABLE
#include "flash_manager_test.h"
#endif
#if DATA_LOGGER_TEST_ENABLE
#include "data_logger_test.h"
#endif

/* Read-only boot diagnostics, observable in the Keil Watch window. */
volatile W25Q64_Status_t g_flash_init_status = W25Q64_NOT_INITIALIZED;
volatile uint32_t g_flash_jedec_id;
volatile FlashManagerStatus_t g_flash_manager_init_status =
    FLASH_MANAGER_NOT_INITIALIZED;
volatile SystemTimeStatus_t g_system_time_init_status = SYSTEM_TIME_INVALID_CLOCK;
volatile DataLoggerStatus_t g_data_logger_init_status = LOGGER_NOT_INITIALIZED;
volatile DataLoggerStatus_t g_data_logger_process_status = LOGGER_NOT_INITIALIZED;
volatile uint8_t g_mpu6050_device_id;
volatile DataLoggerState_t g_logger_state = DATA_LOGGER_STOPPED;
volatile DataLoggerStatus_t g_logger_last_error = LOGGER_NOT_INITIALIZED;
volatile uint32_t g_logger_record_count;
volatile uint32_t g_logger_next_sequence;
volatile uint32_t g_logger_current_sector;
volatile uint32_t g_logger_current_slot;
volatile uint32_t g_logger_oldest_sector;
volatile uint32_t g_logger_oldest_slot;
volatile uint32_t g_logger_write_address;
volatile uint32_t g_logger_oldest_address;
volatile uint32_t g_logger_last_timestamp_ms;
volatile uint64_t g_wear_total_erases;
volatile uint32_t g_wear_minimum_erase_count;
volatile uint32_t g_wear_maximum_erase_count;
volatile uint32_t g_wear_erase_count_spread;
volatile uint32_t g_wear_sectors_at_minimum;
volatile uint32_t g_wear_sectors_at_maximum;
volatile uint32_t g_wear_next_sector;
volatile FlashManagerStatus_t g_flash_recovery_status =
    FLASH_MANAGER_NOT_INITIALIZED;
volatile uint32_t g_flash_recovery_active_copy;
volatile uint32_t g_flash_recovery_generation;
volatile uint8_t g_flash_recovery_metadata_valid;
volatile uint8_t g_flash_recovery_used_log_scan;
volatile uint8_t g_flash_recovery_has_gaps;
volatile CLIStatus_t g_cli_init_status = CLI_NOT_INITIALIZED;
volatile CLIStatus_t g_cli_process_status = CLI_NOT_INITIALIZED;

static void update_logger_watch(void)
{
    DataLoggerInfo_t info;
    FlashManagerRecovery_t recovery;
    WearLevelingStats_t wear;
    if (DataLogger_GetStatus(&info) != LOGGER_OK) { return; }
    g_logger_state = info.state;
    g_logger_last_error = info.last_error;
    g_logger_record_count = info.record_count;
    g_logger_next_sequence = info.next_sequence;
    g_logger_current_sector = info.current_sector;
    g_logger_current_slot = info.current_slot;
    g_logger_oldest_sector = info.oldest_sector;
    g_logger_oldest_slot = info.oldest_slot;
    g_logger_write_address = info.write_address;
    g_logger_oldest_address = info.oldest_address;
    g_logger_last_timestamp_ms = info.last_timestamp_ms;
    g_flash_recovery_status = FlashManager_GetLastRecovery(&recovery);
    if (g_flash_recovery_status == FLASH_MANAGER_OK)
    {
        g_flash_recovery_active_copy = (uint32_t)recovery.active_copy;
        g_flash_recovery_generation = recovery.generation;
        g_flash_recovery_metadata_valid = recovery.metadata_valid;
        g_flash_recovery_used_log_scan = recovery.used_log_scan;
        g_flash_recovery_has_gaps = recovery.has_gaps;
    }
    if (WearLeveling_GetStats(&wear) != WEAR_LEVELING_OK) { return; }
    g_wear_total_erases = wear.total_erases;
    g_wear_minimum_erase_count = wear.minimum_erase_count;
    g_wear_maximum_erase_count = wear.maximum_erase_count;
    g_wear_erase_count_spread = wear.erase_count_spread;
    g_wear_sectors_at_minimum = wear.sectors_at_minimum;
    g_wear_sectors_at_maximum = wear.sectors_at_maximum;
    g_wear_next_sector = wear.next_sector;
}

int main(void)
{
    uint32_t id;
    uint8_t device_id;
    g_system_time_init_status = SystemTime_Init();
    if (g_system_time_init_status == SYSTEM_TIME_OK)
    {
        g_flash_init_status = W25Q64_Init();
        if (g_flash_init_status == W25Q64_OK)
        {
            g_flash_init_status = W25Q64_ReadJEDECID(&id);
            if (g_flash_init_status == W25Q64_OK)
            {
                g_flash_jedec_id = id;
                g_flash_manager_init_status = FlashManager_Init();
                if (g_flash_manager_init_status == FLASH_MANAGER_OK)
                {
                    g_data_logger_init_status = DataLogger_Init();
                    if ((g_data_logger_init_status == LOGGER_OK) &&
                        (MPU6050_ReadDeviceId(&device_id) == MPU6050_OK))
                    {
                        g_mpu6050_device_id = device_id;
                    }
                    if (g_data_logger_init_status == LOGGER_OK)
                    {
                        g_cli_init_status = CLI_Init();
                    }
                }
            }
        }
    }
    while (1)
    {
        if (g_cli_init_status == CLI_OK)
        {
            g_cli_process_status = CLI_Process();
        }
        if (g_data_logger_init_status == LOGGER_OK)
        {
            g_data_logger_process_status = DataLogger_Process();
            update_logger_watch();
        }
#if FLASH_TEST_ENABLE
        FlashTest_Process();
#endif
#if FLASH_MANAGER_TEST_ENABLE
        FlashManagerTest_Process();
#endif
#if DATA_LOGGER_TEST_ENABLE
        DataLoggerTest_Process();
#endif
    }
}
