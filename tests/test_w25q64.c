/* Protocol-level NOR model. Commands are decoded independently of the driver.
 * WREN latches only at CS rising, erase/program start at CS rising, programming
 * ANDs bits, addresses wrap inside a page, BUSY rejects unrelated commands.
 * This checks logical behavior, not analogue SPI timing or real power failure. */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <limits.h>
#include "w25q64_port.h"
#include "flash_test.h"
#include "flash_manager.h"
#include "flash_manager_test.h"
#include "crc32.h"
#include "config.h"
#include "mpu6050_port.h"
#include "data_logger.h"
#include "data_logger_test.h"
#include "system_time.h"
#include "wear_leveling.h"
#include "uart_port.h"
#include "cli.h"
#include "flash_benchmark.h"
#include "power_fail_test.h"

static uint8_t memory[W25Q64_TOTAL_SIZE];
static uint8_t selected, command, wel, sr2, deny_wren, ignore_modify;
static uint8_t page[256];
static uint32_t position, address, payload, transfers, programs, sectors, blocks, wrens;
static uint32_t now, busy_until, busy_duration = 2U, id = 0xEF4017U;
static uint32_t command_count[256];
static int stuck_busy, frozen_clock, fail_after = -1, failed_transaction;
static int corrupt_read;
static W25Q64_Status_t init_result = W25Q64_OK;
static uint8_t sensor_registers[128];
static MPU6050_Status_t sensor_port_init_result = MPU6050_OK;
static MPU6050_Status_t sensor_read_result = MPU6050_OK;
static MPU6050_Status_t sensor_write_result = MPU6050_OK;
static int sensor_ignore_write;
static uint32_t sensor_burst_reads, logger_now;
static uint32_t system_time_script[8];
static uint32_t system_time_script_length, system_time_script_position;
static uint8_t uart_input[256];
static uint32_t uart_input_length, uart_input_position;
static char uart_output[65536];
static uint32_t uart_output_length;
static UARTPortStatus_t uart_init_result = UART_PORT_OK;
static UARTPortStatus_t uart_write_result = UART_PORT_OK;

UARTPortStatus_t UART_PortInit(void)
{
    return uart_init_result;
}

UARTPortStatus_t UART_PortTryRead(uint8_t *byte)
{
    if (byte == NULL) { return UART_PORT_INVALID_PARAM; }
    if (uart_input_position >= uart_input_length) { return UART_PORT_EMPTY; }
    *byte = uart_input[uart_input_position++];
    return UART_PORT_OK;
}

UARTPortStatus_t UART_PortWrite(const uint8_t *data, uint32_t length)
{
    if ((data == NULL) || (length == 0U))
    {
        return UART_PORT_INVALID_PARAM;
    }
    if (uart_write_result != UART_PORT_OK) { return uart_write_result; }
    assert(length < sizeof(uart_output) - uart_output_length);
    memcpy(uart_output + uart_output_length, data, length);
    uart_output_length += length;
    uart_output[uart_output_length] = '\0';
    return UART_PORT_OK;
}

static void reset_uart_output(void)
{
    uart_output_length = 0U;
    uart_output[0] = '\0';
}

static void feed_uart(const char *text)
{
    uint32_t length = (uint32_t)strlen(text);
    assert(length <= sizeof(uart_input));
    memcpy(uart_input, text, length);
    uart_input_length = length;
    uart_input_position = 0U;
}

SystemTimeStatus_t SystemTime_Init(void)
{
    logger_now = 0U;
    return SYSTEM_TIME_OK;
}

uint32_t SystemTime_GetMs(void)
{
    if (system_time_script_position < system_time_script_length)
    {
        return system_time_script[system_time_script_position++];
    }
    return logger_now;
}

void SystemTime_TickISR(void)
{
    ++logger_now;
}

static void set_system_time_script(const uint32_t *values, uint32_t length)
{
    assert(values != NULL && length <= 8U);
    memcpy(system_time_script, values, length * sizeof(values[0]));
    system_time_script_length = length;
    system_time_script_position = 0U;
}

MPU6050_Status_t MPU6050_PortInit(void)
{
    return sensor_port_init_result;
}

MPU6050_Status_t MPU6050_PortWriteRegister(uint8_t register_address,
                                           uint8_t value)
{
    if (sensor_write_result != MPU6050_OK) { return sensor_write_result; }
    if (!sensor_ignore_write) { sensor_registers[register_address] = value; }
    return MPU6050_OK;
}

MPU6050_Status_t MPU6050_PortReadRegisters(uint8_t register_address,
                                           uint8_t *data,
                                           uint32_t length)
{
    uint32_t index;
    if ((data == NULL) || (length == 0U) ||
        (length > sizeof(sensor_registers) - register_address))
    {
        return MPU6050_INVALID_PARAM;
    }
    if (sensor_read_result != MPU6050_OK) { return sensor_read_result; }
    for (index = 0U; index < length; ++index)
    {
        data[index] = sensor_registers[(uint32_t)register_address + index];
    }
    if ((register_address == 0x3BU) && (length == 14U)) { ++sensor_burst_reads; }
    return MPU6050_OK;
}

static void reset_sensor(void)
{
    memset(sensor_registers, 0, sizeof(sensor_registers));
    sensor_registers[0x75U] = MPU6050_EXPECTED_WHO_AM_I;
    sensor_port_init_result = MPU6050_OK;
    sensor_read_result = MPU6050_OK;
    sensor_write_result = MPU6050_OK;
    sensor_ignore_write = 0;
    sensor_burst_reads = 0U;
}

static void set_sensor_i16(uint8_t register_address, int16_t value)
{
    uint16_t bits = (uint16_t)value;
    sensor_registers[register_address] = (uint8_t)(bits >> 8U);
    sensor_registers[(uint8_t)(register_address + 1U)] = (uint8_t)bits;
}

static void set_sensor_sample(int16_t base)
{
    set_sensor_i16(0x3BU, base);
    set_sensor_i16(0x3DU, (int16_t)(base + 1));
    set_sensor_i16(0x3FU, (int16_t)(base + 2));
    set_sensor_i16(0x41U, (int16_t)0x1234); /* Temperature is discarded. */
    set_sensor_i16(0x43U, (int16_t)(base + 3));
    set_sensor_i16(0x45U, (int16_t)(base + 4));
    set_sensor_i16(0x47U, (int16_t)(base + 5));
}

static int busy(void)
{
    return stuck_busy || (int32_t)(busy_until - now) > 0;
}

W25Q64_Status_t W25Q64_PortInit(void)
{
    selected = 0U;
    return init_result;
}

uint32_t W25Q64_PortNowMs(void)
{
    if (!frozen_clock) { ++now; }
    return now;
}

uint32_t W25Q64_PortGetClockHz(void)
{
    return 9000000UL;
}

void W25Q64_PortSelect(uint8_t active)
{
    if (active)
    {
        assert(!selected);
        selected = 1U;
        position = 0U;
        address = 0U;
        payload = 0U;
        command = 0U;
        failed_transaction = 0;
        memset(page, 0xFF, sizeof(page));
        return;
    }
    assert(selected);
    selected = 0U;
    if (position == 0U || failed_transaction) { return; }
    if (command == 0x06U)
    {
        assert(position == 1U);
        if (!busy() && !deny_wren) { wel = 1U; }
        ++wrens;
    }
    if (command == 0x02U || command == 0x20U || command == 0xD8U)
    {
        uint32_t size;
        uint32_t i;
        assert(wel && !busy());
        assert(position >= 4U);
        if (command == 0x02U)
        {
            /* Fail if driver relies on flash's destructive page wrap. */
            assert(payload > 0U && payload <= 256U - address % 256U);
            if (!ignore_modify)
            {
                for (i = 0U; i < payload; ++i)
                {
                    memory[(address & ~255U) | ((address + i) & 255U)] &= page[i];
                }
            }
            ++programs;
        }
        else
        {
            assert(position == 4U);
            size = command == 0x20U ? 4096U : 65536U;
            assert(address % size == 0U && address <= W25Q64_TOTAL_SIZE - size);
            if (!ignore_modify) { memset(memory + address, 0xFF, size); }
            if (command == 0x20U) { ++sectors; } else { ++blocks; }
        }
        wel = 0U;
        busy_until = now + busy_duration;
    }
}

W25Q64_Status_t W25Q64_PortTransfer(uint8_t tx, uint8_t *rx)
{
    assert(selected);
    ++transfers;
    if (fail_after == 0)
    {
        failed_transaction = 1;
        return W25Q64_TIMEOUT;
    }
    if (fail_after > 0) { --fail_after; }
    *rx = 0xFFU;
    if (position == 0U)
    {
        command = tx;
        ++command_count[tx];
        assert(tx == 0x9FU || tx == 0x05U || tx == 0x35U ||
               tx == 0x06U || tx == 0x03U || tx == 0x02U || tx == 0x20U ||
               tx == 0xD8U || tx == 0xABU);
        if (busy()) { assert(tx == 0x05U || tx == 0xABU); }
    }
    else if ((command == 0x03U || command == 0x02U || command == 0x20U ||
              command == 0xD8U) && position <= 3U)
    {
        address = (address << 8) | tx;
        assert(address < W25Q64_TOTAL_SIZE);
    }
    else if (command == 0x03U)
    {
        assert(address + payload < W25Q64_TOTAL_SIZE);
        *rx = memory[address + payload++];
        if (corrupt_read) { *rx ^= 1U; }
    }
    else if (command == 0x02U)
    {
        assert(payload < sizeof(page));
        page[payload++] = tx;
    }
    else if (command == 0x9FU)
    {
        assert(position <= 3U);
        *rx = (uint8_t)(id >> (24U - position * 8U));
    }
    else if (command == 0x05U) { *rx = (uint8_t)((busy() ? 1U : 0U) | (wel ? 2U : 0U)); }
    else if (command == 0x35U) { *rx = sr2; }
    ++position;
    return W25Q64_OK;
}

