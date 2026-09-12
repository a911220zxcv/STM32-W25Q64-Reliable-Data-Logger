#ifndef POWER_FAIL_TEST_H
#define POWER_FAIL_TEST_H

#include "config.h"
#include <stdint.h>

typedef enum
{
    POWER_FAIL_POINT_NONE = 0,
    POWER_FAIL_POINT_RECORD_PROGRAMMED = 1,
    POWER_FAIL_POINT_METADATA_ERASED = 2,
    POWER_FAIL_POINT_METADATA_PARTIAL_PROGRAMMED = 3,
    POWER_FAIL_POINT_METADATA_PROGRAMMED = 4,
    POWER_FAIL_POINT_LOG_SECTOR_ERASED = 5
} PowerFailTestPoint_t;

#if POWER_FAIL_TEST_ENABLE

/* Set g_power_fail_test_point from Watch while the target is stopped, then
 * resume. The matching hook clears the arm value before requesting reset. */
extern volatile uint32_t g_power_fail_test_point;
extern volatile uint32_t g_power_fail_test_last_point;
extern volatile uint32_t g_power_fail_test_trigger_count;

#if defined(POWER_FAIL_TEST_HOST_MODEL)
extern volatile uint8_t g_power_fail_test_host_reset;
#endif

uint8_t PowerFailTest_IsArmed(PowerFailTestPoint_t point);
/* On hardware, a matching hook does not return. The host model returns 1 so
 * its caller can abort the interrupted transaction immediately. */
uint8_t PowerFailTest_Hook(PowerFailTestPoint_t point);

#else

#define PowerFailTest_IsArmed(point) (0U)
#define PowerFailTest_Hook(point)    (0U)

#endif

#endif
