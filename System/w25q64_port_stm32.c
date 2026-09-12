#include "config.h"
#include "w25q64_port.h"

#if W25Q64_BOARD_CONFIGURED
#include "stm32f10x.h"

/* Phase 1 reserves unused TIM2: 10 kHz, no interrupt, no SysTick conflict.
 * Not an application uptime clock: sample at least every 6.5536 seconds while
 * timing an operation. All driver deadlines are <= 3 seconds. Idle wraps do
 * not affect subsequent relative deadlines. Do not change APB clocks in use. */
#define TIMER_HZ       10000UL
#define TICKS_PER_MS   10UL
#define MAX_SCK_HZ     20000000UL
static uint16_t last_tick;
static uint32_t elapsed_ms;
static uint32_t fraction;
static uint8_t bus_fault;

uint32_t W25Q64_PortNowMs(void)
{
    uint16_t tick = (uint16_t)TIM_GetCounter(TIM2);
    fraction += (uint16_t)(tick - last_tick);
    last_tick = tick;
    elapsed_ms += fraction / TICKS_PER_MS;
    fraction %= TICKS_PER_MS;
    return elapsed_ms;
}

uint32_t W25Q64_PortGetClockHz(void)
{
    RCC_ClocksTypeDef clocks;
    uint32_t peripheral_clock;
    uint32_t divisor;
    RCC_GetClocksFreq(&clocks);
    peripheral_clock = W25Q64_SPI == SPI1 ?
                       clocks.PCLK2_Frequency : clocks.PCLK1_Frequency;
    divisor = 2UL << ((uint32_t)W25Q64_SPI_PRESCALER >> 3);
    return peripheral_clock / divisor;
}

void W25Q64_PortSelect(uint8_t active)
{
    if (active) { GPIO_ResetBits(W25Q64_CS_GPIO_PORT, W25Q64_CS_PIN); }
    else
    {
        GPIO_SetBits(W25Q64_CS_GPIO_PORT, W25Q64_CS_PIN);
        /* >= 111 ns at the STM32F103 maximum 72 MHz; meet CS high time. */
        __NOP(); __NOP(); __NOP(); __NOP();
        __NOP(); __NOP(); __NOP(); __NOP();
    }
}

static W25Q64_Status_t wait_flag(uint16_t flag, FlagStatus expected)
{
    uint32_t start = W25Q64_PortNowMs();
    uint32_t spins;
    for (spins = 0U; spins < W25Q64_SPI_SPIN_LIMIT; ++spins)
    {
        if ((W25Q64_SPI->SR & (SPI_I2S_FLAG_OVR | SPI_FLAG_MODF)) != 0U)
        {
            return W25Q64_ERROR;
        }
        if (SPI_I2S_GetFlagStatus(W25Q64_SPI, flag) == expected) { return W25Q64_OK; }
        if ((uint32_t)(W25Q64_PortNowMs() - start) >= W25Q64_SPI_TIMEOUT_MS)
        {
            return W25Q64_TIMEOUT;
        }
    }
    return W25Q64_TIMEOUT;
}

W25Q64_Status_t W25Q64_PortTransfer(uint8_t tx, uint8_t *rx)
{
    W25Q64_Status_t status;
    if (bus_fault) { return W25Q64_ERROR; }
    status = wait_flag(SPI_I2S_FLAG_TXE, SET);
    if (status == W25Q64_OK)
    {
        SPI_I2S_SendData(W25Q64_SPI, tx);
        status = wait_flag(SPI_I2S_FLAG_RXNE, SET);
    }
    if (status == W25Q64_OK)
    {
        *rx = (uint8_t)SPI_I2S_ReceiveData(W25Q64_SPI);
        status = wait_flag(SPI_I2S_FLAG_BSY, RESET);
    }
    if (status != W25Q64_OK)
    {
        /* Stop clocks before transaction cleanup releases CS. Re-init required.
         * Partial program/erase may already have been accepted by the flash. */
        SPI_Cmd(W25Q64_SPI, DISABLE);
        bus_fault = 1U;
    }
    return status;
}

static void gpio_init(GPIO_TypeDef *port, uint16_t pin, GPIOMode_TypeDef mode)
{
    GPIO_InitTypeDef gpio;
    GPIO_StructInit(&gpio);
    gpio.GPIO_Pin = pin;
    gpio.GPIO_Mode = mode;
    gpio.GPIO_Speed = GPIO_Speed_50MHz;
    GPIO_Init(port, &gpio);
}