static void reset_model(void)
{
    memset(memory, 0xFF, sizeof(memory));
    memset(command_count, 0, sizeof(command_count));
    selected = wel = sr2 = deny_wren = ignore_modify = 0U;
    now = busy_until = transfers = programs = sectors = blocks = wrens = 0U;
    busy_duration = 2U;
    id = 0xEF4017U;
    stuck_busy = frozen_clock = corrupt_read = 0;
    fail_after = -1;
    init_result = W25Q64_OK;
    uart_input_length = uart_input_position = 0U;
    uart_init_result = uart_write_result = UART_PORT_OK;
    system_time_script_length = system_time_script_position = 0U;
    g_power_fail_test_point = POWER_FAIL_POINT_NONE;
    g_power_fail_test_last_point = POWER_FAIL_POINT_NONE;
    g_power_fail_test_trigger_count = 0U;
    g_power_fail_test_host_reset = 0U;
    reset_uart_output();
    assert(W25Q64_Init() == W25Q64_OK);
}

static void test_identity(void)
{
    uint32_t actual = 0U;
    uint8_t status = 0U;
    reset_model();
    assert(W25Q64_ReadJEDECID(&actual) == W25Q64_OK && actual == id);
    sr2 = 0x02U; /* BV SR2 QE bit; read only, never modified by this driver. */
    assert(W25Q64_ReadStatusRegister(2U, &status) == W25Q64_OK && status == 0x02U);
    assert(now >= W25Q64_POWER_UP_MS + W25Q64_WAKE_UP_MS);
    assert(programs == 0U && sectors == 0U && blocks == 0U && wrens == 0U);
    id = 0xFFFFFFU;
    assert(W25Q64_Init() == W25Q64_UNSUPPORTED_DEVICE);
    assert(W25Q64_PageProgram(0U, &status, 1U) == W25Q64_NOT_INITIALIZED);
    id = 0U;
    assert(W25Q64_Init() == W25Q64_UNSUPPORTED_DEVICE);
    init_result = W25Q64_NOT_CONFIGURED;
    assert(W25Q64_Init() == W25Q64_NOT_CONFIGURED);
    assert(W25Q64_ReadJEDECID(&actual) == W25Q64_NOT_INITIALIZED);
    puts("PASS identity, status, read-only boot and initialization gating");
}

static void test_parameters(void)
{
    uint8_t data[257] = {0U};
    uint32_t before;
    reset_model();
    before = transfers;
    assert(W25Q64_Read(0U, NULL, 1U) == W25Q64_INVALID_PARAM);
    assert(W25Q64_Read(0U, data, 0U) == W25Q64_INVALID_PARAM);
    assert(W25Q64_Read(W25Q64_TOTAL_SIZE, data, 1U) == W25Q64_INVALID_PARAM);
    assert(W25Q64_Read(UINT32_MAX, data, 2U) == W25Q64_INVALID_PARAM);
    assert(W25Q64_Read(1U, data, UINT32_MAX) == W25Q64_INVALID_PARAM);
    assert(W25Q64_PageProgram(255U, data, 2U) == W25Q64_INVALID_PARAM);
    assert(W25Q64_PageProgram(0U, data, 257U) == W25Q64_INVALID_PARAM);
    assert(W25Q64_PageProgram(0U, NULL, 1U) == W25Q64_INVALID_PARAM);
    assert(W25Q64_Write(0U, NULL, 1U) == W25Q64_INVALID_PARAM);
    assert(W25Q64_Write(0U, data, 0U) == W25Q64_INVALID_PARAM);
    assert(W25Q64_Write(W25Q64_TOTAL_SIZE - 1U, data, 2U) == W25Q64_INVALID_PARAM);
    assert(W25Q64_Write(UINT32_MAX, data, 1U) == W25Q64_INVALID_PARAM);
    assert(W25Q64_SectorErase(1U) == W25Q64_INVALID_PARAM);
    assert(W25Q64_SectorErase(W25Q64_TOTAL_SIZE) == W25Q64_INVALID_PARAM);
    assert(W25Q64_BlockErase(4096U) == W25Q64_INVALID_PARAM);
    assert(W25Q64_BlockErase(UINT32_MAX) == W25Q64_INVALID_PARAM);
    assert(W25Q64_ReadStatusRegister(0U, data) == W25Q64_INVALID_PARAM);
    assert(W25Q64_ReadStatusRegister(3U, data) == W25Q64_INVALID_PARAM);
    assert(W25Q64_ReadStatusRegister(4U, data) == W25Q64_INVALID_PARAM);
    assert(W25Q64_ReadStatusRegister(1U, NULL) == W25Q64_INVALID_PARAM);
    assert(W25Q64_ReadJEDECID(NULL) == W25Q64_INVALID_PARAM);
    assert(W25Q64_WaitBusy(UINT32_MAX) == W25Q64_INVALID_PARAM);
    assert(transfers == before);
    assert(W25Q64_PageProgram(W25Q64_TOTAL_SIZE - 1U, data, 1U) == W25Q64_OK);
    assert(W25Q64_Read(W25Q64_TOTAL_SIZE - 1U, data, 1U) == W25Q64_OK && data[0] == 0U);
    puts("PASS null/zero/overflow/alignment/page/capacity boundaries; invalid calls emit no SPI");
}

static void test_program_and_erase(void)
{
    uint8_t data[600], output[600];
    uint32_t i;
    reset_model();
    for (i = 0U; i < sizeof(data); ++i) { data[i] = (uint8_t)(i ^ 0xA5U); }
    assert(W25Q64_Write(0x20F8U, data, sizeof(data)) == W25Q64_OK);
    assert(programs == 4U && wrens == 4U);
    assert(W25Q64_Read(0x20F8U, output, sizeof(output)) == W25Q64_OK);
    assert(memcmp(data, output, sizeof(data)) == 0);
    assert(memory[0x20F7U] == 0xFFU && memory[0x2350U] == 0xFFU);
    data[0] = 0xFFU;
    assert(W25Q64_PageProgram(0x20F8U, data, 1U) == W25Q64_VERIFY_ERROR);
    memory[0x1FFFU] = memory[0x3000U] = 0x55U;
    assert(W25Q64_SectorErase(0x2000U) == W25Q64_OK);
    for (i = 0x2000U; i < 0x3000U; ++i) { assert(memory[i] == 0xFFU); }
    assert(memory[0x1FFFU] == 0x55U && memory[0x3000U] == 0x55U);
    memset(memory + 0x10000U, 0U, 65536U);
    memory[0xFFFFU] = memory[0x20000U] = 0x55U;
    assert(W25Q64_BlockErase(0x10000U) == W25Q64_OK);
    assert(memory[0xFFFFU] == 0x55U && memory[0x20000U] == 0x55U);
    assert(sectors == 1U && blocks == 1U && wrens == programs + sectors + blocks);
    assert(W25Q64_SectorErase(W25Q64_TOTAL_SIZE - W25Q64_SECTOR_SIZE) == W25Q64_OK);
    assert(W25Q64_BlockErase(W25Q64_TOTAL_SIZE - W25Q64_BLOCK_SIZE) == W25Q64_OK);
    puts("PASS multi-page splitting, WREN per operation, NOR 1->0, exact erase extents");
}

static void test_timeouts(void)
{
    uint8_t byte = 0x12U;
    uint32_t before;
    reset_model();
    assert(W25Q64_WaitBusy(0U) == W25Q64_OK);
    stuck_busy = 1;
    assert(W25Q64_WaitBusy(0U) == W25Q64_TIMEOUT);
    before = now;
    assert(W25Q64_Read(0U, &byte, 1U) == W25Q64_TIMEOUT);
    assert(now - before <= W25Q64_READY_TIMEOUT_MS + 2U);
    frozen_clock = 1;
    assert(W25Q64_WaitBusy(2U) == W25Q64_TIMEOUT);
    frozen_clock = stuck_busy = 0;
    now = UINT32_MAX - 2U;
    busy_until = now + 5U;
    assert(W25Q64_WaitBusy(10U) == W25Q64_OK);
    busy_duration = W25Q64_PROGRAM_TIMEOUT_MS + 20U;
    assert(W25Q64_PageProgram(0U, &byte, 1U) == W25Q64_TIMEOUT);
    now = busy_until;
    busy_duration = W25Q64_SECTOR_TIMEOUT_MS + 20U;
    assert(W25Q64_SectorErase(0U) == W25Q64_TIMEOUT);
    now = busy_until;
    busy_duration = W25Q64_BLOCK_TIMEOUT_MS + 20U;
    assert(W25Q64_BlockErase(0U) == W25Q64_TIMEOUT);
    now = busy_until;
    busy_duration = 0U; /* Fast completion: observing BUSY=1 is not required. */
    assert(W25Q64_PageProgram(0U, &byte, 1U) == W25Q64_OK);
    sr2 = 0xFCU; /* Reserved BV bits must not be interpreted as JV status. */
    assert(W25Q64_Read(0U, &byte, 1U) == W25Q64_OK);
    assert(W25Q64_Init() == W25Q64_OK);
    puts("PASS busy/operation deadlines, stopped timebase, timer wrap, fast completion, BV reserved bits");
}

static void test_failures(void)
{
    uint8_t byte = 0x12U;
    uint32_t total;
    int i;
    reset_model();
    deny_wren = 1U;
    assert(W25Q64_PageProgram(0U, &byte, 1U) == W25Q64_WRITE_ENABLE_ERROR);
    assert(programs == 0U);
    deny_wren = 0U;
    ignore_modify = 1U;
    assert(W25Q64_PageProgram(0U, &byte, 1U) == W25Q64_VERIFY_ERROR);
    memory[0U] = 0U;
    assert(W25Q64_SectorErase(0U) == W25Q64_VERIFY_ERROR);
    ignore_modify = 0U;
    corrupt_read = 1;
    assert(W25Q64_PageProgram(1U, &byte, 1U) == W25Q64_VERIFY_ERROR);
    reset_model();
    total = transfers;
    assert(W25Q64_PageProgram(0U, &byte, 1U) == W25Q64_OK);
    total = transfers - total;
    for (i = 0; i < (int)total; ++i)
    {
        reset_model();
        fail_after = i;
        assert(W25Q64_PageProgram(0U, &byte, 1U) == W25Q64_TIMEOUT);
        assert(!selected); /* Every command/address/payload/poll/verify exit releases CS. */
    }
    reset_model();
    fail_after = 0;
    assert(W25Q64_Init() == W25Q64_TIMEOUT && !selected);
    fail_after = -1;
    frozen_clock = 1;
    assert(W25Q64_Init() == W25Q64_TIMEOUT);
    printf("PASS ignored/protected commands, read corruption, CS cleanup at all %lu program transfer sites\n",
           (unsigned long)total);
}

