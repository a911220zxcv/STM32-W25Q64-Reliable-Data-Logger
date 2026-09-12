#include "config.h"
#include "cli.h"
#include "uart_port.h"
#include "w25q64.h"
#include "flash_manager.h"
#include "data_logger.h"
#include "flash_benchmark.h"
#include "wear_leveling.h"
#include <limits.h>
#include <stddef.h>
#include <string.h>

typedef enum
{
    CLI_JOB_NONE = 0,
    CLI_JOB_DUMP,
    CLI_JOB_VERIFY
} CLIJob_t;

static uint8_t initialized;
static char line_buffer[CLI_LINE_SIZE];
static uint32_t line_length;
static uint8_t line_overflow;
static uint8_t skip_lf;
static CLIJob_t pending_job;
static uint32_t job_index;
static uint32_t job_end;
static uint32_t verify_first_sequence;
static uint32_t verify_valid;
static uint32_t verify_crc_errors;
static uint32_t verify_sequence_errors;
static uint32_t verify_read_errors;

static CLIStatus_t map_uart_status(UARTPortStatus_t status)
{
    return status == UART_PORT_OK ? CLI_OK : CLI_UART_ERROR;
}

static CLIStatus_t write_bytes(const uint8_t *data, uint32_t length)
{
    if (length == 0U) { return CLI_OK; }
    return map_uart_status(UART_PortWrite(data, length));
}

static CLIStatus_t write_text(const char *text)
{
    uint32_t length = 0U;
    if (text == NULL) { return CLI_INVALID_PARAM; }
    while (text[length] != '\0') { ++length; }
    return write_bytes((const uint8_t *)text, length);
}

static CLIStatus_t write_u32(uint32_t value)
{
    char buffer[10];
    uint32_t length = 0U;
    uint32_t index;
    do
    {
        buffer[length++] = (char)('0' + value % 10U);
        value /= 10U;
    } while (value != 0U);
    for (index = 0U; index < length / 2U; ++index)
    {
        char temporary = buffer[index];
        buffer[index] = buffer[length - index - 1U];
        buffer[length - index - 1U] = temporary;
    }
    return write_bytes((const uint8_t *)buffer, length);
}

static CLIStatus_t write_u64(uint64_t value)
{
    char buffer[20];
    uint32_t length = 0U;
    uint32_t index;
    do
    {
        buffer[length++] = (char)('0' + value % 10U);
        value /= 10U;
    } while (value != 0U);
    for (index = 0U; index < length / 2U; ++index)
    {
        char temporary = buffer[index];
        buffer[index] = buffer[length - index - 1U];
        buffer[length - index - 1U] = temporary;
    }
    return write_bytes((const uint8_t *)buffer, length);
}

static CLIStatus_t write_i16(int16_t value)
{
    int32_t wide = value;
    CLIStatus_t status;
    if (wide < 0)
    {
        status = write_text("-");
        if (status != CLI_OK) { return status; }
        wide = -wide;
    }
    return write_u32((uint32_t)wide);
}

static CLIStatus_t write_hex(uint32_t value, uint32_t digits)
{
    static const char hex[] = "0123456789ABCDEF";
    char buffer[8];
    uint32_t index;
    if ((digits == 0U) || (digits > sizeof(buffer)))
    {
        return CLI_INVALID_PARAM;
    }
    for (index = 0U; index < digits; ++index)
    {
        buffer[digits - index - 1U] = hex[value & 0x0FU];
        value >>= 4U;
    }
    return write_bytes((const uint8_t *)buffer, digits);
}

static CLIStatus_t write_label_u32(const char *label, uint32_t value)
{
    CLIStatus_t status = write_text(label);
    if (status != CLI_OK) { return status; }
    status = write_u32(value);
    if (status != CLI_OK) { return status; }
    return write_text("\r\n");
}

static CLIStatus_t write_label_u64(const char *label, uint64_t value)
{
    CLIStatus_t status = write_text(label);
    if (status != CLI_OK) { return status; }
    status = write_u64(value);
    if (status != CLI_OK) { return status; }
    return write_text("\r\n");
}

static CLIStatus_t write_prompt(void)
{
    return write_text("> ");
}

