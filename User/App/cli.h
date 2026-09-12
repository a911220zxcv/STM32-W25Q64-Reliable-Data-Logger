#ifndef CLI_H
#define CLI_H

#include <stdint.h>

typedef enum
{
    CLI_OK = 0,
    CLI_ERROR,
    CLI_UART_ERROR,
    CLI_INVALID_PARAM,
    CLI_NOT_INITIALIZED
} CLIStatus_t;

/* Initializes the configured UART and prints the boot banner. */
CLIStatus_t CLI_Init(void);
/* Polls input and advances at most one long-running output item. */
CLIStatus_t CLI_Process(void);

#if defined(CLI_TEST_ENABLE) && CLI_TEST_ENABLE
/* Host-only direct line entry; input excludes CR/LF. */
CLIStatus_t CLI_TestExecuteLine(const char *line);
#endif

#endif
