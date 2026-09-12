#include "w25q64.h"
#include "w25q64_port.h"
#include "config.h"
#include <stddef.h>

#define CMD_JEDEC_ID       0x9FU
#define CMD_STATUS_1       0x05U
#define CMD_STATUS_2       0x35U
#define CMD_WRITE_ENABLE   0x06U
#define CMD_READ           0x03U
#define CMD_PAGE_PROGRAM   0x02U
#define CMD_SECTOR_ERASE   0x20U
#define CMD_BLOCK_ERASE    0xD8U
#define CMD_RELEASE_POWER  0xABU
#define STATUS_BUSY        0x01U
#define STATUS_WEL         0x02U
#define DUMMY_BYTE         0xFFU
#define VERIFY_CHUNK       32U

static uint8_t port_ready;
static uint8_t initialized;

static uint8_t valid_range(uint32_t address, uint32_t length)
{
    return (uint8_t)((length != 0U) && (address < W25Q64_TOTAL_SIZE) &&
                     (length <= W25Q64_TOTAL_SIZE - address));
}

W25Q64_Status_t W25Q64_GetSPIClockHz(uint32_t *clock_hz)
{
    uint32_t result;
    if (clock_hz == NULL) { return W25Q64_INVALID_PARAM; }
    if (!initialized) { return W25Q64_NOT_INITIALIZED; }
    result = W25Q64_PortGetClockHz();
    if (result == 0U) { return W25Q64_ERROR; }
    *clock_hz = result;
    return W25Q64_OK;
}

/* Always terminate the transaction, including a failed command/address byte. */
static W25Q64_Status_t transaction(uint8_t command, uint32_t address,
                                 uint8_t has_address, const uint8_t *tx,
                                 uint8_t *rx, uint32_t length)
{
    W25Q64_Status_t status;
    uint8_t byte;
    uint32_t i;
    W25Q64_PortSelect(1U);
    status = W25Q64_PortTransfer(command, &byte);
    if ((status == W25Q64_OK) && has_address)
    {
        for (i = 0U; (i < 3U) && (status == W25Q64_OK); ++i)
        {
            status = W25Q64_PortTransfer((uint8_t)(address >> (16U - 8U * i)), &byte);
        }
    }
    for (i = 0U; (i < length) && (status == W25Q64_OK); ++i)
    {
        status = W25Q64_PortTransfer(tx != NULL ? tx[i] : DUMMY_BYTE, &byte);
        if ((status == W25Q64_OK) && (rx != NULL))
        {
            rx[i] = byte;
        }
    }
    W25Q64_PortSelect(0U);
    return status;
}

W25Q64_Status_t W25Q64_ReadStatusRegister(uint8_t number, uint8_t *value)
{
    static const uint8_t commands[] = {CMD_STATUS_1, CMD_STATUS_2};
    W25Q64_Status_t status;
    uint8_t result;
    if ((value == NULL) || (number < 1U) || (number > 2U))
    {
        return W25Q64_INVALID_PARAM;
    }
    if (!port_ready) { return W25Q64_NOT_INITIALIZED; }
    status = transaction(commands[number - 1U], 0U, 0U, NULL, &result, 1U);
    if (status == W25Q64_OK) { *value = result; }
    return status;
}

W25Q64_Status_t W25Q64_WaitBusy(uint32_t timeout_ms)
{
    uint32_t start;
    uint32_t polls;
    uint8_t value;
    W25Q64_Status_t status;
    if (timeout_ms > W25Q64_READY_TIMEOUT_MS) { return W25Q64_INVALID_PARAM; }
    if (!port_ready) { return W25Q64_NOT_INITIALIZED; }
    start = W25Q64_PortNowMs();
    for (polls = 0U; polls < W25Q64_POLL_LIMIT; ++polls)
    {
        status = W25Q64_ReadStatusRegister(1U, &value);
        if (status != W25Q64_OK) { return status; }
        if ((value & STATUS_BUSY) == 0U) { return W25Q64_OK; }
        if ((uint32_t)(W25Q64_PortNowMs() - start) >= timeout_ms)
        {
            return W25Q64_TIMEOUT;
        }
    }
    return W25Q64_TIMEOUT;
}

static W25Q64_Status_t ready(void)
{
    /* BV Rev E Fig.3b marks SR2[7:2] reserved, although section 11.2.21
     * mentions SUS without mapping its bit. Do not assume the JV layout.
     * This driver never suspends; callers must provide an unsuspended device. */
    return W25Q64_WaitBusy(W25Q64_READY_TIMEOUT_MS);
}

W25Q64_Status_t W25Q64_ReadJEDECID(uint32_t *id)
{
    uint8_t bytes[3];
    W25Q64_Status_t status;
    if (id == NULL) { return W25Q64_INVALID_PARAM; }
    if (!port_ready) { return W25Q64_NOT_INITIALIZED; }
    status = ready();
    if (status != W25Q64_OK) { return status; }
    status = transaction(CMD_JEDEC_ID, 0U, 0U, NULL, bytes, sizeof(bytes));
    if (status == W25Q64_OK)
    {
        *id = ((uint32_t)bytes[0] << 16) | ((uint32_t)bytes[1] << 8) | bytes[2];
    }
    return status;
}

static W25Q64_Status_t delay_ms(uint32_t duration)
{
    uint32_t start = W25Q64_PortNowMs();
    uint32_t polls;
    for (polls = 0U; polls < W25Q64_POLL_LIMIT; ++polls)
    {
        if ((uint32_t)(W25Q64_PortNowMs() - start) >= duration) { return W25Q64_OK; }
    }
    return W25Q64_TIMEOUT;
}