static CLIStatus_t write_error(const char *message)
{
    CLIStatus_t status = write_text("ERROR: ");
    if (status != CLI_OK) { return status; }
    status = write_text(message);
    if (status != CLI_OK) { return status; }
    return write_text("\r\n");
}

static uint8_t parse_u32(const char *text, uint32_t *value)
{
    uint32_t result = 0U;
    uint32_t base = 10U;
    uint32_t digit;
    if ((text == NULL) || (value == NULL) || (*text == '\0')) { return 0U; }
    if ((text[0] == '0') && ((text[1] == 'x') || (text[1] == 'X')))
    {
        base = 16U;
        text += 2;
        if (*text == '\0') { return 0U; }
    }
    while (*text != '\0')
    {
        if ((*text >= '0') && (*text <= '9'))
        {
            digit = (uint32_t)(*text - '0');
        }
        else if ((*text >= 'a') && (*text <= 'f'))
        {
            digit = (uint32_t)(*text - 'a') + 10U;
        }
        else if ((*text >= 'A') && (*text <= 'F'))
        {
            digit = (uint32_t)(*text - 'A') + 10U;
        }
        else { return 0U; }
        if ((digit >= base) || (result > (UINT32_MAX - digit) / base))
        {
            return 0U;
        }
        result = result * base + digit;
        ++text;
    }
    *value = result;
    return 1U;
}

static uint32_t tokenize(char *line, char *argv[CLI_MAX_ARGS])
{
    uint32_t argc = 0U;
    char *cursor = line;
    while (*cursor != '\0')
    {
        while (*cursor == ' ') { ++cursor; }
        if (*cursor == '\0') { break; }
        if (argc >= CLI_MAX_ARGS) { return CLI_MAX_ARGS + 1U; }
        argv[argc++] = cursor;
        while ((*cursor != '\0') && (*cursor != ' ')) { ++cursor; }
        if (*cursor != '\0') { *cursor++ = '\0'; }
    }
    return argc;
}

static CLIStatus_t print_flash_id(void)
{
    uint32_t id;
    CLIStatus_t status;
    if (W25Q64_ReadJEDECID(&id) != W25Q64_OK)
    {
        return write_error("flash communication failed");
    }
    status = write_text("Manufacturer: ");
    if (status != CLI_OK) { return status; }
    status = write_text(id == W25Q64_EXPECTED_JEDEC_ID ? "Winbond\r\n" :
                                                       "Unknown\r\n");
    if (status != CLI_OK) { return status; }
    status = write_text("JEDEC ID: 0x");
    if (status != CLI_OK) { return status; }
    status = write_hex(id, 6U);
    if (status != CLI_OK) { return status; }
    return write_text("\r\nCapacity: 8 MB\r\n");
}

static CLIStatus_t print_flash_status(void)
{
    uint8_t sr1;
    uint8_t sr2;
    CLIStatus_t status;
    if ((W25Q64_ReadStatusRegister(1U, &sr1) != W25Q64_OK) ||
        (W25Q64_ReadStatusRegister(2U, &sr2) != W25Q64_OK))
    {
        return write_error("flash status read failed");
    }
    status = write_text("SR1: 0x");
    if (status != CLI_OK) { return status; }
    status = write_hex(sr1, 2U);
    if (status != CLI_OK) { return status; }
    status = write_text("\r\nSR2: 0x");
    if (status != CLI_OK) { return status; }
    status = write_hex(sr2, 2U);
    if (status != CLI_OK) { return status; }
    return write_text("\r\n");
}

static CLIStatus_t print_flash_bytes(uint32_t address, uint32_t length)
{
    uint8_t data[CLI_FLASH_READ_MAX];
    uint32_t index;
    CLIStatus_t status;
    if ((length == 0U) || (length > CLI_FLASH_READ_MAX) ||
        (address >= FLASH_TOTAL_SIZE) ||
        (length > FLASH_TOTAL_SIZE - address))
    {
        return write_error("address or length out of range");
    }
    if (W25Q64_Read(address, data, length) != W25Q64_OK)
    {
        return write_error("flash read failed");
    }
    for (index = 0U; index < length; ++index)
    {
        if ((index % 16U) == 0U)
        {
            status = write_text("0x");
            if (status != CLI_OK) { return status; }
            status = write_hex(address + index, 6U);
            if (status != CLI_OK) { return status; }
            status = write_text(":");
            if (status != CLI_OK) { return status; }
        }
        status = write_text(" ");
        if (status != CLI_OK) { return status; }
        status = write_hex(data[index], 2U);
        if (status != CLI_OK) { return status; }
        if (((index % 16U) == 15U) || (index + 1U == length))
        {
            status = write_text("\r\n");
            if (status != CLI_OK) { return status; }
        }
    }
    return CLI_OK;
}

