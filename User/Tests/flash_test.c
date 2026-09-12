#include "config.h"
#include "flash_test.h"

#if FLASH_TEST_ENABLE
volatile uint32_t g_flash_test_command = FLASH_TEST_IDLE;
volatile uint32_t g_flash_test_done;
volatile W25Q64_Status_t g_flash_test_result = W25Q64_NOT_INITIALIZED;
volatile uint32_t g_flash_test_value;

W25Q64_Status_t FlashTest_PowerCycleVerify(void)
{
    uint8_t data[4];
    W25Q64_Status_t status = W25Q64_Read(FLASH_TEST_SECTOR_ADDR, data, sizeof(data));
    if (status != W25Q64_OK) { return status; }
    g_flash_test_value = ((uint32_t)data[0] << 24) | ((uint32_t)data[1] << 16) |
                         ((uint32_t)data[2] << 8) | data[3];
    return g_flash_test_value == 0x12345678UL ? W25Q64_OK : W25Q64_VERIFY_ERROR;
}

W25Q64_Status_t FlashTest_PowerCyclePrepare(void)
{
    static const uint8_t data[] = {0x12U, 0x34U, 0x56U, 0x78U};
    W25Q64_Status_t status = W25Q64_SectorErase(FLASH_TEST_SECTOR_ADDR);
    if (status != W25Q64_OK) { return status; }
    return W25Q64_PageProgram(FLASH_TEST_SECTOR_ADDR, data, sizeof(data));
}

W25Q64_Status_t FlashTest_ReadWrite(void)
{
    W25Q64_Status_t status = FlashTest_PowerCyclePrepare();
    if (status != W25Q64_OK) { return status; }
    return FlashTest_PowerCycleVerify();
}

W25Q64_Status_t FlashTest_SectorErase(void)
{
    static const uint8_t zero = 0U;
    W25Q64_Status_t status = W25Q64_PageProgram(FLASH_TEST_SECTOR_ADDR, &zero, 1U);
    if (status != W25Q64_OK) { return status; }
    /* Driver verifies all 4096 erased bytes, not only the first word. */
    return W25Q64_SectorErase(FLASH_TEST_SECTOR_ADDR);
}

W25Q64_Status_t FlashTest_PageBoundary(void)
{
    uint8_t data[32];
    uint8_t readback[32];
    uint32_t i;
    uint32_t address = FLASH_TEST_SECTOR_ADDR + W25Q64_PAGE_SIZE - 8U;
    W25Q64_Status_t status = W25Q64_SectorErase(FLASH_TEST_SECTOR_ADDR);
    if (status != W25Q64_OK) { return status; }
    for (i = 0U; i < sizeof(data); ++i) { data[i] = (uint8_t)(0xA5U ^ i); }
    status = W25Q64_PageProgram(address, data, sizeof(data));
    if (status != W25Q64_INVALID_PARAM) { return W25Q64_ERROR; }
    status = W25Q64_Read(address, readback, sizeof(readback));
    if (status != W25Q64_OK) { return status; }
    for (i = 0U; i < sizeof(data); ++i)
    {
        if (readback[i] != 0xFFU) { return W25Q64_VERIFY_ERROR; }
    }
    status = W25Q64_Write(address, data, sizeof(data));
    if (status != W25Q64_OK) { return status; }
    status = W25Q64_Read(address, readback, sizeof(readback));
    if (status != W25Q64_OK) { return status; }
    for (i = 0U; i < sizeof(data); ++i)
    {
        if (readback[i] != data[i]) { return W25Q64_VERIFY_ERROR; }
    }
    return W25Q64_OK;
}

void FlashTest_Process(void)
{
    uint32_t command = g_flash_test_command;
    W25Q64_Status_t status;
    if (command == FLASH_TEST_IDLE) { return; }
    g_flash_test_command = FLASH_TEST_IDLE;
    g_flash_test_done = 0U;
    switch (command)
    {
        case FLASH_TEST_READ_WRITE: status = FlashTest_ReadWrite(); break;
        case FLASH_TEST_ERASE: status = FlashTest_SectorErase(); break;
        case FLASH_TEST_PAGE_BOUNDARY: status = FlashTest_PageBoundary(); break;
        case FLASH_TEST_POWER_PREPARE: status = FlashTest_PowerCyclePrepare(); break;
        case FLASH_TEST_POWER_VERIFY: status = FlashTest_PowerCycleVerify(); break;
        default: status = W25Q64_INVALID_PARAM; break;
    }
    g_flash_test_result = status;
    g_flash_test_done = command;
}
#endif
