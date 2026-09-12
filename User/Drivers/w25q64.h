#ifndef W25Q64_H
#define W25Q64_H

#include <stdint.h>

#define W25Q64_TOTAL_SIZE       0x800000UL
#define W25Q64_PAGE_SIZE        256UL
#define W25Q64_SECTOR_SIZE      4096UL
#define W25Q64_BLOCK_SIZE       65536UL

typedef enum
{
    W25Q64_OK = 0,
    W25Q64_ERROR,
    W25Q64_TIMEOUT,
    W25Q64_INVALID_PARAM,
    W25Q64_NOT_INITIALIZED,
    W25Q64_NOT_CONFIGURED,
    W25Q64_UNSUPPORTED_DEVICE,
    W25Q64_WRITE_ENABLE_ERROR,
    W25Q64_VERIFY_ERROR
} W25Q64_Status_t;

/* W25Q64BV, single device, synchronous, non-reentrant. No ISR calls.
 * No suspend/resume support: device must not have been externally suspended.
 * All data pointers must be valid for length bytes; no dynamic allocation.
 * No implicit erase. Program only erased bytes (NOR: 1 -> 0).
 * An error/timeout may leave partially modified flash; no rollback/retry.
 * Program and erase return OK only after BUSY clears AND read-back matches. */

/* Initialize port, wake flash, wait ready, reject non-EF4017 device. */
W25Q64_Status_t W25Q64_Init(void);
/* Output is modified only on success. BV status register number is 1 or 2.
 * Register 3 is invalid and emits no SPI command. Reserved SR2 bits are raw. */
W25Q64_Status_t W25Q64_ReadJEDECID(uint32_t *id);
/* Report the runtime SPI SCK derived by the hardware port. */
W25Q64_Status_t W25Q64_GetSPIClockHz(uint32_t *clock_hz);
W25Q64_Status_t W25Q64_ReadStatusRegister(uint8_t number, uint8_t *value);
/* Wait ready, send WREN, confirm WEL. Usually called internally. */
W25Q64_Status_t W25Q64_WriteEnable(void);
/* Poll SR1 BUSY with millisecond deadline. 0 means one immediate check.
 * Accepted timeout <= W25Q64_READY_TIMEOUT_MS; stalled clock is bounded too. */
W25Q64_Status_t W25Q64_WaitBusy(uint32_t timeout_ms);
/* Nonzero length; full range must fit inside 8 MiB. */
W25Q64_Status_t W25Q64_Read(uint32_t address, uint8_t *data, uint32_t length);
/* Strict single-page operation; crossing a page returns INVALID_PARAM. */
W25Q64_Status_t W25Q64_PageProgram(uint32_t address, const uint8_t *data,
                               uint32_t length);
/* Automatically split at page boundaries; failure may follow earlier pages. */
W25Q64_Status_t W25Q64_Write(uint32_t address, const uint8_t *data, uint32_t length);
/* Byte address MUST be aligned; never silently round down. Block = 64 KiB.
 * Driver permits the whole device; application must enforce its memory map. */
W25Q64_Status_t W25Q64_SectorErase(uint32_t address);
W25Q64_Status_t W25Q64_BlockErase(uint32_t address);
/* No Chip Erase API or command. */

#endif
