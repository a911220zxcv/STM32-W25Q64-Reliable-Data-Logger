#ifndef FLASH_MANAGER_TEST_H
#define FLASH_MANAGER_TEST_H

#include "flash_manager.h"

typedef enum
{
    FLASH_MANAGER_TEST_IDLE = 0,
    FLASH_MANAGER_TEST_CODEC = 1,
    FLASH_MANAGER_TEST_RECORD_IO = 2,
    FLASH_MANAGER_TEST_CRC_DETECTION = 3
} FlashManagerTestCommand_t;

extern volatile uint32_t g_flash_manager_test_command;
extern volatile uint32_t g_flash_manager_test_done;
extern volatile FlashManagerStatus_t g_flash_manager_test_result;
extern volatile uint32_t g_flash_manager_test_value;

FlashManagerStatus_t FlashManagerTest_Codec(void);
FlashManagerStatus_t FlashManagerTest_RecordIO(void);
FlashManagerStatus_t FlashManagerTest_CrcDetection(void);
void FlashManagerTest_Process(void);

#endif