static void test_hardware_test_entrypoints(void)
{
    uint32_t p, s;
    reset_model();
    FlashTest_Process();
    assert(programs == 0U && sectors == 0U);
    assert(FlashTest_ReadWrite() == W25Q64_OK);
    assert(FlashTest_SectorErase() == W25Q64_OK);
    assert(FlashTest_PageBoundary() == W25Q64_OK);
    assert(FlashTest_PowerCyclePrepare() == W25Q64_OK);
    p = programs;
    s = sectors;
    /* MCU/flash volatile state reset while the model's nonvolatile bytes persist. */
    wel = 0U;
    now = busy_until = 0U;
    assert(W25Q64_Init() == W25Q64_OK);
    assert(FlashTest_PowerCycleVerify() == W25Q64_OK);
    assert(g_flash_test_value == 0x12345678U && p == programs && s == sectors);
    g_flash_test_command = 99U;
    FlashTest_Process();
    assert(g_flash_test_result == W25Q64_INVALID_PARAM && g_flash_test_done == 99U);
    puts("PASS debugger tests and persistence model (physical power-cycle recorded separately)");
}

static void assert_record_equal(const LogRecord_t *left, const LogRecord_t *right)
{
    assert(left->magic == right->magic);
    assert(left->sequence == right->sequence);
    assert(left->timestamp_ms == right->timestamp_ms);
    assert(left->accel_x == right->accel_x);
    assert(left->accel_y == right->accel_y);
    assert(left->accel_z == right->accel_z);
    assert(left->gyro_x == right->gyro_x);
    assert(left->gyro_y == right->gyro_y);
    assert(left->gyro_z == right->gyro_z);
}

static void corrupt_model_byte_one_to_zero(uint32_t target)
{
    uint32_t offset;
    uint8_t bit;
    for (offset = 0U; offset < 4U; ++offset)
    {
        for (bit = 0U; bit < 8U; ++bit)
        {
            if ((memory[target + offset] & (uint8_t)(1U << bit)) != 0U)
            {
                memory[target + offset] &=
                    (uint8_t)~(uint8_t)(1U << bit);
                return;
            }
        }
    }
    assert(0);
}

static LogRecord_t phase2_record(void);
static void reset_logger_environment(void);

static LogRecord_t recovery_record(uint32_t sequence)
{
    LogRecord_t record = phase2_record();
    record.sequence = sequence;
    record.timestamp_ms = 1000U + sequence;
    record.accel_x = (int16_t)(0x1234 + sequence);
    return record;
}

static LogRecord_t phase2_record(void)
{
    LogRecord_t record;
    record.magic = FLASH_LOG_RECORD_MAGIC;
    record.sequence = 0x12345678UL;
    record.timestamp_ms = 0xA1B2C3D4UL;
    record.accel_x = (int16_t)0x1122;
    record.accel_y = -2;
    record.accel_z = 32767;
    record.gyro_x = -32768;
    record.gyro_y = 0;
    record.gyro_z = (int16_t)0x3344;
    record.crc32 = 0xDEADBEEFUL; /* Serializer must ignore this field. */
    return record;
}

static void test_flash_manager_codec_and_map(void)
{
    static const uint8_t check[] = "123456789";
    static const uint8_t prefix[FLASH_LOG_CRC_OFFSET] =
    {
        0x4CU, 0x4FU, 0x47U, 0x31U,
        0x78U, 0x56U, 0x34U, 0x12U,
        0xD4U, 0xC3U, 0xB2U, 0xA1U,
        0x22U, 0x11U, 0xFEU, 0xFFU,
        0xFFU, 0x7FU, 0x00U, 0x80U,
        0x00U, 0x00U, 0x44U, 0x33U
    };
    uint8_t encoded[FLASH_LOG_RECORD_SIZE];
    uint8_t erased[FLASH_LOG_RECORD_SIZE];
    LogRecord_t input = phase2_record();
    LogRecord_t output;
    uint32_t mapped_address;
    uint32_t stored_crc;

    assert(CRC32_Calculate(check, 9U) == 0xCBF43926UL);
    assert(CRC32_Calculate(NULL, 0U) == 0U);
    assert(CRC32_Calculate(NULL, 1U) == 0U);
    assert(FLASH_LOG_SECTOR_COUNT == 2046UL);
    assert(FLASH_RECORDS_PER_SECTOR == 146UL);
    assert(FLASH_SECTOR_UNUSED_BYTES == 8UL);
    assert(FLASH_LOG_RECORD_CAPACITY == 298716UL);

    assert(FlashManager_SerializeRecord(&input, encoded) == FLASH_MANAGER_OK);
    assert(memcmp(encoded, prefix, sizeof(prefix)) == 0);
    stored_crc = (uint32_t)encoded[24] | ((uint32_t)encoded[25] << 8) |
                 ((uint32_t)encoded[26] << 16) | ((uint32_t)encoded[27] << 24);
    assert(stored_crc == CRC32_Calculate(encoded, FLASH_LOG_CRC_OFFSET));
    assert(FlashManager_DeserializeRecord(encoded, &output) == FLASH_MANAGER_OK);
    assert_record_equal(&input, &output);
    assert(output.crc32 == stored_crc);

    encoded[12] ^= 1U;
    assert(FlashManager_DeserializeRecord(encoded, &output) == FLASH_MANAGER_CRC_ERROR);
    encoded[0] = 0U;
    assert(FlashManager_DeserializeRecord(encoded, &output) == FLASH_MANAGER_INVALID_RECORD);
    memset(erased, 0xFF, sizeof(erased));
    assert(FlashManager_DeserializeRecord(erased, &output) == FLASH_MANAGER_EMPTY);
    input.magic = 0U;
    assert(FlashManager_SerializeRecord(&input, encoded) == FLASH_MANAGER_INVALID_RECORD);
    assert(FlashManager_SerializeRecord(NULL, encoded) == FLASH_MANAGER_INVALID_PARAM);
    assert(FlashManager_DeserializeRecord(encoded, NULL) == FLASH_MANAGER_INVALID_PARAM);

    assert(FlashManager_GetSectorAddress(0U, &mapped_address) == FLASH_MANAGER_OK);
    assert(mapped_address == LOG_START_ADDR);
    assert(FlashManager_GetSectorAddress(FLASH_LOG_SECTOR_COUNT - 1U, &mapped_address) ==
           FLASH_MANAGER_OK);
    assert(mapped_address == LOG_END_ADDR - FLASH_SECTOR_SIZE);
    assert(FlashManager_GetSectorAddress(FLASH_LOG_SECTOR_COUNT, &mapped_address) ==
           FLASH_MANAGER_INVALID_PARAM);
    assert(FlashManager_GetRecordAddress(0U, 9U, &mapped_address) == FLASH_MANAGER_OK);
    assert(mapped_address == LOG_START_ADDR + 252U);
    assert(FlashManager_GetRecordAddress(0U, FLASH_RECORDS_PER_SECTOR - 1U,
                                         &mapped_address) == FLASH_MANAGER_OK);
    assert(mapped_address + FLASH_LOG_RECORD_SIZE == LOG_START_ADDR +
           FLASH_RECORDS_PER_SECTOR * FLASH_LOG_RECORD_SIZE);
    assert(FlashManager_GetRecordAddress(0U, FLASH_RECORDS_PER_SECTOR, &mapped_address) ==
           FLASH_MANAGER_INVALID_PARAM);
    assert(FlashManager_GetRecordAddress(0U, 0U, NULL) == FLASH_MANAGER_INVALID_PARAM);
    puts("PASS Phase 2 CRC32 vector, exact little-endian codec, CRC/magic/empty checks and map bounds");
}

static void test_flash_manager_storage(void)
{
    LogRecord_t input = phase2_record();
    LogRecord_t output;
    uint32_t before_programs;
    uint32_t last_sector = FLASH_LOG_SECTOR_COUNT - 1U;

    reset_model();
    assert(FlashManager_PrepareSector(0U) == FLASH_MANAGER_NOT_INITIALIZED);
    assert(FlashManager_Init() == FLASH_MANAGER_OK);
    memory[METADATA_A_ADDR] = 0x55U;
    memory[METADATA_B_ADDR] = 0x66U;
    memory[LOG_START_ADDR + FLASH_SECTOR_SIZE] = 0x77U;
    assert(FlashManager_PrepareSector(0U) == FLASH_MANAGER_OK);
    assert(memory[METADATA_A_ADDR] == 0x55U);
    assert(memory[METADATA_B_ADDR] == 0x66U);
    assert(memory[LOG_START_ADDR + FLASH_SECTOR_SIZE] == 0x77U);
    assert(FlashManager_ReadRecord(0U, 9U, &output) == FLASH_MANAGER_EMPTY);

    before_programs = programs;
    assert(FlashManager_WriteRecord(0U, 9U, &input) == FLASH_MANAGER_OK);
    assert(programs - before_programs == 2U); /* Offset 252 crosses a 256-byte page. */
    assert(FlashManager_ReadRecord(0U, 9U, &output) == FLASH_MANAGER_OK);
    assert_record_equal(&input, &output);
    before_programs = programs;
    assert(FlashManager_WriteRecord(0U, 9U, &input) == FLASH_MANAGER_NOT_ERASED);
    assert(programs == before_programs);

    assert(FlashManager_WriteRecord(0U, FLASH_RECORDS_PER_SECTOR - 1U, &input) ==
           FLASH_MANAGER_OK);
    assert(memory[LOG_START_ADDR + FLASH_SECTOR_SIZE - 1U] == 0xFFU);
    assert(FlashManager_WriteRecord(0U, FLASH_RECORDS_PER_SECTOR, &input) ==
           FLASH_MANAGER_INVALID_PARAM);
    assert(FlashManager_PrepareSector(FLASH_LOG_SECTOR_COUNT) ==
           FLASH_MANAGER_INVALID_PARAM);
    assert(FlashManager_ReadRecord(0U, 0U, NULL) == FLASH_MANAGER_INVALID_PARAM);

    assert(FlashManager_PrepareSector(last_sector) == FLASH_MANAGER_OK);
    assert(FlashManager_WriteRecord(last_sector, FLASH_RECORDS_PER_SECTOR - 1U,
                                    &input) == FLASH_MANAGER_OK);
    assert(FlashManager_ReadRecord(last_sector, FLASH_RECORDS_PER_SECTOR - 1U,
                                   &output) == FLASH_MANAGER_OK);
    assert_record_equal(&input, &output);
    assert(memory[LOG_END_ADDR - 1U] == 0xFFU);

    assert(FlashManager_PrepareSector(0U) == FLASH_MANAGER_OK);
    assert(FlashManager_WriteRecord(0U, 9U, &input) == FLASH_MANAGER_OK);
    memory[LOG_START_ADDR + 252U + 12U] &= 0xFDU;
    assert(FlashManager_ReadRecord(0U, 9U, &output) == FLASH_MANAGER_CRC_ERROR);
    puts("PASS Phase 2 metadata protection, sector isolation, crossed-page record I/O, overwrite guard and last slot");
}

