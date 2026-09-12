#include "config.h"
#include "flash_manager_test.h"
#include "crc32.h"
#include "w25q64.h"

#if FLASH_MANAGER_TEST_ENABLE
volatile uint32_t g_flash_manager_test_command = FLASH_MANAGER_TEST_IDLE;
volatile uint32_t g_flash_manager_test_done;
volatile FlashManagerStatus_t g_flash_manager_test_result =
    FLASH_MANAGER_NOT_INITIALIZED;
volatile uint32_t g_flash_manager_test_value;

static void make_record(LogRecord_t *record)
{
    record->magic = FLASH_LOG_RECORD_MAGIC;
    record->sequence = 0x12345678UL;
    record->timestamp_ms = 123400UL;
    record->accel_x = (int16_t)0x1234;
    record->accel_y = -2345;
    record->accel_z = 32767;
    record->gyro_x = -32768;
    record->gyro_y = 0;
    record->gyro_z = 9876;
    record->crc32 = 0U;
}

static uint8_t records_equal(const LogRecord_t *left, const LogRecord_t *right)
{
    return (uint8_t)((left->magic == right->magic) &&
                     (left->sequence == right->sequence) &&
                     (left->timestamp_ms == right->timestamp_ms) &&
                     (left->accel_x == right->accel_x) &&
                     (left->accel_y == right->accel_y) &&
                     (left->accel_z == right->accel_z) &&
                     (left->gyro_x == right->gyro_x) &&
                     (left->gyro_y == right->gyro_y) &&
                     (left->gyro_z == right->gyro_z));
}

FlashManagerStatus_t FlashManagerTest_Codec(void)
{
    static const uint8_t check[] = "123456789";
    uint8_t encoded[FLASH_LOG_RECORD_SIZE];
    LogRecord_t input;
    LogRecord_t output;
    FlashManagerStatus_t status;
    if (CRC32_Calculate(check, 9U) != 0xCBF43926UL)
    {
        return FLASH_MANAGER_CRC_ERROR;
    }
    make_record(&input);
    status = FlashManager_SerializeRecord(&input, encoded);
    if (status != FLASH_MANAGER_OK) { return status; }
    status = FlashManager_DeserializeRecord(encoded, &output);
    if (status != FLASH_MANAGER_OK) { return status; }
    if (!records_equal(&input, &output)) { return FLASH_MANAGER_ERROR; }
    g_flash_manager_test_value = output.crc32;
    return FLASH_MANAGER_OK;
}

FlashManagerStatus_t FlashManagerTest_RecordIO(void)
{
    LogRecord_t input;
    LogRecord_t output;
    FlashManagerStatus_t status;
    status = FlashManager_Init();
    if (status != FLASH_MANAGER_OK) { return status; }
    status = FlashManager_PrepareSector(FLASH_MANAGER_TEST_SECTOR_INDEX);
    if (status != FLASH_MANAGER_OK) { return status; }
    make_record(&input);
    /* Slot 9 starts at offset 252, intentionally crossing a page boundary. */
    status = FlashManager_WriteRecord(FLASH_MANAGER_TEST_SECTOR_INDEX, 9U, &input);
    if (status != FLASH_MANAGER_OK) { return status; }
    status = FlashManager_ReadRecord(FLASH_MANAGER_TEST_SECTOR_INDEX, 9U, &output);
    if (status != FLASH_MANAGER_OK) { return status; }
    if (!records_equal(&input, &output)) { return FLASH_MANAGER_ERROR; }
    status = FlashManager_WriteRecord(FLASH_MANAGER_TEST_SECTOR_INDEX, 9U, &input);
    if (status != FLASH_MANAGER_NOT_ERASED) { return FLASH_MANAGER_ERROR; }
    g_flash_manager_test_value = output.crc32;
    return FLASH_MANAGER_OK;
}

FlashManagerStatus_t FlashManagerTest_CrcDetection(void)
{
    LogRecord_t input;
    LogRecord_t output;
    FlashManagerStatus_t status;
    uint32_t address;
    uint8_t corrupted = 0x30U;
    status = FlashManager_Init();
    if (status != FLASH_MANAGER_OK) { return status; }
    status = FlashManager_PrepareSector(FLASH_MANAGER_TEST_SECTOR_INDEX);
    if (status != FLASH_MANAGER_OK) { return status; }
    make_record(&input);
    status = FlashManager_WriteRecord(FLASH_MANAGER_TEST_SECTOR_INDEX, 9U, &input);
    if (status != FLASH_MANAGER_OK) { return status; }
    status = FlashManager_GetRecordAddress(FLASH_MANAGER_TEST_SECTOR_INDEX,
                                           9U, &address);
    if (status != FLASH_MANAGER_OK) { return status; }
    /* accel_x low byte changes 0x34 -> 0x30 using legal NOR 1 -> 0 bits. */
    if (W25Q64_PageProgram(address + 12U, &corrupted, 1U) != W25Q64_OK)
    {
        return FLASH_MANAGER_FLASH_ERROR;
    }
    status = FlashManager_ReadRecord(FLASH_MANAGER_TEST_SECTOR_INDEX, 9U, &output);
    return status == FLASH_MANAGER_CRC_ERROR ?
           FLASH_MANAGER_OK : FLASH_MANAGER_ERROR;
}

void FlashManagerTest_Process(void)
{
    uint32_t command = g_flash_manager_test_command;
    FlashManagerStatus_t status;
    if (command == FLASH_MANAGER_TEST_IDLE) { return; }
    g_flash_manager_test_command = FLASH_MANAGER_TEST_IDLE;
    g_flash_manager_test_done = 0U;
    switch (command)
    {
        case FLASH_MANAGER_TEST_CODEC:
            status = FlashManagerTest_Codec();
            break;
        case FLASH_MANAGER_TEST_RECORD_IO:
            status = FlashManagerTest_RecordIO();
            break;
        case FLASH_MANAGER_TEST_CRC_DETECTION:
            status = FlashManagerTest_CrcDetection();
            break;
        default:
            status = FLASH_MANAGER_INVALID_PARAM;
            break;
    }
    g_flash_manager_test_result = status;
    g_flash_manager_test_done = command;
}
#endif
