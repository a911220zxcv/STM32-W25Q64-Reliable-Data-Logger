/* DESTRUCTIVE TEST COMPILE FIXTURE ONLY. Wiring now comes from User/config.h.
 * Only tests/build_keil.ps1 -CompileFixture preincludes this file.
 * The test address is synthetic, not confirmed disposable. Do not flash it. */
#define FLASH_TEST_ENABLE 1
#define FLASH_TEST_SECTOR_ADDR 0x2000UL
#define FLASH_MANAGER_TEST_ENABLE 1
#define FLASH_MANAGER_TEST_SECTOR_INDEX 0UL
#define DATA_LOGGER_TEST_ENABLE 1
#define CLI_TEST_ENABLE 1
#define POWER_FAIL_TEST_ENABLE 1
/* Compile the confirmed USART1 branch. Destructive test macros still make
 * this fixture image unsafe to flash. */
#define UART_BOARD_CONFIGURED 1