static void test_flash_manager_errors_and_debugger_tests(void)
{
    LogRecord_t input = phase2_record();
    reset_model();
    assert(FlashManager_Init() == FLASH_MANAGER_OK);
    stuck_busy = 1;
    assert(FlashManager_PrepareSector(0U) == FLASH_MANAGER_TIMEOUT);
    stuck_busy = 0;
    id = 0xFFFFFFU;
    assert(W25Q64_Init() == W25Q64_UNSUPPORTED_DEVICE);
    assert(FlashManager_Init() == FLASH_MANAGER_UNSUPPORTED_DEVICE);
    assert(FlashManager_WriteRecord(0U, 0U, &input) ==
           FLASH_MANAGER_NOT_INITIALIZED);

    reset_model();
    assert(FlashManagerTest_Codec() == FLASH_MANAGER_OK);
    assert(FlashManagerTest_RecordIO() == FLASH_MANAGER_OK);
    assert(FlashManagerTest_CrcDetection() == FLASH_MANAGER_OK);
    g_flash_manager_test_command = 99U;
    FlashManagerTest_Process();
    assert(g_flash_manager_test_result == FLASH_MANAGER_INVALID_PARAM);
    assert(g_flash_manager_test_done == 99U);
    puts("PASS Phase 2 flash error mapping and debugger mailbox tests");
}

static FlashManagerState_t empty_flash_state(uint32_t sector)
{
    FlashManagerState_t state;
    state.write_sector = sector;
    state.write_slot = 0U;
    state.oldest_sector = sector;
    state.oldest_slot = 0U;
    state.record_count = 0U;
    state.next_sequence = 0U;
    return state;
}

static void test_metadata_codec_and_redundancy(void)
{
    uint8_t encoded[FLASH_METADATA_SIZE];
    uint8_t erased[FLASH_METADATA_SIZE];
    FlashMetadata_t input;
    FlashMetadata_t output;
    FlashManagerState_t state;
    FlashManagerRecovery_t recovery;
    uint32_t generation;

    input.magic = FLASH_METADATA_MAGIC;
    input.version = FLASH_METADATA_VERSION;
    input.generation = 7U;
    input.write_sector = 5U;
    input.write_offset = 2U * FLASH_LOG_RECORD_SIZE;
    input.oldest_sector = 5U;
    input.record_count = 2U;
    input.next_sequence = 44U;
    input.crc32 = 0U;
    assert(FlashManager_SerializeMetadata(&input, encoded) == FLASH_MANAGER_OK);
    assert(encoded[0] == 'M' && encoded[1] == 'E' &&
           encoded[2] == 'T' && encoded[3] == 'A');
    assert(encoded[4] == 1U && encoded[5] == 0U &&
           encoded[8] == 7U && encoded[12] == 5U &&
           encoded[16] == 56U && encoded[20] == 5U &&
           encoded[24] == 2U && encoded[28] == 44U);
    assert(FlashManager_DeserializeMetadata(encoded, &output) ==
           FLASH_MANAGER_OK);
    assert(output.generation == 7U && output.write_sector == 5U &&
           output.write_offset == 56U && output.record_count == 2U &&
           output.next_sequence == 44U);
    encoded[0] ^= 1U;
    assert(FlashManager_DeserializeMetadata(encoded, &output) ==
           FLASH_MANAGER_CRC_ERROR);
    memset(erased, 0xFF, sizeof(erased));
    assert(FlashManager_DeserializeMetadata(erased, &output) ==
           FLASH_MANAGER_NO_METADATA);
    input.write_offset = 1U;
    assert(FlashManager_SerializeMetadata(&input, encoded) ==
           FLASH_MANAGER_INVALID_PARAM);

    reset_model();
    assert(FlashManager_Init() == FLASH_MANAGER_OK);
    assert(FlashManager_TestResetMetadata() == FLASH_MANAGER_OK);
    state = empty_flash_state(0U);
    assert(FlashManager_CommitState(&state, &generation) == FLASH_MANAGER_OK &&
           generation == 1U);
    assert(FlashManager_CommitState(&state, &generation) == FLASH_MANAGER_OK &&
           generation == 2U);
    assert(FlashManager_ReadMetadata(FLASH_METADATA_COPY_A, &output) ==
           FLASH_MANAGER_OK && output.generation == 1U);
    assert(FlashManager_ReadMetadata(FLASH_METADATA_COPY_B, &output) ==
           FLASH_MANAGER_OK && output.generation == 2U);
    assert(FlashManager_Init() == FLASH_MANAGER_OK);
    assert(FlashManager_Recover(&recovery) == FLASH_MANAGER_OK);
    assert(recovery.active_copy == FLASH_METADATA_COPY_B &&
           recovery.generation == 2U && recovery.metadata_valid);
    corrupt_model_byte_one_to_zero(METADATA_B_ADDR +
                                   FLASH_METADATA_CRC_OFFSET);
    assert(FlashManager_Init() == FLASH_MANAGER_OK);
    assert(FlashManager_Recover(&recovery) == FLASH_MANAGER_OK);
    assert(recovery.active_copy == FLASH_METADATA_COPY_A &&
           recovery.generation == 1U && recovery.metadata_valid);

    ignore_modify = 1U;
    assert(FlashManager_CommitState(&state, NULL) ==
           FLASH_MANAGER_FLASH_ERROR);
    ignore_modify = 0U;
    assert(FlashManager_Init() == FLASH_MANAGER_OK);
    assert(FlashManager_Recover(&recovery) == FLASH_MANAGER_OK);
    assert(recovery.active_copy == FLASH_METADATA_COPY_A &&
           recovery.generation == 1U);

    assert(FlashManager_CommitState(&state, &generation) == FLASH_MANAGER_OK &&
           generation == 2U);
    corrupt_model_byte_one_to_zero(METADATA_A_ADDR +
                                   FLASH_METADATA_CRC_OFFSET);
    assert(FlashManager_Init() == FLASH_MANAGER_OK);
    assert(FlashManager_Recover(&recovery) == FLASH_MANAGER_OK);
    assert(recovery.active_copy == FLASH_METADATA_COPY_B &&
           recovery.generation == 2U && recovery.metadata_valid);
    corrupt_model_byte_one_to_zero(METADATA_B_ADDR +
                                   FLASH_METADATA_CRC_OFFSET);
    assert(FlashManager_Init() == FLASH_MANAGER_OK);
    assert(FlashManager_Recover(&recovery) == FLASH_MANAGER_OK);
    assert(!recovery.metadata_valid && recovery.used_log_scan &&
           recovery.state.record_count == 0U);

    reset_model();
    assert(FlashManager_Init() == FLASH_MANAGER_OK);
    state = empty_flash_state(0U);
    input.magic = FLASH_METADATA_MAGIC;
    input.version = FLASH_METADATA_VERSION;
    input.write_sector = state.write_sector;
    input.write_offset = 0U;
    input.oldest_sector = state.oldest_sector;
    input.record_count = 0U;
    input.next_sequence = 0U;
    input.generation = UINT32_MAX;
    assert(FlashManager_SerializeMetadata(&input, encoded) == FLASH_MANAGER_OK);
    memcpy(memory + METADATA_A_ADDR, encoded, sizeof(encoded));
    input.generation = 0U;
    assert(FlashManager_SerializeMetadata(&input, encoded) == FLASH_MANAGER_OK);
    memcpy(memory + METADATA_B_ADDR, encoded, sizeof(encoded));
    assert(FlashManager_Recover(&recovery) == FLASH_MANAGER_OK);
    assert(recovery.active_copy == FLASH_METADATA_COPY_B &&
           recovery.generation == 0U);
    puts("PASS Phase 6 metadata codec, alternating A/B commit, fallback, failed update and generation wrap");
}

