#ifndef W25Q64_PORT_H
#define W25Q64_PORT_H

#include "w25q64.h"

/* Link one implementation: STM32 SPL on target, protocol model on host.
 * Init leaves CS high. Select(0) MUST release CS even following a bus error.
 * Transfer sends/receives one byte, including TXE/RXNE/BSY timeouts.
 * Millisecond time must advance during polling, including with IRQs masked.
 * Time subtraction uses unsigned arithmetic to tolerate uint32_t wrap. */
W25Q64_Status_t W25Q64_PortInit(void);
void W25Q64_PortSelect(uint8_t active);
W25Q64_Status_t W25Q64_PortTransfer(uint8_t tx, uint8_t *rx);
uint32_t W25Q64_PortNowMs(void);
/* Return the configured peripheral SCK after prescaling, or 0 if unavailable. */
uint32_t W25Q64_PortGetClockHz(void);

#endif