static CLIStatus_t erase_log_sector(uint32_t sector)
{
    DataLoggerInfo_t info;
    FlashManagerStatus_t flash_status;
    if (sector >= FLASH_LOG_SECTOR_COUNT)
    {
        return write_error("sector outside log area");
    }
    if (DataLogger_GetStatus(&info) != LOGGER_OK)
    {
        return write_error("logger unavailable");
    }
    if (info.state != DATA_LOGGER_STOPPED)
    {
        return write_error("stop logger before erase");
    }
    if (info.record_count != 0U)
    {
        return write_error("log clear required before raw erase");
    }
    flash_status = FlashManager_PrepareSector(sector);
    if (flash_status != FLASH_MANAGER_OK)
    {
        return write_error("sector erase failed");
    }
    return write_text("Sector erased.\r\n");
}

static CLIStatus_t print_benchmark_measurement(const char *label,
                                               uint32_t elapsed_ms,
                                               uint32_t speed_kib_per_s)
{
    CLIStatus_t status = write_text(label);
    if (status != CLI_OK) { return status; }
    status = write_text("Time: ");
    if (status != CLI_OK) { return status; }
    status = write_u32(elapsed_ms);
    if (status != CLI_OK) { return status; }
    status = write_text(" ms\r\n");
    if (status != CLI_OK) { return status; }
    status = write_text("Speed: ");
    if (status != CLI_OK) { return status; }
    status = write_u32(speed_kib_per_s);
    if (status != CLI_OK) { return status; }
    return write_text(" KiB/s\r\n");
}

static CLIStatus_t run_flash_benchmark(void)
{
    DataLoggerInfo_t info;
    FlashBenchmarkResult_t result;
    FlashBenchmarkStatus_t benchmark_status;
    CLIStatus_t status;
    if (DataLogger_GetStatus(&info) != LOGGER_OK)
    {
        return write_error("logger unavailable");
    }
    if (info.state != DATA_LOGGER_STOPPED)
    {
        return write_error("stop logger before benchmark");
    }
    if (info.record_count != 0U)
    {
        return write_error("log clear required before benchmark");
    }
    benchmark_status = FlashBenchmark_Run(info.current_sector, &result);
    if (benchmark_status == FLASH_BENCHMARK_TIMEOUT)
    {
        return write_error("benchmark flash timeout");
    }
    if (benchmark_status == FLASH_BENCHMARK_VERIFY_ERROR)
    {
        return write_error("benchmark data verify failed");
    }
    if (benchmark_status == FLASH_BENCHMARK_TIMER_ERROR)
    {
        return write_error("benchmark timer resolution failure");
    }
    if (benchmark_status != FLASH_BENCHMARK_OK)
    {
        return write_error("benchmark flash operation failed");
    }
    status = write_text("W25Q64 Benchmark\r\n");
    if (status != CLI_OK) { return status; }
    status = write_label_u32("Log sector: ", result.sector_index);
    if (status != CLI_OK) { return status; }
    status = write_text("Address: 0x");
    if (status != CLI_OK) { return status; }
    status = write_hex(result.address, 6U);
    if (status != CLI_OK) { return status; }
    status = write_text("\r\nSPI clock: ");
    if (status != CLI_OK) { return status; }
    status = write_u32(result.spi_clock_hz);
    if (status != CLI_OK) { return status; }
    status = write_text(" Hz\r\n");
    if (status != CLI_OK) { return status; }
    status = print_benchmark_measurement("Read 4 KiB:\r\n",
                                         result.read_time_ms,
                                         result.read_speed_kib_per_s);
    if (status != CLI_OK) { return status; }
    status = print_benchmark_measurement("Write 4 KiB:\r\n",
                                         result.write_time_ms,
                                         result.write_speed_kib_per_s);
    if (status != CLI_OK) { return status; }
    status = write_text("Sector erase + verify:\r\nTime: ");
    if (status != CLI_OK) { return status; }
    status = write_u32(result.erase_time_ms);
    if (status != CLI_OK) { return status; }
    return write_text(" ms\r\nCleanup: erased and verified\r\n");
}