W25Q64_Status_t W25Q64_Init(void)
{
    W25Q64_Status_t status;
    uint32_t id;
    initialized = 0U;
    port_ready = 0U;
    status = W25Q64_PortInit();
    if (status != W25Q64_OK) { return status; }
    port_ready = 1U;
    status = delay_ms(W25Q64_POWER_UP_MS);
    if (status != W25Q64_OK) { return status; }
    status = transaction(CMD_RELEASE_POWER, 0U, 0U, NULL, NULL, 0U);
    if (status != W25Q64_OK) { return status; }
    status = delay_ms(W25Q64_WAKE_UP_MS);
    if (status != W25Q64_OK) { return status; }
    status = W25Q64_ReadJEDECID(&id);
    if (status != W25Q64_OK) { return status; }
    if (id != W25Q64_EXPECTED_JEDEC_ID) { return W25Q64_UNSUPPORTED_DEVICE; }
    initialized = 1U;
    return W25Q64_OK;
}

W25Q64_Status_t W25Q64_WriteEnable(void)
{
    W25Q64_Status_t status;
    uint8_t value;
    if (!initialized) { return W25Q64_NOT_INITIALIZED; }
    status = ready();
    if (status != W25Q64_OK) { return status; }
    status = transaction(CMD_WRITE_ENABLE, 0U, 0U, NULL, NULL, 0U);
    if (status != W25Q64_OK) { return status; }
    status = W25Q64_ReadStatusRegister(1U, &value);
    if (status != W25Q64_OK) { return status; }
    return ((value & (STATUS_BUSY | STATUS_WEL)) == STATUS_WEL) ?
           W25Q64_OK : W25Q64_WRITE_ENABLE_ERROR;
}

W25Q64_Status_t W25Q64_Read(uint32_t address, uint8_t *data, uint32_t length)
{
    W25Q64_Status_t status;
    if ((data == NULL) || !valid_range(address, length)) { return W25Q64_INVALID_PARAM; }
    if (!initialized) { return W25Q64_NOT_INITIALIZED; }
    status = ready();
    if (status != W25Q64_OK) { return status; }
    return transaction(CMD_READ, address, 1U, NULL, data, length);
}

/* Verification detects ignored/protected commands and impossible 0 -> 1 writes.
 * A short stack buffer bounds RAM usage independently of erase length. */
static W25Q64_Status_t verify(uint32_t address, const uint8_t *expected, uint32_t length)
{
    uint8_t bytes[VERIFY_CHUNK];
    uint32_t chunk;
    uint32_t i;
    W25Q64_Status_t status;
    while (length != 0U)
    {
        chunk = length > sizeof(bytes) ? sizeof(bytes) : length;
        status = W25Q64_Read(address, bytes, chunk);
        if (status != W25Q64_OK) { return status; }
        for (i = 0U; i < chunk; ++i)
        {
            if (bytes[i] != (expected != NULL ? expected[i] : DUMMY_BYTE))
            {
                return W25Q64_VERIFY_ERROR;
            }
        }
        if (expected != NULL) { expected += chunk; }
        address += chunk;
        length -= chunk;
    }
    return W25Q64_OK;
}

W25Q64_Status_t W25Q64_PageProgram(uint32_t address, const uint8_t *data, uint32_t length)
{
    W25Q64_Status_t status;
    if ((data == NULL) || !valid_range(address, length) ||
        (length > W25Q64_PAGE_SIZE - address % W25Q64_PAGE_SIZE))
    {
        return W25Q64_INVALID_PARAM;
    }
    status = W25Q64_WriteEnable();
    if (status != W25Q64_OK) { return status; }
    status = transaction(CMD_PAGE_PROGRAM, address, 1U, data, NULL, length);
    if (status != W25Q64_OK) { return status; }
    status = W25Q64_WaitBusy(W25Q64_PROGRAM_TIMEOUT_MS);
    if (status != W25Q64_OK) { return status; }
    return verify(address, data, length);
}

W25Q64_Status_t W25Q64_Write(uint32_t address, const uint8_t *data, uint32_t length)
{
    uint32_t chunk;
    W25Q64_Status_t status;
    if ((data == NULL) || !valid_range(address, length)) { return W25Q64_INVALID_PARAM; }
    while (length != 0U)
    {
        chunk = W25Q64_PAGE_SIZE - address % W25Q64_PAGE_SIZE;
        if (chunk > length) { chunk = length; }
        status = W25Q64_PageProgram(address, data, chunk);
        if (status != W25Q64_OK) { return status; }
        address += chunk;
        data += chunk;
        length -= chunk;
    }
    return W25Q64_OK;
}

static W25Q64_Status_t erase(uint32_t address, uint32_t size, uint8_t command,
                           uint32_t timeout_ms)
{
    W25Q64_Status_t status;
    if (!valid_range(address, size) || ((address % size) != 0U))
    {
        return W25Q64_INVALID_PARAM;
    }
    status = W25Q64_WriteEnable();
    if (status != W25Q64_OK) { return status; }
    status = transaction(command, address, 1U, NULL, NULL, 0U);
    if (status != W25Q64_OK) { return status; }
    status = W25Q64_WaitBusy(timeout_ms);
    if (status != W25Q64_OK) { return status; }
    return verify(address, NULL, size);
}

W25Q64_Status_t W25Q64_SectorErase(uint32_t address)
{
    return erase(address, W25Q64_SECTOR_SIZE, CMD_SECTOR_ERASE, W25Q64_SECTOR_TIMEOUT_MS);
}

W25Q64_Status_t W25Q64_BlockErase(uint32_t address)
{
    return erase(address, W25Q64_BLOCK_SIZE, CMD_BLOCK_ERASE, W25Q64_BLOCK_TIMEOUT_MS);
}
