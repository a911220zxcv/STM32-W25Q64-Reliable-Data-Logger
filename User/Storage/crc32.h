#ifndef CRC32_H
#define CRC32_H

#include <stdint.h>

/* CRC-32/ISO-HDLC: reflected 0x04C11DB7 polynomial, init/final XOR all ones.
 * Returns zero for an empty input. A NULL pointer is valid only when length=0. */
uint32_t CRC32_Calculate(const uint8_t *data, uint32_t length);

#endif