static CLIStatus_t print_wear_status(uint32_t start_sector,
                                    uint32_t display_count)
{
    WearLevelingStats_t wear;
    DataLoggerInfo_t info;
    CLIStatus_t status;
    uint32_t index;
    uint32_t sector;
    uint32_t erase_count;
    if ((start_sector >= FLASH_LOG_SECTOR_COUNT) ||
        (display_count == 0U) || (display_count > CLI_WEAR_DISPLAY_MAX))
    {
        return write_error("invalid wear display range");
    }
    if ((WearLeveling_GetStats(&wear) != WEAR_LEVELING_OK) ||
        (DataLogger_GetStatus(&info) != LOGGER_OK))
    {
        return write_error("wear diagnostics unavailable");
    }
    status = write_text("Wear counters: logger-managed RAM only\r\n");
    if (status != CLI_OK) { return status; }
    status = write_label_u64("Total erases: ", wear.total_erases);
    if (status != CLI_OK) { return status; }
    status = write_label_u32("Minimum: ", wear.minimum_erase_count);
    if (status != CLI_OK) { return status; }
    status = write_label_u32("Maximum: ", wear.maximum_erase_count);
    if (status != CLI_OK) { return status; }
    status = write_label_u32("Spread: ", wear.erase_count_spread);
    if (status != CLI_OK) { return status; }
    status = write_label_u32("Sectors at minimum: ", wear.sectors_at_minimum);
    if (status != CLI_OK) { return status; }
    status = write_label_u32("Sectors at maximum: ", wear.sectors_at_maximum);
    if (status != CLI_OK) { return status; }
    status = write_label_u32("Next erase sector: ", wear.next_sector);
    if (status != CLI_OK) { return status; }
    status = write_label_u32("Current sector: ", info.current_sector);
    if (status != CLI_OK) { return status; }
    status = write_label_u32("Oldest sector: ", info.oldest_sector);
    if (status != CLI_OK) { return status; }
    status = write_text("Sector Erase count\r\n");
    if (status != CLI_OK) { return status; }
    sector = start_sector;
    for (index = 0U; index < display_count; ++index)
    {
        if (WearLeveling_GetSectorEraseCount(sector, &erase_count) !=
            WEAR_LEVELING_OK)
        {
            return write_error("wear counter read failed");
        }
        status = write_u32(sector);
        if (status != CLI_OK) { return status; }
        status = write_text(" ");
        if (status != CLI_OK) { return status; }
        status = write_u32(erase_count);
        if (status != CLI_OK) { return status; }
        status = write_text("\r\n");
        if (status != CLI_OK) { return status; }
        sector = sector + 1U;
        if (sector >= FLASH_LOG_SECTOR_COUNT) { sector = 0U; }
    }
    return CLI_OK;
}

static CLIStatus_t print_log_status(void)
{
    DataLoggerInfo_t info;
    CLIStatus_t status;
    if (DataLogger_GetStatus(&info) != LOGGER_OK)
    {
        return write_error("logger unavailable");
    }
    status = write_text("Logger: ");
    if (status != CLI_OK) { return status; }
    status = write_text(info.state == DATA_LOGGER_RUNNING ? "RUNNING\r\n" :
                                                          "STOPPED\r\n");
    if (status != CLI_OK) { return status; }
    status = write_label_u32("Records: ", info.record_count);
    if (status != CLI_OK) { return status; }
    status = write_label_u32("Next sequence: ", info.next_sequence);
    if (status != CLI_OK) { return status; }
    status = write_label_u32("Current sector: ", info.current_sector);
    if (status != CLI_OK) { return status; }
    status = write_label_u32("Current offset: ",
                             info.current_slot * FLASH_LOG_RECORD_SIZE);
    if (status != CLI_OK) { return status; }
    status = write_label_u32("Oldest sector: ", info.oldest_sector);
    if (status != CLI_OK) { return status; }
    return write_label_u32("Last error: ", (uint32_t)info.last_error);
}