static void test_recovery_scan_and_corrupt_tail(void)
{
    FlashManagerState_t state;
    FlashManagerRecovery_t recovery;
    DataLoggerInfo_t info;
    LogRecord_t record;
    uint32_t sequence;
    uint32_t record_address;

    reset_model();
    assert(FlashManager_Init() == FLASH_MANAGER_OK);
    assert(FlashManager_TestResetMetadata() == FLASH_MANAGER_OK);
    assert(FlashManager_PrepareSector(0U) == FLASH_MANAGER_OK);
    state = empty_flash_state(0U);
    assert(FlashManager_CommitState(&state, NULL) == FLASH_MANAGER_OK);
    for (sequence = 0U; sequence < 4U; ++sequence)
    {
        record = recovery_record(sequence);
        assert(FlashManager_WriteRecord(0U, sequence, &record) ==
               FLASH_MANAGER_OK);
    }
    assert(FlashManager_GetRecordAddress(0U, 3U, &record_address) ==
           FLASH_MANAGER_OK);
    corrupt_model_byte_one_to_zero(record_address + 12U);
    assert(FlashManager_Init() == FLASH_MANAGER_OK);
    assert(FlashManager_Recover(&recovery) == FLASH_MANAGER_OK);
    assert(recovery.metadata_valid && recovery.used_log_scan &&
           recovery.has_gaps && recovery.active_copy == FLASH_METADATA_COPY_A);
    assert(recovery.state.oldest_sector == 0U &&
           recovery.state.write_sector == 1U &&
           recovery.state.write_slot == 0U &&
           recovery.state.record_count == 3U &&
           recovery.state.next_sequence == 3U &&
           recovery.last_timestamp_ms == 1002U);
    assert(FlashManager_FindRecordBySequence(&recovery.state, 2U, &record) ==
           FLASH_MANAGER_OK && record.timestamp_ms == 1002U);

    reset_sensor();
    logger_now = 0U;
    set_sensor_sample(700);
    assert(DataLogger_Init() == LOGGER_OK);
    assert(DataLogger_Start() == LOGGER_OK);
    logger_now = LOGGER_SAMPLE_PERIOD_MS;
    assert(DataLogger_Process() == LOGGER_OK);
    assert(DataLogger_GetStatus(&info) == LOGGER_OK);
    assert(info.current_sector == 1U && info.current_slot == 1U &&
           info.record_count == 4U && info.next_sequence == 4U);
    assert(DataLogger_ReadRecord(0U, &record) == LOGGER_OK &&
           record.sequence == 0U);
    assert(DataLogger_ReadRecord(3U, &record) == LOGGER_OK &&
           record.sequence == 3U && record.accel_x == 700);
    state.write_sector = info.current_sector;
    state.write_slot = info.current_slot;
    state.oldest_sector = info.oldest_sector;
    state.oldest_slot = info.oldest_slot;
    state.record_count = info.record_count;
    state.next_sequence = info.next_sequence;
    assert(FlashManager_CommitState(&state, NULL) == FLASH_MANAGER_OK);
    assert(FlashManager_Init() == FLASH_MANAGER_OK);
    assert(FlashManager_Recover(&recovery) == FLASH_MANAGER_OK);
    assert(recovery.metadata_valid && recovery.has_gaps &&
           recovery.state.write_sector == 1U &&
           recovery.state.write_slot == 1U &&
           recovery.state.record_count == 4U &&
           recovery.state.next_sequence == 4U);
    assert(FlashManager_FindRecordBySequence(&recovery.state, 3U, &record) ==
           FLASH_MANAGER_OK && record.accel_x == 700);

    reset_model();
    assert(FlashManager_Init() == FLASH_MANAGER_OK);
    assert(FlashManager_PrepareSector(2U) == FLASH_MANAGER_OK);
    for (sequence = 10U; sequence < 13U; ++sequence)
    {
        record = recovery_record(sequence);
        assert(FlashManager_WriteRecord(2U, sequence - 10U, &record) ==
               FLASH_MANAGER_OK);
    }
    assert(FlashManager_Recover(&recovery) == FLASH_MANAGER_OK);
    assert(!recovery.metadata_valid && recovery.used_log_scan &&
           !recovery.has_gaps && recovery.state.oldest_sector == 2U &&
           recovery.state.write_sector == 2U &&
           recovery.state.write_slot == 3U &&
           recovery.state.record_count == 3U &&
           recovery.state.next_sequence == 13U &&
           recovery.last_timestamp_ms == 1012U);
    puts("PASS Phase 6 metadata-tail replay, corrupt-tail abandonment, gap-aware reads and full-scan fallback");
}

static void test_reclaim_commits_before_erase(void)
{
    uint8_t encoded[FLASH_METADATA_SIZE];
    FlashMetadata_t metadata;
    FlashManagerRecovery_t recovery;
    LogRecord_t record;

    reset_logger_environment();
    assert(DataLogger_TestSetupReclaim() == LOGGER_OK);
    assert(FlashManager_GetLastRecovery(&recovery) == FLASH_MANAGER_OK);
    assert(recovery.active_copy == FLASH_METADATA_COPY_A &&
           recovery.generation == 1U && recovery.state.record_count == 3U);
    assert(DataLogger_TestPrepareWriteSector() == LOGGER_OK);
    assert(FlashManager_GetLastRecovery(&recovery) == FLASH_MANAGER_OK);
    assert(recovery.active_copy == FLASH_METADATA_COPY_B &&
           recovery.generation == 2U && recovery.state.record_count == 0U);
    assert(FlashManager_ReadRecord(0U, 0U, &record) == FLASH_MANAGER_EMPTY);

    assert(DataLogger_TestSetupReclaim() == LOGGER_OK);
    metadata.magic = FLASH_METADATA_MAGIC;
    metadata.version = FLASH_METADATA_VERSION;
    metadata.generation = 2U;
    metadata.write_sector = 0U;
    metadata.write_offset = 0U;
    metadata.oldest_sector = 0U;
    metadata.record_count = 0U;
    metadata.next_sequence = 103U;
    metadata.crc32 = 0U;
    assert(FlashManager_SerializeMetadata(&metadata, encoded) ==
           FLASH_MANAGER_OK);
    memcpy(memory + METADATA_B_ADDR, encoded, sizeof(encoded));
    ignore_modify = 1U;
    assert(DataLogger_TestPrepareWriteSector() == LOGGER_FLASH_ERROR);
    ignore_modify = 0U;
    assert(memory[LOG_START_ADDR] != 0xFFU);
    assert(FlashManager_Init() == FLASH_MANAGER_OK);
    assert(FlashManager_Recover(&recovery) == FLASH_MANAGER_OK);
    assert(recovery.active_copy == FLASH_METADATA_COPY_B &&
           recovery.generation == 2U && recovery.state.record_count == 0U &&
           recovery.state.next_sequence == 103U);
    puts("PASS Phase 6 reclaim metadata is durable before erase, including erase-failure recovery");
}

static void test_mpu6050_driver(void)
{
    MPU6050_Sample_t sample;
    uint8_t device_id;
    reset_sensor();
    sensor_port_init_result = MPU6050_TIMEOUT;
    assert(MPU6050_Init() == MPU6050_TIMEOUT);
    assert(MPU6050_ReadSample(&sample) == MPU6050_NOT_INITIALIZED);

    reset_sensor();
    sensor_registers[0x75U] = 0x69U;
    assert(MPU6050_Init() == MPU6050_UNSUPPORTED_DEVICE);
    reset_sensor();
    sensor_ignore_write = 1;
    assert(MPU6050_Init() == MPU6050_VERIFY_ERROR);

    reset_sensor();
    assert(MPU6050_Init() == MPU6050_OK);
    assert(sensor_registers[0x6BU] == 0x01U);
    assert(sensor_registers[0x6CU] == 0x00U);
    assert(sensor_registers[0x19U] == 0x09U);
    assert(sensor_registers[0x1AU] == 0x06U);
    assert(sensor_registers[0x1BU] == 0x18U);
    assert(sensor_registers[0x1CU] == 0x18U);
    assert(MPU6050_ReadDeviceId(&device_id) == MPU6050_OK);
    assert(device_id == MPU6050_EXPECTED_WHO_AM_I);
    assert(MPU6050_ReadDeviceId(NULL) == MPU6050_INVALID_PARAM);
    assert(MPU6050_ReadSample(NULL) == MPU6050_INVALID_PARAM);
    set_sensor_sample(-30000);
    assert(MPU6050_ReadSample(&sample) == MPU6050_OK);
    assert(sample.accel_x == -30000 && sample.accel_y == -29999);
    assert(sample.accel_z == -29998 && sample.gyro_x == -29997);
    assert(sample.gyro_y == -29996 && sample.gyro_z == -29995);
    assert(sensor_burst_reads == 1U);
    sensor_read_result = MPU6050_TIMEOUT;
    assert(MPU6050_ReadSample(&sample) == MPU6050_TIMEOUT);
    puts("PASS Phase 3 MPU6050 identity/config verification, signed decode, burst read and errors");
}

static void reset_logger_environment(void)
{
    reset_model();
    reset_sensor();
    logger_now = 0U;
    assert(DataLogger_Init() == LOGGER_OK);
}

