#ifndef CONFIG_H
#define CONFIG_H

/* W25Q64BV, SPI1 wiring supplied by the user (2026-09-08).
 * Module: CS->PA4, CLK->PA5, DO->PA6, DI->PA7, VCC->3.3V, GND->GND.
 * Set to 0 for a build that does not touch board peripherals. */
#ifndef W25Q64_BOARD_CONFIGURED
#define W25Q64_BOARD_CONFIGURED 1
#endif
#if W25Q64_BOARD_CONFIGURED
#define W25Q64_SPI SPI1
#define W25Q64_SPI_CLOCK_ENABLE() RCC_APB2PeriphClockCmd(RCC_APB2Periph_SPI1, ENABLE)
#define W25Q64_GPIO_CLOCKS RCC_APB2Periph_GPIOA
#define W25Q64_CS_GPIO_PORT GPIOA
#define W25Q64_CS_PIN GPIO_Pin_4
#define W25Q64_SCK_GPIO_PORT GPIOA
#define W25Q64_SCK_PIN GPIO_Pin_5
#define W25Q64_MISO_GPIO_PORT GPIOA
#define W25Q64_MISO_PIN GPIO_Pin_6
#define W25Q64_MOSI_GPIO_PORT GPIOA
#define W25Q64_MOSI_PIN GPIO_Pin_7
#define W25Q64_PIN_REMAP() GPIO_PinRemapConfig(GPIO_Remap_SPI1, DISABLE)
/* The system config targets HSE=8 MHz and PCLK2=72 MHz, so /8 gives a
 * runtime-calculated 9 MHz SCK.  UART, JEDEC ID, program/erase/verify and the
 * benchmark have passed on this board; the physical waveform was not measured. */
#define W25Q64_SPI_PRESCALER SPI_BaudRatePrescaler_8
/* This six-pin module does not expose WP# or HOLD#.  Successful write/erase
 * testing confirms that both are functionally deasserted in this assembly;
 * the module's internal pull-up topology is not known. */
#if !defined(W25Q64_SPI) || !defined(W25Q64_SPI_CLOCK_ENABLE) || \
    !defined(W25Q64_GPIO_CLOCKS) || !defined(W25Q64_CS_GPIO_PORT) || \
    !defined(W25Q64_CS_PIN) || !defined(W25Q64_SCK_GPIO_PORT) || \
    !defined(W25Q64_SCK_PIN) || !defined(W25Q64_MISO_GPIO_PORT) || \
    !defined(W25Q64_MISO_PIN) || !defined(W25Q64_MOSI_GPIO_PORT) || \
    !defined(W25Q64_MOSI_PIN) || !defined(W25Q64_PIN_REMAP) || \
    !defined(W25Q64_SPI_PRESCALER)
#error "Complete the W25Q64 board macros in User/config.h"
#endif
#endif

/* MPU6050 hardware I2C wiring supplied by the user's reference diagram.
 * Module: SCL->PB10, SDA->PB11, VCC->3.3V, GND->GND.  The module's AD0
 * arrangement selects the 7-bit address used by the reference project. */
#ifndef MPU6050_BOARD_CONFIGURED
#define MPU6050_BOARD_CONFIGURED 1
#endif
#if MPU6050_BOARD_CONFIGURED
#define MPU6050_I2C I2C2
#define MPU6050_I2C_CLOCK_ENABLE() RCC_APB1PeriphClockCmd(RCC_APB1Periph_I2C2, ENABLE)
#define MPU6050_I2C_FORCE_RESET() RCC_APB1PeriphResetCmd(RCC_APB1Periph_I2C2, ENABLE)
#define MPU6050_I2C_RELEASE_RESET() RCC_APB1PeriphResetCmd(RCC_APB1Periph_I2C2, DISABLE)
#define MPU6050_GPIO_CLOCKS RCC_APB2Periph_GPIOB
#define MPU6050_GPIO_PORT GPIOB
#define MPU6050_SCL_PIN GPIO_Pin_10
#define MPU6050_SDA_PIN GPIO_Pin_11
#define MPU6050_I2C_ADDRESS_7BIT 0x68U
#define MPU6050_I2C_CLOCK_HZ 100000UL
#if !defined(MPU6050_I2C) || !defined(MPU6050_I2C_CLOCK_ENABLE) || \
    !defined(MPU6050_I2C_FORCE_RESET) || !defined(MPU6050_I2C_RELEASE_RESET) || \
    !defined(MPU6050_GPIO_CLOCKS) || !defined(MPU6050_GPIO_PORT) || \
    !defined(MPU6050_SCL_PIN) || !defined(MPU6050_SDA_PIN) || \
    !defined(MPU6050_I2C_ADDRESS_7BIT) || !defined(MPU6050_I2C_CLOCK_HZ)
#error "Complete MPU6050 board macros in User/config.h"
#endif
#endif

#define MPU6050_I2C_TIMEOUT_MS 10UL
#define MPU6050_I2C_SPIN_LIMIT 100000UL

/* Phase 3 application cadence.  The timer is a continuous 32-bit millisecond
 * uptime counter, and logger scheduling remains correct across counter wrap. */
#define LOGGER_SAMPLE_PERIOD_MS 100UL