static CLIStatus_t print_metadata(void)
{
    FlashManagerRecovery_t recovery;
    CLIStatus_t status;
    if (FlashManager_GetLastRecovery(&recovery) != FLASH_MANAGER_OK)
    {
        return write_error("metadata unavailable");
    }
    status = write_text("Active metadata: ");
    if (status != CLI_OK) { return status; }
    if (recovery.active_copy == FLASH_METADATA_COPY_A)
    {
        status = write_text("A\r\n");
    }
    else if (recovery.active_copy == FLASH_METADATA_COPY_B)
    {
        status = write_text("B\r\n");
    }
    else { status = write_text("NONE\r\n"); }
    if (status != CLI_OK) { return status; }
    status = write_label_u32("Generation: ", recovery.generation);
    if (status != CLI_OK) { return status; }
    status = write_label_u32("Write sector: ", recovery.state.write_sector);
    if (status != CLI_OK) { return status; }
    status = write_label_u32("Write offset: ",
                             recovery.state.write_slot *
                             FLASH_LOG_RECORD_SIZE);
    if (status != CLI_OK) { return status; }
    status = write_label_u32("Oldest sector: ",
                             recovery.state.oldest_sector);
    if (status != CLI_OK) { return status; }
    status = write_label_u32("Records: ", recovery.state.record_count);
    if (status != CLI_OK) { return status; }
    status = write_label_u32("Next sequence: ",
                             recovery.state.next_sequence);
    if (status != CLI_OK) { return status; }
    status = write_label_u32("Metadata valid: ", recovery.metadata_valid);
    if (status != CLI_OK) { return status; }
    status = write_label_u32("Used log scan: ", recovery.used_log_scan);
    if (status != CLI_OK) { return status; }
    return write_label_u32("Has gaps: ", recovery.has_gaps);
}

static CLIStatus_t print_record(const LogRecord_t *record)
{
    CLIStatus_t status;
    status = write_u32(record->sequence);
    if (status != CLI_OK) { return status; }
    status = write_text(" ");
    if (status != CLI_OK) { return status; }
    status = write_u32(record->timestamp_ms);
    if (status != CLI_OK) { return status; }
#define WRITE_SENSOR_FIELD(field) \
    do { \
        status = write_text(" "); \
        if (status != CLI_OK) { return status; } \
        status = write_i16(record->field); \
        if (status != CLI_OK) { return status; } \
    } while (0)
    WRITE_SENSOR_FIELD(accel_x);
    WRITE_SENSOR_FIELD(accel_y);
    WRITE_SENSOR_FIELD(accel_z);
    WRITE_SENSOR_FIELD(gyro_x);
    WRITE_SENSOR_FIELD(gyro_y);
    WRITE_SENSOR_FIELD(gyro_z);
#undef WRITE_SENSOR_FIELD
    status = write_text(" 0x");
    if (status != CLI_OK) { return status; }
    status = write_hex(record->crc32, 8U);
    if (status != CLI_OK) { return status; }
    return write_text("\r\n");
}

static CLIStatus_t print_verify_summary(void)
{
    CLIStatus_t status = write_label_u32("Records checked: ", job_end);
    if (status != CLI_OK) { return status; }
    status = write_label_u32("Valid: ", verify_valid);
    if (status != CLI_OK) { return status; }
    status = write_label_u32("CRC errors: ", verify_crc_errors);
    if (status != CLI_OK) { return status; }
    status = write_label_u32("Sequence errors: ", verify_sequence_errors);
    if (status != CLI_OK) { return status; }
    return write_label_u32("Read errors: ", verify_read_errors);
}

static CLIStatus_t print_verify_issue(uint32_t index, const char *issue)
{
    CLIStatus_t status = write_text("Record ");
    if (status != CLI_OK) { return status; }
    status = write_u32(index);
    if (status != CLI_OK) { return status; }
    status = write_text(": ");
    if (status != CLI_OK) { return status; }
    status = write_text(issue);
    if (status != CLI_OK) { return status; }
    return write_text("\r\n");
}