static void test_wear_leveling_policy(void)
{
    WearLevelingStats_t stats;
    uint32_t sector;
    uint32_t count;
    reset_model();
    assert(FlashManager_Init() == FLASH_MANAGER_OK);
    assert(WearLeveling_GetStats(&stats) == WEAR_LEVELING_NOT_INITIALIZED);
    assert(WearLeveling_Init() == WEAR_LEVELING_OK);
    assert(WearLeveling_GetStats(&stats) == WEAR_LEVELING_OK);
    assert(stats.total_erases == 0U && stats.minimum_erase_count == 0U);
    assert(stats.maximum_erase_count == 0U && stats.erase_count_spread == 0U);
    assert(stats.sectors_at_minimum == FLASH_LOG_SECTOR_COUNT);
    assert(stats.sectors_at_maximum == FLASH_LOG_SECTOR_COUNT);
    assert(stats.next_sector == 0U);
    assert(WearLeveling_PrepareNextSector(NULL) ==
           WEAR_LEVELING_INVALID_PARAM);
    assert(WearLeveling_GetSectorEraseCount(FLASH_LOG_SECTOR_COUNT, &count) ==
           WEAR_LEVELING_INVALID_PARAM);
    assert(WearLeveling_GetNextSector(FLASH_LOG_SECTOR_COUNT - 1U, &sector) ==
           WEAR_LEVELING_OK && sector == 0U);

    stuck_busy = 1;
    assert(WearLeveling_PrepareNextSector(&sector) == WEAR_LEVELING_TIMEOUT);
    stuck_busy = 0;
    assert(WearLeveling_GetStats(&stats) == WEAR_LEVELING_OK);
    assert(stats.total_erases == 0U && stats.next_sector == 0U);
    assert(WearLeveling_PrepareNextSector(&sector) == WEAR_LEVELING_OK);
    assert(sector == 0U);
    assert(WearLeveling_GetSectorEraseCount(0U, &count) == WEAR_LEVELING_OK &&
           count == 1U);
    assert(WearLeveling_GetStats(&stats) == WEAR_LEVELING_OK);
    assert(stats.total_erases == 1U && stats.next_sector == 1U);
    assert(stats.maximum_erase_count == 1U && stats.minimum_erase_count == 0U);

    assert(WearLeveling_TestSimulate(3U, 17U) == WEAR_LEVELING_OK);
    assert(WearLeveling_GetStats(&stats) == WEAR_LEVELING_OK);
    assert(stats.total_erases == 6155U);
    assert(stats.minimum_erase_count == 3U &&
           stats.maximum_erase_count == 4U && stats.erase_count_spread == 1U);
    assert(stats.sectors_at_maximum == 17U);
    assert(stats.sectors_at_minimum == FLASH_LOG_SECTOR_COUNT - 17U);
    assert(stats.next_sector == 17U);
    assert(WearLeveling_GetSectorEraseCount(0U, &count) == WEAR_LEVELING_OK &&
           count == 4U);
    assert(WearLeveling_GetSectorEraseCount(16U, &count) == WEAR_LEVELING_OK &&
           count == 4U);
    assert(WearLeveling_GetSectorEraseCount(17U, &count) == WEAR_LEVELING_OK &&
           count == 3U);
    assert(WearLeveling_GetSectorEraseCount(FLASH_LOG_SECTOR_COUNT - 1U,
                                             &count) == WEAR_LEVELING_OK &&
           count == 3U);
    assert(WearLeveling_ValidateDistribution(1U) == WEAR_LEVELING_OK);
    assert(WearLeveling_ValidateDistribution(0U) == WEAR_LEVELING_IMBALANCED);
    assert(WearLeveling_TestSimulate(0U, FLASH_LOG_SECTOR_COUNT + 1U) ==
           WEAR_LEVELING_INVALID_PARAM);
    assert(WearLeveling_TestSimulate(UINT32_MAX, 0U) ==
           WEAR_LEVELING_INVALID_PARAM);
    puts("PASS Phase 5 round-robin allocator, successful-erase accounting and RAM distribution statistics");
}

static void assert_logger_record(uint32_t index, uint32_t sequence,
                                 uint32_t timestamp, int16_t base)
{
    LogRecord_t record;
    assert(DataLogger_ReadRecord(index, &record) == LOGGER_OK);
    assert(record.magic == FLASH_LOG_RECORD_MAGIC);
    assert(record.sequence == sequence && record.timestamp_ms == timestamp);
    assert(record.accel_x == base && record.accel_y == (int16_t)(base + 1));
    assert(record.accel_z == (int16_t)(base + 2));
    assert(record.gyro_x == (int16_t)(base + 3));
    assert(record.gyro_y == (int16_t)(base + 4));
    assert(record.gyro_z == (int16_t)(base + 5));
}

static void test_data_logger_timing_and_records(void)
{
    DataLoggerInfo_t info;
    uint32_t before_sectors;
    reset_logger_environment();
    assert(sectors == 0U); /* Init is read-only for flash. */
    /* Phase 6 recovery makes an empty, scanned flash immediately startable. */
    assert(DataLogger_Start() == LOGGER_OK);
    assert(DataLogger_Stop() == LOGGER_OK);
    before_sectors = sectors;
    /* One log-sector erase plus both metadata copies. */
    assert(DataLogger_Clear() == LOGGER_OK && sectors == before_sectors + 3U);
    set_sensor_sample(100);
    logger_now = 1000U;
    assert(DataLogger_Start() == LOGGER_OK);
    logger_now = 1099U;
    assert(DataLogger_Process() == LOGGER_OK);
    assert(DataLogger_GetStatus(&info) == LOGGER_OK && info.record_count == 0U);
    logger_now = 1100U;
    assert(DataLogger_Process() == LOGGER_OK);
    assert_logger_record(0U, 0U, 1100U, 100);

    set_sensor_sample(200);
    logger_now = 1350U;
    assert(DataLogger_Process() == LOGGER_OK); /* One sample, no catch-up burst. */
    assert(DataLogger_Process() == LOGGER_OK);
    assert(DataLogger_GetStatus(&info) == LOGGER_OK && info.record_count == 2U);
    assert_logger_record(1U, 1U, 1350U, 200);
    assert(DataLogger_ReadRecord(2U, (LogRecord_t *)&info) == LOGGER_INVALID_PARAM);
    assert(DataLogger_ReadRecord(0U, NULL) == LOGGER_INVALID_PARAM);
    assert(DataLogger_Stop() == LOGGER_OK);
    logger_now = 2000U;
    assert(DataLogger_Process() == LOGGER_OK);
    assert(DataLogger_GetStatus(&info) == LOGGER_OK &&
           info.state == DATA_LOGGER_STOPPED && info.record_count == 2U);
    puts("PASS Phase 3 explicit start, 10 Hz deadline, no catch-up burst and exact records");
}

static void test_data_logger_wrap_sector_and_errors(void)
{
    DataLoggerInfo_t info;
    WearLevelingStats_t wear;
    uint32_t index;
    reset_logger_environment();
    assert(DataLogger_Clear() == LOGGER_OK);
    set_sensor_sample(-10);
    logger_now = UINT32_MAX - 50U;
    assert(DataLogger_Start() == LOGGER_OK);
    logger_now = UINT32_MAX;
    assert(DataLogger_Process() == LOGGER_OK);
    assert(DataLogger_GetStatus(&info) == LOGGER_OK && info.record_count == 0U);
    logger_now = 49U;
    assert(DataLogger_Process() == LOGGER_OK);
    assert(DataLogger_GetStatus(&info) == LOGGER_OK && info.record_count == 1U);
    assert_logger_record(0U, 0U, 49U, -10);

    assert(DataLogger_Clear() == LOGGER_OK);
    logger_now = 0U;
    assert(DataLogger_Start() == LOGGER_OK);
    for (index = 0U; index < FLASH_RECORDS_PER_SECTOR + 1U; ++index)
    {
        logger_now += LOGGER_SAMPLE_PERIOD_MS;
        assert(DataLogger_Process() == LOGGER_OK);
    }
    assert(DataLogger_GetStatus(&info) == LOGGER_OK);
    assert(info.record_count == FLASH_RECORDS_PER_SECTOR + 1U);
    assert(info.current_sector == 2U && info.current_slot == 1U);
    assert(WearLeveling_GetStats(&wear) == WEAR_LEVELING_OK);
    assert(wear.total_erases == 3U && wear.next_sector == 3U);
    assert(wear.minimum_erase_count == 0U &&
           wear.maximum_erase_count == 1U && wear.erase_count_spread == 1U);
    assert_logger_record(FLASH_RECORDS_PER_SECTOR,
                         FLASH_RECORDS_PER_SECTOR,
                         (FLASH_RECORDS_PER_SECTOR + 1U) * LOGGER_SAMPLE_PERIOD_MS,
                         -10);

    assert(DataLogger_Clear() == LOGGER_OK);
    assert(DataLogger_Start() == LOGGER_OK);
    sensor_read_result = MPU6050_TIMEOUT;
    logger_now += LOGGER_SAMPLE_PERIOD_MS;
    assert(DataLogger_Process() == LOGGER_TIMEOUT);
    assert(DataLogger_GetStatus(&info) == LOGGER_OK);
    assert(info.state == DATA_LOGGER_STOPPED && info.last_error == LOGGER_TIMEOUT);

    sensor_read_result = MPU6050_OK;
    assert(DataLogger_Clear() == LOGGER_OK);
    assert(DataLogger_Start() == LOGGER_OK);
    stuck_busy = 1;
    logger_now += LOGGER_SAMPLE_PERIOD_MS;
    assert(DataLogger_Process() == LOGGER_TIMEOUT);
    stuck_busy = 0;
    assert(DataLogger_GetStatus(&info) == LOGGER_OK);
    assert(info.state == DATA_LOGGER_STOPPED && info.last_error == LOGGER_TIMEOUT);
    puts("PASS Phase 3 timestamp wrap, sector transition and fail-stop sensor/flash errors");
}

static void test_data_logger_circular_wrap(void)
{
    DataLoggerInfo_t info;
    LogRecord_t record;
    reset_logger_environment();
    set_sensor_sample(-123);
    logger_now = 500U;
    assert(DataLogger_TestCircularWrap() == LOGGER_OK);
    assert(DataLogger_GetStatus(&info) == LOGGER_OK);
    assert(info.state == DATA_LOGGER_STOPPED);
    assert(info.current_sector == 0U && info.current_slot == 1U);
    assert(info.oldest_sector == 1U && info.oldest_slot == 0U);
    assert(info.write_address == LOG_START_ADDR + FLASH_LOG_RECORD_SIZE);
    assert(info.oldest_address == LOG_START_ADDR + FLASH_SECTOR_SIZE);
    assert(info.record_count == FLASH_LOG_RECORD_CAPACITY -
                                FLASH_RECORDS_PER_SECTOR + 1U);
    assert(info.next_sequence == FLASH_LOG_RECORD_CAPACITY + 1U);
    assert(DataLogger_ReadRecord(0U, &record) == LOGGER_OK);
    assert(record.sequence == FLASH_RECORDS_PER_SECTOR);
    assert(DataLogger_ReadRecord(info.record_count - 1U, &record) == LOGGER_OK);
    assert(record.sequence == FLASH_LOG_RECORD_CAPACITY);
    assert(record.timestamp_ms == 501U && record.accel_x == -123);
    assert(FlashManager_ReadRecord(0U, 1U, &record) == FLASH_MANAGER_EMPTY);
    puts("PASS Phase 4 sector erase before wrap, bounded count, oldest/write pointers and chronological read");
}

