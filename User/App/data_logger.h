#ifndef DATA_LOGGER_H
#define DATA_LOGGER_H

#include <stdint.h>
#include "flash_manager.h"

typedef enum
{
    DATA_LOGGER_STOPPED = 0,
    DATA_LOGGER_RUNNING
} DataLoggerState_t;

typedef enum
{
    LOGGER_OK = 0,
    LOGGER_ERROR,
    LOGGER_TIMEOUT,
    LOGGER_INVALID_PARAM,
    LOGGER_NOT_INITIALIZED,
    LOGGER_NOT_READY,
    LOGGER_FLASH_ERROR,
    LOGGER_CRC_ERROR,
    LOGGER_SENSOR_ERROR,
    LOGGER_WEAR_LEVELING_ERROR,
    LOGGER_FULL
} DataLoggerStatus_t;

typedef struct
{
    DataLoggerState_t state;
    DataLoggerStatus_t last_error;
    uint32_t record_count;
    uint32_t next_sequence;
    uint32_t current_sector;
    uint32_t current_slot;
    uint32_t oldest_sector;
    uint32_t oldest_slot;
    uint32_t write_address;
    uint32_t oldest_address;
    uint32_t last_timestamp_ms;
} DataLoggerInfo_t;

/* Recover the Flash Manager, then initialize MPU6050. Read-only for W25Q64. */
DataLoggerStatus_t DataLogger_Init(void);
/* Start/stop periodic collection from the recovered or cleared state. */
DataLoggerStatus_t DataLogger_Start(void);
DataLoggerStatus_t DataLogger_Stop(void);
/* Poll from main. At most one sample is written per call. */
DataLoggerStatus_t DataLogger_Process(void);
DataLoggerStatus_t DataLogger_GetStatus(DataLoggerInfo_t *info);
/* Start a new durable session and commit redundant metadata. */
DataLoggerStatus_t DataLogger_Clear(void);
/* Read in chronological order: index 0 is the oldest retained record. */
DataLoggerStatus_t DataLogger_ReadRecord(uint32_t index, LogRecord_t *record);

#if DATA_LOGGER_TEST_ENABLE
/* Destructive Phase 4 test hook: erases sectors 0, 1 and the last log sector. */
DataLoggerStatus_t DataLogger_TestCircularWrap(void);
/* Phase 6 host hooks for verifying metadata-before-reclaim ordering. */
DataLoggerStatus_t DataLogger_TestSetupReclaim(void);
DataLoggerStatus_t DataLogger_TestPrepareWriteSector(void);
#endif

#endif