static CLIStatus_t start_job(CLIJob_t job, uint32_t count)
{
    DataLoggerInfo_t info;
    CLIStatus_t status;
    if (DataLogger_GetStatus(&info) != LOGGER_OK)
    {
        return write_error("logger unavailable");
    }
    if (info.state != DATA_LOGGER_STOPPED)
    {
        return write_error("stop logger before reading or verifying");
    }
    if (count > info.record_count) { count = info.record_count; }
    job_index = job == CLI_JOB_DUMP ? info.record_count - count : 0U;
    job_end = job == CLI_JOB_DUMP ? info.record_count : count;
    if (job == CLI_JOB_VERIFY)
    {
        verify_first_sequence = info.next_sequence - info.record_count;
        verify_valid = 0U;
        verify_crc_errors = 0U;
        verify_sequence_errors = 0U;
        verify_read_errors = 0U;
    }
    if (job == CLI_JOB_DUMP)
    {
        status = write_text("SEQ TIME(ms) AX AY AZ GX GY GZ CRC\r\n");
        if (status != CLI_OK) { return status; }
    }
    pending_job = job;
    if (job_index == job_end)
    {
        pending_job = CLI_JOB_NONE;
        if (job == CLI_JOB_VERIFY)
        {
            status = print_verify_summary();
            if (status != CLI_OK) { return status; }
        }
        return CLI_OK;
    }
    return CLI_OK;
}

static CLIStatus_t advance_job(void)
{
    LogRecord_t record;
    DataLoggerStatus_t logger_status;
    CLIStatus_t status;
    if (pending_job == CLI_JOB_NONE) { return CLI_OK; }
    logger_status = DataLogger_ReadRecord(job_index, &record);
    if (pending_job == CLI_JOB_DUMP)
    {
        if (logger_status != LOGGER_OK)
        {
            pending_job = CLI_JOB_NONE;
            status = write_error("record CRC or read failure");
            if (status != CLI_OK) { return status; }
            return write_prompt();
        }
        status = print_record(&record);
        if (status != CLI_OK) { return status; }
    }
    else if (logger_status == LOGGER_OK)
    {
        ++verify_valid;
        if (record.sequence != verify_first_sequence + job_index)
        {
            ++verify_sequence_errors;
            status = print_verify_issue(job_index, "sequence mismatch");
            if (status != CLI_OK) { return status; }
        }
    }
    else if (logger_status == LOGGER_CRC_ERROR)
    {
        ++verify_crc_errors;
        status = print_verify_issue(job_index, "CRC mismatch");
        if (status != CLI_OK) { return status; }
    }
    else
    {
        ++verify_read_errors;
        status = print_verify_issue(job_index, "read failure");
        if (status != CLI_OK) { return status; }
    }
    ++job_index;
    if (job_index >= job_end)
    {
        CLIJob_t completed = pending_job;
        pending_job = CLI_JOB_NONE;
        if (completed == CLI_JOB_VERIFY)
        {
            status = print_verify_summary();
            if (status != CLI_OK) { return status; }
        }
        return write_prompt();
    }
    return CLI_OK;
}

static CLIStatus_t print_help(void)
{
    return write_text(
        "help\r\n"
        "flash id\r\n"
        "flash status\r\n"
        "flash read <address> <length 1..64>\r\n"
        "flash erase <log-sector 0..2045>\r\n"
        "flash benchmark\r\n"
        "log start | stop | status | count | clear | verify\r\n"
        "log read <count> | readall\r\n"
        "metadata\r\n"
        "wear status [start count<=16]\r\n");
}