W25Q64_Status_t W25Q64_PortInit(void)
{
    RCC_ClocksTypeDef clocks;
    TIM_TimeBaseInitTypeDef timer;
    SPI_InitTypeDef spi;
    uint32_t timer_clock;
    uint32_t spi_clock;
    uint32_t divisor;
    uint32_t spins;

    RCC_GetClocksFreq(&clocks);
    timer_clock = clocks.PCLK1_Frequency;
    if (clocks.PCLK1_Frequency != clocks.HCLK_Frequency) { timer_clock *= 2U; }
    if ((W25Q64_SPI != SPI1) && (W25Q64_SPI != SPI2)) { return W25Q64_INVALID_PARAM; }
    if (!IS_SPI_BAUDRATE_PRESCALER(W25Q64_SPI_PRESCALER)) { return W25Q64_INVALID_PARAM; }
    spi_clock = W25Q64_SPI == SPI1 ? clocks.PCLK2_Frequency : clocks.PCLK1_Frequency;
    divisor = 2UL << ((uint32_t)W25Q64_SPI_PRESCALER >> 3);
    if ((spi_clock / divisor > MAX_SCK_HZ) || (timer_clock < TIMER_HZ) ||
        ((timer_clock % TIMER_HZ) != 0U)) { return W25Q64_INVALID_PARAM; }

    RCC_APB2PeriphClockCmd(W25Q64_GPIO_CLOCKS | RCC_APB2Periph_AFIO, ENABLE);
    /* Set output latch BEFORE switching CS to output to avoid a low pulse. */
    GPIO_SetBits(W25Q64_CS_GPIO_PORT, W25Q64_CS_PIN);
    gpio_init(W25Q64_CS_GPIO_PORT, W25Q64_CS_PIN, GPIO_Mode_Out_PP);
    W25Q64_PIN_REMAP();
    W25Q64_SPI_CLOCK_ENABLE();
    SPI_I2S_DeInit(W25Q64_SPI);
    gpio_init(W25Q64_SCK_GPIO_PORT, W25Q64_SCK_PIN, GPIO_Mode_AF_PP);
    gpio_init(W25Q64_MOSI_GPIO_PORT, W25Q64_MOSI_PIN, GPIO_Mode_AF_PP);
    gpio_init(W25Q64_MISO_GPIO_PORT, W25Q64_MISO_PIN, GPIO_Mode_IN_FLOATING);
    SPI_StructInit(&spi);
    spi.SPI_Direction = SPI_Direction_2Lines_FullDuplex;
    spi.SPI_Mode = SPI_Mode_Master;
    spi.SPI_DataSize = SPI_DataSize_8b;
    spi.SPI_CPOL = SPI_CPOL_Low;
    spi.SPI_CPHA = SPI_CPHA_1Edge;
    spi.SPI_NSS = SPI_NSS_Soft;
    spi.SPI_BaudRatePrescaler = W25Q64_SPI_PRESCALER;
    spi.SPI_FirstBit = SPI_FirstBit_MSB;
    SPI_Init(W25Q64_SPI, &spi);
    SPI_NSSInternalSoftwareConfig(W25Q64_SPI, SPI_NSSInternalSoft_Set);
    SPI_Cmd(W25Q64_SPI, ENABLE);

    RCC_APB1PeriphClockCmd(RCC_APB1Periph_TIM2, ENABLE);
    TIM_DeInit(TIM2);
    TIM_TimeBaseStructInit(&timer);
    timer.TIM_Prescaler = (uint16_t)(timer_clock / TIMER_HZ - 1U);
    timer.TIM_Period = 0xFFFFU;
    timer.TIM_CounterMode = TIM_CounterMode_Up;
    TIM_TimeBaseInit(TIM2, &timer);
    TIM_SetCounter(TIM2, 0U);
    last_tick = 0U;
    elapsed_ms = 0U;
    fraction = 0U;
    bus_fault = 0U;
    TIM_Cmd(TIM2, ENABLE);
    for (spins = 0U; spins < W25Q64_SPI_SPIN_LIMIT; ++spins)
    {
        if (TIM_GetCounter(TIM2) != 0U) { return W25Q64_OK; }
    }
    SPI_Cmd(W25Q64_SPI, DISABLE);
    bus_fault = 1U;
    return W25Q64_TIMEOUT;
}

#else
/* Safe, buildable placeholder until the user supplies wiring. */
W25Q64_Status_t W25Q64_PortInit(void) { return W25Q64_NOT_CONFIGURED; }
void W25Q64_PortSelect(uint8_t active) { (void)active; }
W25Q64_Status_t W25Q64_PortTransfer(uint8_t tx, uint8_t *rx)
{
    (void)tx;
    (void)rx;
    return W25Q64_NOT_CONFIGURED;
}
uint32_t W25Q64_PortNowMs(void) { return 0U; }
uint32_t W25Q64_PortGetClockHz(void) { return 0U; }
#endif
