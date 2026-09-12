#ifndef UART_PORT_H
#define UART_PORT_H

#include <stdint.h>

typedef enum
{
    UART_PORT_OK = 0,
    UART_PORT_EMPTY,
    UART_PORT_TIMEOUT,
    UART_PORT_INVALID_PARAM,
    UART_PORT_NOT_CONFIGURED,
    UART_PORT_ERROR
} UARTPortStatus_t;

/* Board port used by the CLI. TryRead never blocks. */
UARTPortStatus_t UART_PortInit(void);
UARTPortStatus_t UART_PortTryRead(uint8_t *byte);
UARTPortStatus_t UART_PortWrite(const uint8_t *data, uint32_t length);

#endif