static CLIStatus_t execute_flash(uint32_t argc, char *argv[CLI_MAX_ARGS])
{
    uint32_t first;
    uint32_t second;
    if (argc < 2U) { return write_error("invalid flash command"); }
    if ((strcmp(argv[1], "id") == 0) && (argc == 2U))
    {
        return print_flash_id();
    }
    if ((strcmp(argv[1], "status") == 0) && (argc == 2U))
    {
        return print_flash_status();
    }
    if ((strcmp(argv[1], "read") == 0) && (argc == 4U))
    {
        if (!parse_u32(argv[2], &first) || !parse_u32(argv[3], &second))
        {
            return write_error("invalid numeric argument");
        }
        return print_flash_bytes(first, second);
    }
    if ((strcmp(argv[1], "erase") == 0) && (argc == 3U))
    {
        if (!parse_u32(argv[2], &first))
        {
            return write_error("invalid sector");
        }
        return erase_log_sector(first);
    }
    if ((strcmp(argv[1], "benchmark") == 0) && (argc == 2U))
    {
        return run_flash_benchmark();
    }
    return write_error("invalid flash command");
}

static CLIStatus_t execute_log(uint32_t argc, char *argv[CLI_MAX_ARGS])
{
    DataLoggerInfo_t info;
    DataLoggerStatus_t logger_status;
    uint32_t count;
    if (argc < 2U) { return write_error("invalid log command"); }
    if ((strcmp(argv[1], "start") == 0) && (argc == 2U))
    {
        logger_status = DataLogger_Start();
        return logger_status == LOGGER_OK ? write_text("Logging started.\r\n") :
                                           write_error("logging start failed");
    }
    if ((strcmp(argv[1], "stop") == 0) && (argc == 2U))
    {
        logger_status = DataLogger_Stop();
        return logger_status == LOGGER_OK ? write_text("Logging stopped.\r\n") :
                                           write_error("logging stop failed");
    }
    if ((strcmp(argv[1], "status") == 0) && (argc == 2U))
    {
        return print_log_status();
    }
    if ((strcmp(argv[1], "count") == 0) && (argc == 2U))
    {
        if (DataLogger_GetStatus(&info) != LOGGER_OK)
        {
            return write_error("logger unavailable");
        }
        return write_label_u32("Records: ", info.record_count);
    }
    if ((strcmp(argv[1], "clear") == 0) && (argc == 2U))
    {
        logger_status = DataLogger_Clear();
        return logger_status == LOGGER_OK ? write_text("Log cleared.\r\n") :
                                           write_error("log clear failed");
    }
    if ((strcmp(argv[1], "read") == 0) && (argc == 3U))
    {
        if (!parse_u32(argv[2], &count))
        {
            return write_error("invalid record count");
        }
        return start_job(CLI_JOB_DUMP, count);
    }
    if ((strcmp(argv[1], "readall") == 0) && (argc == 2U))
    {
        if (DataLogger_GetStatus(&info) != LOGGER_OK)
        {
            return write_error("logger unavailable");
        }
        return start_job(CLI_JOB_DUMP, info.record_count);
    }
    if ((strcmp(argv[1], "verify") == 0) && (argc == 2U))
    {
        if (DataLogger_GetStatus(&info) != LOGGER_OK)
        {
            return write_error("logger unavailable");
        }
        return start_job(CLI_JOB_VERIFY, info.record_count);
    }
    return write_error("invalid log command");
}

static CLIStatus_t execute_wear(uint32_t argc, char *argv[CLI_MAX_ARGS])
{
    DataLoggerInfo_t info;
    uint32_t start_sector;
    uint32_t display_count;
    if ((argc != 2U) && (argc != 4U))
    {
        return write_error("usage: wear status [start count]");
    }
    if (strcmp(argv[1], "status") != 0)
    {
        return write_error("invalid wear command");
    }
    if (argc == 2U)
    {
        if (DataLogger_GetStatus(&info) != LOGGER_OK)
        {
            return write_error("logger unavailable");
        }
        start_sector = info.current_sector;
        display_count = CLI_WEAR_DISPLAY_COUNT;
    }
    else
    {
        if (!parse_u32(argv[2], &start_sector) ||
            !parse_u32(argv[3], &display_count))
        {
            return write_error("invalid numeric argument");
        }
    }
    return print_wear_status(start_sector, display_count);
}