static void test_data_logger_debugger_mailbox(void)
{
    reset_logger_environment();
    set_sensor_sample(321);
    assert(DataLoggerTest_Sensor() == LOGGER_OK);
    assert(g_data_logger_test_device_id == MPU6050_EXPECTED_WHO_AM_I);
    assert(DataLoggerTest_Start() == LOGGER_OK);
    logger_now += LOGGER_SAMPLE_PERIOD_MS;
    assert(DataLogger_Process() == LOGGER_OK);
    assert(DataLoggerTest_StopAndVerify() == LOGGER_OK);
    assert(g_data_logger_test_value == 1U);
    assert(g_data_logger_test_record.accel_x == 321);
    g_data_logger_test_command = DATA_LOGGER_TEST_CIRCULAR_WRAP;
    DataLoggerTest_Process();
    assert(g_data_logger_test_done == DATA_LOGGER_TEST_CIRCULAR_WRAP);
    assert(g_data_logger_test_result == LOGGER_OK);
    assert(g_data_logger_test_value == FLASH_LOG_RECORD_CAPACITY -
                                       FLASH_RECORDS_PER_SECTOR + 1U);
    g_wear_test_query_sector = 16U;
    g_data_logger_test_command = DATA_LOGGER_TEST_WEAR_DISTRIBUTION;
    DataLoggerTest_Process();
    assert(g_data_logger_test_done == DATA_LOGGER_TEST_WEAR_DISTRIBUTION);
    assert(g_data_logger_test_result == LOGGER_OK);
    assert(g_data_logger_test_value == 1U && g_wear_test_query_count == 4U);
    g_wear_test_query_sector = 17U;
    DataLoggerTest_Process();
    assert(g_wear_test_query_count == 3U);
    g_data_logger_test_command = DATA_LOGGER_TEST_METADATA_REDUNDANCY;
    DataLoggerTest_Process();
    assert(g_data_logger_test_done == DATA_LOGGER_TEST_METADATA_REDUNDANCY);
    assert(g_data_logger_test_result == LOGGER_OK &&
           g_metadata_test_active_copy == FLASH_METADATA_COPY_A &&
           g_metadata_test_generation == 1U && g_metadata_test_valid);
    g_data_logger_test_command = DATA_LOGGER_TEST_TAIL_RECOVERY;
    DataLoggerTest_Process();
    assert(g_data_logger_test_done == DATA_LOGGER_TEST_TAIL_RECOVERY);
    assert(g_data_logger_test_result == LOGGER_OK &&
           g_metadata_test_write_sector == 1U &&
           g_metadata_test_write_slot == 0U &&
           g_metadata_test_record_count == 3U &&
           g_metadata_test_next_sequence == 3U &&
           g_metadata_test_used_log_scan && g_metadata_test_has_gaps);
    g_data_logger_test_command = DATA_LOGGER_TEST_POWER_CYCLE_PREPARE;
    DataLoggerTest_Process();
    assert(g_data_logger_test_result == LOGGER_OK);
    g_data_logger_test_command = DATA_LOGGER_TEST_POWER_CYCLE_VERIFY;
    DataLoggerTest_Process();
    assert(g_data_logger_test_done == DATA_LOGGER_TEST_POWER_CYCLE_VERIFY &&
           g_data_logger_test_result == LOGGER_OK);
    g_data_logger_test_command = 99U;
    DataLoggerTest_Process();
    assert(g_data_logger_test_result == LOGGER_INVALID_PARAM);
    assert(g_data_logger_test_done == 99U);
    puts("PASS Phase 3-6 debugger mailbox sensor, record, wrap, wear, metadata and recovery verification");
}

static void test_cli_commands_and_parser(void)
{
    DataLoggerInfo_t info;
    LogRecord_t record;
    uint32_t index;
    char long_line[100];

    reset_logger_environment();
    uart_init_result = UART_PORT_NOT_CONFIGURED;
    assert(CLI_Init() == CLI_UART_ERROR);
    uart_init_result = UART_PORT_OK;
    reset_uart_output();
    assert(CLI_Init() == CLI_OK);
    assert(strstr(uart_output, "STM32 W25Q64 Data Logger") != NULL);
    assert(strstr(uart_output, "JEDEC ID: 0xEF4017") != NULL);
    assert(CLI_TestExecuteLine(NULL) == CLI_INVALID_PARAM);

    reset_uart_output();
    assert(CLI_TestExecuteLine("help") == CLI_OK);
    assert(strstr(uart_output, "flash read <address>") != NULL);
    assert(CLI_TestExecuteLine("flash id") == CLI_OK);
    assert(strstr(uart_output, "Manufacturer: Winbond") != NULL);
    assert(CLI_TestExecuteLine("flash status") == CLI_OK);
    assert(strstr(uart_output, "SR1: 0x00") != NULL);
    assert(CLI_TestExecuteLine("metadata") == CLI_OK);
    assert(strstr(uart_output, "Active metadata: NONE") != NULL);
    assert(CLI_TestExecuteLine("flash read 0x7fffff 1") == CLI_OK);
    assert(strstr(uart_output, "0x7FFFFF: FF") != NULL);
    assert(CLI_TestExecuteLine("flash read 0x7fffff 2") == CLI_OK);
    assert(strstr(uart_output, "address or length out of range") != NULL);
    assert(CLI_TestExecuteLine("flash read xyz 1") == CLI_OK);
    assert(strstr(uart_output, "invalid numeric argument") != NULL);
    assert(CLI_TestExecuteLine("unknown") == CLI_OK);
    assert(strstr(uart_output, "invalid command") != NULL);

    reset_uart_output();
    assert(CLI_TestExecuteLine("log clear") == CLI_OK);
    assert(strstr(uart_output, "Log cleared") != NULL);
    set_sensor_sample(50);
    logger_now = 0U;
    assert(CLI_TestExecuteLine("log start") == CLI_OK);
    for (index = 0U; index < 3U; ++index)
    {
        logger_now += LOGGER_SAMPLE_PERIOD_MS;
        assert(DataLogger_Process() == LOGGER_OK);
    }
    assert(CLI_TestExecuteLine("log count") == CLI_OK);
    assert(strstr(uart_output, "Records: 3") != NULL);
    assert(CLI_TestExecuteLine("log read 2") == CLI_OK);
    assert(strstr(uart_output, "stop logger") != NULL);
    assert(CLI_TestExecuteLine("log stop") == CLI_OK);
    assert(CLI_TestExecuteLine("log status") == CLI_OK);
    assert(strstr(uart_output, "Logger: STOPPED") != NULL);
    reset_uart_output();
    assert(CLI_TestExecuteLine("log read 2") == CLI_OK);
    assert(CLI_Process() == CLI_OK);
    assert(CLI_Process() == CLI_OK);
    assert(strstr(uart_output, "1 200 50 51 52 53 54 55") != NULL);
    assert(strstr(uart_output, "2 300 50 51 52 53 54 55") != NULL);

    reset_uart_output();
    assert(CLI_TestExecuteLine("log readall") == CLI_OK);
    assert(CLI_Process() == CLI_OK);
    assert(CLI_Process() == CLI_OK);
    assert(CLI_Process() == CLI_OK);
    assert(strstr(uart_output, "0 100 50 51 52 53 54 55") != NULL);
    assert(strstr(uart_output, "2 300 50 51 52 53 54 55") != NULL);

    reset_uart_output();
    assert(CLI_TestExecuteLine("log verify") == CLI_OK);
    assert(CLI_TestExecuteLine("help") == CLI_OK);
    assert(strstr(uart_output, "command busy") != NULL);
    assert(CLI_Process() == CLI_OK);
    assert(CLI_Process() == CLI_OK);
    assert(CLI_Process() == CLI_OK);
    assert(strstr(uart_output, "Records checked: 3") != NULL);
    assert(strstr(uart_output, "Valid: 3") != NULL);
    assert(strstr(uart_output, "CRC errors: 0") != NULL);
    assert(strstr(uart_output, "Sequence errors: 0") != NULL);

    reset_uart_output();
    assert(CLI_TestExecuteLine("flash erase 0") == CLI_OK);
    assert(strstr(uart_output, "log clear required") != NULL);
    assert(CLI_TestExecuteLine("log clear") == CLI_OK);
    assert(CLI_TestExecuteLine("flash erase 5") == CLI_OK);
    assert(strstr(uart_output, "Sector erased") != NULL);
    assert(CLI_TestExecuteLine("flash erase 2046") == CLI_OK);
    assert(strstr(uart_output, "outside log area") != NULL);

    reset_uart_output();
    feed_uart("log count\r\n");
    assert(CLI_Process() == CLI_OK);
    assert(strstr(uart_output, "Records: 0") != NULL);
    memset(long_line, 'a', sizeof(long_line) - 2U);
    long_line[sizeof(long_line) - 2U] = '\r';
    long_line[sizeof(long_line) - 1U] = '\0';
    feed_uart(long_line);
    for (index = 0U; index < 7U; ++index) { assert(CLI_Process() == CLI_OK); }
    assert(strstr(uart_output, "command line too long") != NULL);

    assert(CLI_TestExecuteLine("log clear") == CLI_OK);
    assert(CLI_TestExecuteLine("log start") == CLI_OK);
    logger_now += LOGGER_SAMPLE_PERIOD_MS;
    assert(DataLogger_Process() == LOGGER_OK);
    assert(CLI_TestExecuteLine("log stop") == CLI_OK);
    assert(DataLogger_ReadRecord(0U, &record) == LOGGER_OK);
    assert(DataLogger_GetStatus(&info) == LOGGER_OK);
    corrupt_model_byte_one_to_zero(LOG_START_ADDR +
                                   info.oldest_sector * FLASH_SECTOR_SIZE +
                                   12U);
    reset_uart_output();
    assert(CLI_TestExecuteLine("log verify") == CLI_OK);
    assert(CLI_Process() == CLI_OK);
    assert(strstr(uart_output, "Record 0: CRC mismatch") != NULL);
    assert(strstr(uart_output, "Records checked: 1") != NULL);
    assert(strstr(uart_output, "Valid: 0") != NULL);
    assert(strstr(uart_output, "CRC errors: 1") != NULL);

    uart_write_result = UART_PORT_TIMEOUT;
    assert(CLI_TestExecuteLine("help") == CLI_UART_ERROR);
    uart_write_result = UART_PORT_OK;
    puts("PASS Phase 7 CLI banner, parsing, range checks, safe erase, log control, incremental dump and CRC verify");
}