/* Phase 7 UART wiring confirmed by the user (2026-09-12).
 * USART1 default map: PA9 TX -> USB-TTL RX, PA10 RX <- USB-TTL TX.
 * USB-TTL uses 3.3 V logic and shares GND with the STM32. */
#ifndef UART_BOARD_CONFIGURED
#define UART_BOARD_CONFIGURED 1
#endif
#if UART_BOARD_CONFIGURED
#define UART_USART USART1
#define UART_USART_CLOCK_ENABLE() \
    RCC_APB2PeriphClockCmd(RCC_APB2Periph_USART1, ENABLE)
#define UART_GPIO_CLOCK_ENABLE() \
    RCC_APB2PeriphClockCmd(RCC_APB2Periph_GPIOA | RCC_APB2Periph_AFIO, ENABLE)
#define UART_GPIO_PORT GPIOA
#define UART_TX_PIN GPIO_Pin_9
#define UART_RX_PIN GPIO_Pin_10
#define UART_PIN_REMAP() GPIO_PinRemapConfig(GPIO_Remap_USART1, DISABLE)
#endif
#define UART_BAUD_RATE       115200UL
#define UART_TX_SPIN_LIMIT   100000UL
#if UART_BOARD_CONFIGURED
#if !defined(UART_USART) || !defined(UART_USART_CLOCK_ENABLE) || \
    !defined(UART_GPIO_CLOCK_ENABLE) || !defined(UART_GPIO_PORT) || \
    !defined(UART_TX_PIN) || !defined(UART_RX_PIN) || \
    !defined(UART_PIN_REMAP)
#error "Configure UART instance, clocks, GPIO pins and remap in User/config.h"
#endif
#endif

#ifndef CLI_TEST_ENABLE
#define CLI_TEST_ENABLE 0
#endif
#define CLI_LINE_SIZE          80UL
#define CLI_MAX_ARGS           4UL
#define CLI_RX_BUDGET          16UL
#define CLI_FLASH_READ_MAX     64UL
#define CLI_WEAR_DISPLAY_COUNT 8UL
#define CLI_WEAR_DISPLAY_MAX   16UL

/* W25Q64BV datasheet Rev E, pp.49/53: tPUW max 10 ms; tPP max 3 ms;
 * tSE max 400 ms over endurance range; 64 KiB tBE max 1000 ms.
 * Policy includes margin; power-up 20 ms also covers timer quantization. */
#define W25Q64_EXPECTED_JEDEC_ID      0xEF4017UL
#define W25Q64_POWER_UP_MS           20UL
#define W25Q64_WAKE_UP_MS            1UL
#define W25Q64_READY_TIMEOUT_MS      3000UL
#define W25Q64_PROGRAM_TIMEOUT_MS    10UL
#define W25Q64_SECTOR_TIMEOUT_MS     1000UL
#define W25Q64_BLOCK_TIMEOUT_MS      3000UL
#define W25Q64_SPI_TIMEOUT_MS        10UL
/* Secondary finite bounds also catch a stopped timer. Not calibrated delays. */
#define W25Q64_POLL_LIMIT            1000000UL
#define W25Q64_SPI_SPIN_LIMIT        100000UL

/* Hardware tests remain disabled by default. Choose a disposable sector.
 * Enabling tests without selecting a safe sector produces a build error.
 * 0x000000..0x001FFF contains redundant metadata and is always protected. */
#ifndef FLASH_TEST_ENABLE
#define FLASH_TEST_ENABLE 0
#endif

#define FLASH_TEST_SECTOR_ADDR 0x2000UL

#if FLASH_TEST_ENABLE
#ifndef FLASH_TEST_SECTOR_ADDR
#error "Set FLASH_TEST_SECTOR_ADDR to a disposable aligned sector"
#endif
#if (FLASH_TEST_SECTOR_ADDR < 0x2000UL) || \
    (FLASH_TEST_SECTOR_ADDR > 0x7FF000UL) || \
    ((FLASH_TEST_SECTOR_ADDR % 4096UL) != 0)
#error "Test sector must be aligned and outside the two reserved sectors"
#endif
#endif

/* Phase 2 debugger tests are destructive and remain disabled by default.
 * Sector index 0 maps to 0x002000 and must contain no data to preserve. */
#ifndef FLASH_MANAGER_TEST_ENABLE
#define FLASH_MANAGER_TEST_ENABLE 0
#endif
#define FLASH_MANAGER_TEST_SECTOR_INDEX 0UL
#if FLASH_MANAGER_TEST_ENABLE && \
    (FLASH_MANAGER_TEST_SECTOR_INDEX >= 2046UL)
#error "Flash Manager test sector index is outside the log area"
#endif

/* Destructive debugger mailbox. Enable only while reproducing Phase 3..6
 * acceptance commands; no command runs automatically at boot. */
#ifndef DATA_LOGGER_TEST_ENABLE
#define DATA_LOGGER_TEST_ENABLE 0
#endif

/* Debug-only one-shot reset injection. Keep disabled in the normal image. */
#ifndef POWER_FAIL_TEST_ENABLE
#define POWER_FAIL_TEST_ENABLE 0
#endif

#endif
