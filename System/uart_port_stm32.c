#include "config.h"
#include "uart_port.h"
#include <stddef.h>

#if UART_BOARD_CONFIGURED
#include "stm32f10x.h"

UARTPortStatus_t UART_PortInit(void)
{
    GPIO_InitTypeDef gpio;
    USART_InitTypeDef usart;
    UART_GPIO_CLOCK_ENABLE();
    UART_USART_CLOCK_ENABLE();
    UART_PIN_REMAP();

    gpio.GPIO_Pin = UART_TX_PIN;
    gpio.GPIO_Speed = GPIO_Speed_50MHz;
    gpio.GPIO_Mode = GPIO_Mode_AF_PP;
    GPIO_Init(UART_GPIO_PORT, &gpio);
    gpio.GPIO_Pin = UART_RX_PIN;
    gpio.GPIO_Mode = GPIO_Mode_IN_FLOATING;
    GPIO_Init(UART_GPIO_PORT, &gpio);

    USART_StructInit(&usart);
    usart.USART_BaudRate = UART_BAUD_RATE;
    usart.USART_WordLength = USART_WordLength_8b;
    usart.USART_StopBits = USART_StopBits_1;
    usart.USART_Parity = USART_Parity_No;
    usart.USART_HardwareFlowControl = USART_HardwareFlowControl_None;
    usart.USART_Mode = USART_Mode_Rx | USART_Mode_Tx;
    USART_Init(UART_USART, &usart);
    USART_Cmd(UART_USART, ENABLE);
    return UART_PORT_OK;
}

UARTPortStatus_t UART_PortTryRead(uint8_t *byte)
{
    if (byte == NULL) { return UART_PORT_INVALID_PARAM; }
    if (USART_GetFlagStatus(UART_USART, USART_FLAG_ORE) != RESET)
    {
        (void)UART_USART->SR;
        (void)UART_USART->DR;
        return UART_PORT_ERROR;
    }
    if (USART_GetFlagStatus(UART_USART, USART_FLAG_RXNE) == RESET)
    {
        return UART_PORT_EMPTY;
    }
    *byte = (uint8_t)USART_ReceiveData(UART_USART);
    return UART_PORT_OK;
}

UARTPortStatus_t UART_PortWrite(const uint8_t *data, uint32_t length)
{
    uint32_t index;
    uint32_t spins;
    if ((data == NULL) || (length == 0U))
    {
        return UART_PORT_INVALID_PARAM;
    }
    for (index = 0U; index < length; ++index)
    {
        spins = 0U;
        while (USART_GetFlagStatus(UART_USART, USART_FLAG_TXE) == RESET)
        {
            if (++spins >= UART_TX_SPIN_LIMIT) { return UART_PORT_TIMEOUT; }
        }
        USART_SendData(UART_USART, data[index]);
    }
    spins = 0U;
    while (USART_GetFlagStatus(UART_USART, USART_FLAG_TC) == RESET)
    {
        if (++spins >= UART_TX_SPIN_LIMIT) { return UART_PORT_TIMEOUT; }
    }
    return UART_PORT_OK;
}

#else

UARTPortStatus_t UART_PortInit(void)
{
    return UART_PORT_NOT_CONFIGURED;
}

UARTPortStatus_t UART_PortTryRead(uint8_t *byte)
{
    if (byte == NULL) { return UART_PORT_INVALID_PARAM; }
    return UART_PORT_NOT_CONFIGURED;
}

UARTPortStatus_t UART_PortWrite(const uint8_t *data, uint32_t length)
{
    if ((data == NULL) || (length == 0U))
    {
        return UART_PORT_INVALID_PARAM;
    }
    return UART_PORT_NOT_CONFIGURED;
}

#endif
