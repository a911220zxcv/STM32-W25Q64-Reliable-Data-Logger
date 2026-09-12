#ifndef FLASH_TEST_H
#define FLASH_TEST_H

#include "w25q64.h"

/* Debugger mailbox commands. Available only with FLASH_TEST_ENABLE=1.
 * Set command after checking the configured disposable sector.
 * No UART/CLI dependency; main only processes explicitly submitted commands. */
typedef enum
{
    FLASH_TEST_IDLE = 0,
    FLASH_TEST_READ_WRITE = 1,
    FLASH_TEST_ERASE = 2,
    FLASH_TEST_PAGE_BOUNDARY = 3,
    FLASH_TEST_POWER_PREPARE = 4,
    FLASH_TEST_POWER_VERIFY = 5
} FlashTestCommand_t;

extern volatile uint32_t g_flash_test_command;
extern volatile uint32_t g_flash_test_done;
extern volatile W25Q64_Status_t g_flash_test_result;
extern volatile uint32_t g_flash_test_value;

/* Explicit destructive tests: erase ONLY FLASH_TEST_SECTOR_ADDR. */
W25Q64_Status_t FlashTest_ReadWrite(void);
W25Q64_Status_t FlashTest_SectorErase(void);
W25Q64_Status_t FlashTest_PageBoundary(void);
/* Prepare erases/writes 12 34 56 78; Verify is strictly read-only after boot. */
W25Q64_Status_t FlashTest_PowerCyclePrepare(void);
W25Q64_Status_t FlashTest_PowerCycleVerify(void);
void FlashTest_Process(void);

#endif
