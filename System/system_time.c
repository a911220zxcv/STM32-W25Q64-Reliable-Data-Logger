#include "system_time.h"
#include "stm32f10x.h"

#define SYSTEM_TIME_COUNTER_HZ 1000000UL
#define SYSTEM_TIME_PERIOD_US  1000UL

static volatile uint32_t uptime_ms;

SystemTimeStatus_t SystemTime_Init(void)
{
    RCC_ClocksTypeDef clocks;
    TIM_TimeBaseInitTypeDef timer;
    NVIC_InitTypeDef nvic;
    uint32_t timer_clock;

    RCC_GetClocksFreq(&clocks);
    timer_clock = clocks.PCLK1_Frequency;
    if (clocks.PCLK1_Frequency != clocks.HCLK_Frequency) { timer_clock *= 2U; }
    if ((timer_clock < SYSTEM_TIME_COUNTER_HZ) ||
        ((timer_clock % SYSTEM_TIME_COUNTER_HZ) != 0U))
    {
        return SYSTEM_TIME_INVALID_CLOCK;
    }

    RCC_APB1PeriphClockCmd(RCC_APB1Periph_TIM4, ENABLE);
    TIM_DeInit(TIM4);
    TIM_TimeBaseStructInit(&timer);
    timer.TIM_Prescaler = (uint16_t)(timer_clock / SYSTEM_TIME_COUNTER_HZ - 1U);
    timer.TIM_Period = (uint16_t)(SYSTEM_TIME_PERIOD_US - 1U);
    timer.TIM_CounterMode = TIM_CounterMode_Up;
    TIM_TimeBaseInit(TIM4, &timer);
    TIM_ClearITPendingBit(TIM4, TIM_IT_Update);
    TIM_ITConfig(TIM4, TIM_IT_Update, ENABLE);

    nvic.NVIC_IRQChannel = TIM4_IRQn;
    nvic.NVIC_IRQChannelPreemptionPriority = 1U;
    nvic.NVIC_IRQChannelSubPriority = 0U;
    nvic.NVIC_IRQChannelCmd = ENABLE;
    NVIC_Init(&nvic);

    uptime_ms = 0U;
    TIM_SetCounter(TIM4, 0U);
    TIM_Cmd(TIM4, ENABLE);
    return SYSTEM_TIME_OK;
}

uint32_t SystemTime_GetMs(void)
{
    return uptime_ms;
}

void SystemTime_TickISR(void)
{
    ++uptime_ms;
}