static CLIStatus_t execute_line(char *line)
{
    char *argv[CLI_MAX_ARGS];
    uint32_t argc = tokenize(line, argv);
    if (argc == 0U) { return write_prompt(); }
    if (argc > CLI_MAX_ARGS) { return write_error("too many arguments"); }
    if (pending_job != CLI_JOB_NONE) { return write_error("command busy"); }
    if ((strcmp(argv[0], "help") == 0) && (argc == 1U))
    {
        return print_help();
    }
    if (strcmp(argv[0], "flash") == 0) { return execute_flash(argc, argv); }
    if (strcmp(argv[0], "log") == 0) { return execute_log(argc, argv); }
    if (strcmp(argv[0], "wear") == 0) { return execute_wear(argc, argv); }
    if ((strcmp(argv[0], "metadata") == 0) && (argc == 1U))
    {
        return print_metadata();
    }
    return write_error("invalid command");
}

CLIStatus_t CLI_Init(void)
{
    CLIStatus_t status;
    FlashManagerRecovery_t recovery;
    initialized = 0U;
    line_length = 0U;
    line_overflow = 0U;
    skip_lf = 0U;
    pending_job = CLI_JOB_NONE;
    if (UART_PortInit() != UART_PORT_OK) { return CLI_UART_ERROR; }
    initialized = 1U;
    status = write_text("\r\nSTM32 W25Q64 Data Logger\r\n");
    if (status != CLI_OK) { return status; }
    status = print_flash_id();
    if (status != CLI_OK) { return status; }
    if (FlashManager_GetLastRecovery(&recovery) == FLASH_MANAGER_OK)
    {
        status = write_text(recovery.metadata_valid ?
                            "Metadata recovered.\r\n" :
                            "Metadata unavailable; log scan used.\r\n");
        if (status != CLI_OK) { return status; }
        status = write_label_u32("Records found: ",
                                 recovery.state.record_count);
        if (status != CLI_OK) { return status; }
    }
    return write_prompt();
}

CLIStatus_t CLI_Process(void)
{
    uint8_t byte;
    uint32_t budget;
    UARTPortStatus_t uart_status;
    CLIStatus_t status;
    if (!initialized) { return CLI_NOT_INITIALIZED; }
    for (budget = 0U; budget < CLI_RX_BUDGET; ++budget)
    {
        uart_status = UART_PortTryRead(&byte);
        if (uart_status == UART_PORT_EMPTY) { break; }
        if (uart_status != UART_PORT_OK) { return CLI_UART_ERROR; }
        if ((byte == '\r') || (byte == '\n'))
        {
            if ((byte == '\n') && skip_lf)
            {
                skip_lf = 0U;
                continue;
            }
            skip_lf = (uint8_t)(byte == '\r');
            status = write_text("\r\n");
            if (status != CLI_OK) { return status; }
            if (line_overflow)
            {
                status = write_error("command line too long");
                if (status == CLI_OK) { status = write_prompt(); }
            }
            else
            {
                line_buffer[line_length] = '\0';
                status = execute_line(line_buffer);
                if ((status == CLI_OK) && (pending_job == CLI_JOB_NONE) &&
                    (line_length != 0U))
                {
                    status = write_prompt();
                }
            }
            line_length = 0U;
            line_overflow = 0U;
            if (status != CLI_OK) { return status; }
            continue;
        }
        skip_lf = 0U;
        if ((byte == 8U) || (byte == 127U))
        {
            if (!line_overflow && (line_length != 0U))
            {
                --line_length;
                status = write_text("\b \b");
                if (status != CLI_OK) { return status; }
            }
            continue;
        }
        if ((byte < 32U) || (byte > 126U)) { continue; }
        if (line_length + 1U >= CLI_LINE_SIZE)
        {
            line_overflow = 1U;
        }
        else if (!line_overflow)
        {
            line_buffer[line_length++] = (char)byte;
            status = write_bytes(&byte, 1U);
            if (status != CLI_OK) { return status; }
        }
    }
    return advance_job();
}

#if CLI_TEST_ENABLE
CLIStatus_t CLI_TestExecuteLine(const char *line)
{
    uint32_t length = 0U;
    if (!initialized) { return CLI_NOT_INITIALIZED; }
    if (line == NULL) { return CLI_INVALID_PARAM; }
    while (line[length] != '\0')
    {
        if (length + 1U >= CLI_LINE_SIZE) { return CLI_INVALID_PARAM; }
        line_buffer[length] = line[length];
        ++length;
    }
    line_buffer[length] = '\0';
    return execute_line(line_buffer);
}
#endif
