#include "crc32.h"
#include <stddef.h>

#define CRC32_INITIAL_VALUE 0xFFFFFFFFUL
#define CRC32_POLYNOMIAL    0xEDB88320UL

uint32_t CRC32_Calculate(const uint8_t *data, uint32_t length)
{
    uint32_t crc = CRC32_INITIAL_VALUE;
    uint32_t index;
    uint8_t bit;

    if ((data == NULL) && (length != 0U))
    {
        return 0U;
    }

    for (index = 0U; index < length; ++index)
    {
        crc ^= data[index];
        for (bit = 0U; bit < 8U; ++bit)
        {
            crc = (crc >> 1U) ^ ((0U - (crc & 1U)) & CRC32_POLYNOMIAL);
        }
    }
    return crc ^ CRC32_INITIAL_VALUE;
}
