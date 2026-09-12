#ifndef SYSTEM_TIME_H
#define SYSTEM_TIME_H

#include <stdint.h>

typedef enum
{
    SYSTEM_TIME_OK = 0,
    SYSTEM_TIME_INVALID_CLOCK
} SystemTimeStatus_t;

/* Configure TIM4 as a continuous 1 ms application uptime counter. */
SystemTimeStatus_t SystemTime_Init(void);
uint32_t SystemTime_GetMs(void);
/* Called only by TIM4_IRQHandler. */
void SystemTime_TickISR(void);

#endif
