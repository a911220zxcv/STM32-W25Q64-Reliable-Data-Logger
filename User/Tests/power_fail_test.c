#include "config.h"
#include "power_fail_test.h"

#if POWER_FAIL_TEST_ENABLE

#if !defined(POWER_FAIL_TEST_HOST_MODEL)
#include "stm32f10x.h"
#endif

volatile uint32_t g_power_fail_test_point = POWER_FAIL_POINT_NONE;
volatile uint32_t g_power_fail_test_last_point = POWER_FAIL_POINT_NONE;
volatile uint32_t g_power_fail_test_trigger_count;

#if defined(POWER_FAIL_TEST_HOST_MODEL)
volatile uint8_t g_power_fail_test_host_reset;
#endif

uint8_t PowerFailTest_IsArmed(PowerFailTestPoint_t point)
{
    return (uint8_t)((point != POWER_FAIL_POINT_NONE) &&
                     (g_power_fail_test_point == (uint32_t)point));
}

uint8_t PowerFailTest_Hook(PowerFailTestPoint_t point)
{
    if (!PowerFailTest_IsArmed(point)) { return 0U; }
    /* Clear first so RAM-retaining debugger reset modes cannot reset-loop. */
    g_power_fail_test_point = POWER_FAIL_POINT_NONE;
    g_power_fail_test_last_point = (uint32_t)point;
    ++g_power_fail_test_trigger_count;
#if defined(POWER_FAIL_TEST_HOST_MODEL)
    g_power_fail_test_host_reset = 1U;
    return 1U;
#else
    NVIC_SystemReset();
    while (1) { }
#endif
}

#endif