static void test_phase8_benchmark_and_wear_diagnostics(void)
{
    static const uint32_t times[] = {100U, 140U, 200U, 216U, 300U, 305U};
    static const uint32_t zero_elapsed[] = {500U, 500U};
    FlashBenchmarkResult_t result;
    DataLoggerInfo_t info;
    LogRecord_t record;
    uint32_t index;
    uint32_t programs_before;
    uint32_t sectors_before;

    reset_logger_environment();
    assert(FlashBenchmark_Run(0U, NULL) == FLASH_BENCHMARK_INVALID_PARAM);
    assert(FlashBenchmark_Run(FLASH_LOG_SECTOR_COUNT, &result) ==
           FLASH_BENCHMARK_INVALID_PARAM);
    assert(DataLogger_Clear() == LOGGER_OK);
    assert(DataLogger_GetStatus(&info) == LOGGER_OK);
    programs_before = programs;
    sectors_before = sectors;
    set_system_time_script(times, 6U);
    assert(FlashBenchmark_Run(info.current_sector, &result) ==
           FLASH_BENCHMARK_OK);
    assert(result.sector_index == info.current_sector);
    assert(result.address == LOG_START_ADDR +
           info.current_sector * FLASH_SECTOR_SIZE);
    assert(result.spi_clock_hz == 9000000U);
    assert(result.erase_time_ms == 40U);
    assert(result.write_time_ms == 16U &&
           result.write_speed_kib_per_s == 250U);
    assert(result.read_time_ms == 5U &&
           result.read_speed_kib_per_s == 800U);
    assert(programs == programs_before + FLASH_BENCHMARK_SIZE / FLASH_PAGE_SIZE);
    assert(sectors == sectors_before + 2U);
    for (index = 0U; index < FLASH_SECTOR_SIZE; ++index)
    {
        assert(memory[result.address + index] == 0xFFU);
    }
    set_system_time_script(zero_elapsed, 2U);
    assert(FlashBenchmark_Run(info.current_sector, &result) ==
           FLASH_BENCHMARK_TIMER_ERROR);
    for (index = 0U; index < FLASH_SECTOR_SIZE; ++index)
    {
        assert(memory[LOG_START_ADDR +
                      info.current_sector * FLASH_SECTOR_SIZE + index] == 0xFFU);
    }

    reset_uart_output();
    assert(CLI_Init() == CLI_OK);
    reset_uart_output();
    set_system_time_script(times, 6U);
    assert(CLI_TestExecuteLine("flash benchmark") == CLI_OK);
    assert(strstr(uart_output, "W25Q64 Benchmark") != NULL);
    assert(strstr(uart_output, "SPI clock: 9000000 Hz") != NULL);
    assert(strstr(uart_output, "Time: 5 ms\r\nSpeed: 800 KiB/s") != NULL);
    assert(strstr(uart_output, "Time: 16 ms\r\nSpeed: 250 KiB/s") != NULL);
    assert(strstr(uart_output, "Time: 40 ms") != NULL);
    assert(strstr(uart_output, "Cleanup: erased and verified") != NULL);

    reset_uart_output();
    assert(CLI_TestExecuteLine("wear status") == CLI_OK);
    assert(strstr(uart_output, "Wear counters: logger-managed RAM only") != NULL);
    assert(strstr(uart_output, "Sector Erase count") != NULL);
    assert(CLI_TestExecuteLine("wear status 2045 2") == CLI_OK);
    assert(strstr(uart_output, "2045 0") != NULL);
    assert(CLI_TestExecuteLine("wear status 2046 1") == CLI_OK);
    assert(strstr(uart_output, "invalid wear display range") != NULL);
    assert(CLI_TestExecuteLine("wear status 0 17") == CLI_OK);
    assert(strstr(uart_output, "invalid wear display range") != NULL);

    set_sensor_sample(70);
    logger_now = 0U;
    assert(DataLogger_Start() == LOGGER_OK);
    reset_uart_output();
    assert(CLI_TestExecuteLine("flash benchmark") == CLI_OK);
    assert(strstr(uart_output, "stop logger before benchmark") != NULL);
    logger_now += LOGGER_SAMPLE_PERIOD_MS;
    assert(DataLogger_Process() == LOGGER_OK);
    assert(DataLogger_Stop() == LOGGER_OK);
    assert(DataLogger_ReadRecord(0U, &record) == LOGGER_OK);
    reset_uart_output();
    assert(CLI_TestExecuteLine("flash benchmark") == CLI_OK);
    assert(strstr(uart_output, "log clear required before benchmark") != NULL);
    puts("PASS Phase 8 real-timer benchmark model, cleanup, data verification, CLI safety and wear diagnostics");
}

static void arm_power_fail_test(PowerFailTestPoint_t point)
{
    g_power_fail_test_host_reset = 0U;
    g_power_fail_test_point = (uint32_t)point;
}

static void simulate_flash_manager_reset(FlashManagerRecovery_t *recovery)
{
    assert(recovery != NULL);
    assert(W25Q64_Init() == W25Q64_OK);
    assert(FlashManager_Init() == FLASH_MANAGER_OK);
    assert(FlashManager_Recover(recovery) == FLASH_MANAGER_OK);
}

static void test_debug_power_fail_reset_injection(void)
{
    FlashManagerRecovery_t before;
    FlashManagerRecovery_t after;
    DataLoggerInfo_t info;
    LogRecord_t record;
    PowerFailTestPoint_t metadata_points[3];
    uint32_t point_index;

    metadata_points[0] = POWER_FAIL_POINT_METADATA_ERASED;
    metadata_points[1] = POWER_FAIL_POINT_METADATA_PARTIAL_PROGRAMMED;
    metadata_points[2] = POWER_FAIL_POINT_METADATA_PROGRAMMED;

    reset_logger_environment();
    assert(DataLogger_Clear() == LOGGER_OK);
    set_sensor_sample(90);
    logger_now = 0U;
    assert(DataLogger_Start() == LOGGER_OK);
    arm_power_fail_test(POWER_FAIL_POINT_RECORD_PROGRAMMED);
    logger_now += LOGGER_SAMPLE_PERIOD_MS;
    assert(DataLogger_Process() == LOGGER_ERROR);
    assert(g_power_fail_test_host_reset == 1U);
    assert(g_power_fail_test_point == POWER_FAIL_POINT_NONE);
    assert(g_power_fail_test_last_point ==
           POWER_FAIL_POINT_RECORD_PROGRAMMED);
    assert(DataLogger_GetStatus(&info) == LOGGER_OK &&
           info.record_count == 0U);
    assert(W25Q64_Init() == W25Q64_OK);
    assert(DataLogger_Init() == LOGGER_OK);
    assert(DataLogger_GetStatus(&info) == LOGGER_OK &&
           info.record_count == 1U && info.next_sequence == 1U);
    assert(DataLogger_ReadRecord(0U, &record) == LOGGER_OK &&
           record.sequence == 0U);

    for (point_index = 0U; point_index < 3U; ++point_index)
    {
        reset_logger_environment();
        assert(DataLogger_Clear() == LOGGER_OK);
        assert(FlashManager_GetLastRecovery(&before) == FLASH_MANAGER_OK);
        arm_power_fail_test(metadata_points[point_index]);
        assert(FlashManager_CommitState(&before.state, NULL) ==
               FLASH_MANAGER_RECOVERY_ERROR);
        assert(g_power_fail_test_host_reset == 1U);
        assert(g_power_fail_test_last_point ==
               (uint32_t)metadata_points[point_index]);
        simulate_flash_manager_reset(&after);
        if (metadata_points[point_index] ==
            POWER_FAIL_POINT_METADATA_PROGRAMMED)
        {
            assert(after.generation == before.generation + 1U);
            assert(after.active_copy != before.active_copy);
        }
        else
        {
            assert(after.generation == before.generation);
            assert(after.active_copy == before.active_copy);
        }
        assert(after.metadata_valid == 1U);
        assert(after.state.write_sector == before.state.write_sector);
        assert(after.state.record_count == before.state.record_count);
        assert(after.state.next_sequence == before.state.next_sequence);
    }

    reset_logger_environment();
    assert(DataLogger_Clear() == LOGGER_OK);
    assert(DataLogger_GetStatus(&info) == LOGGER_OK);
    arm_power_fail_test(POWER_FAIL_POINT_LOG_SECTOR_ERASED);
    assert(DataLogger_Clear() == LOGGER_WEAR_LEVELING_ERROR);
    assert(g_power_fail_test_host_reset == 1U);
    assert(g_power_fail_test_last_point ==
           POWER_FAIL_POINT_LOG_SECTOR_ERASED);
    assert(W25Q64_Init() == W25Q64_OK);
    assert(DataLogger_Init() == LOGGER_OK);
    assert(DataLogger_GetStatus(&info) == LOGGER_OK &&
           info.record_count == 0U && info.next_sequence == 0U);
    puts("PASS debug-only reset injection at record, metadata erase/partial/full program and log-sector erase durability boundaries");
}

int main(void)
{
    test_identity();
    test_parameters();
    test_program_and_erase();
    test_timeouts();
    test_failures();
    test_hardware_test_entrypoints();
    test_flash_manager_codec_and_map();
    test_flash_manager_storage();
    test_flash_manager_errors_and_debugger_tests();
    test_mpu6050_driver();
    test_wear_leveling_policy();
    test_metadata_codec_and_redundancy();
    test_recovery_scan_and_corrupt_tail();
    test_reclaim_commits_before_erase();
    test_data_logger_timing_and_records();
    test_data_logger_wrap_sector_and_errors();
    test_data_logger_circular_wrap();
    test_data_logger_debugger_mailbox();
    test_cli_commands_and_parser();
    test_phase8_benchmark_and_wear_diagnostics();
    test_debug_power_fail_reset_injection();
    puts("All Phase 1 through Phase 8 host tests passed.");
    return 0;
}
