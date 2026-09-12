# Third-Party Notices

This repository contains vendor support code that predates the data logger implementation.

## STMicroelectronics

The `Library/` directory contains STM32F10x Standard Peripheral Library V3.5.0 sources. Parts of `Start/` also contain STM32F10x CMSIS device support and startup files supplied by STMicroelectronics.

These files retain their original copyright and notice blocks. Their use and redistribution remain subject to the terms stated in those files and the applicable STMicroelectronics software package.

## Arm CMSIS

`Start/core_cm3.c` and `Start/core_cm3.h` contain Arm CMSIS Cortex-M3 support code. These files retain the original Arm copyright, use conditions and warranty disclaimer.

## Project Code

The data logger code under `User/`, the STM32 hardware ports added under `System/`, the host tests under `tests/`, and the project documentation are separate from the vendor support code above.

Except for files that retain a different copyright or license notice, the project-specific code and documentation are licensed under the repository's top-level MIT License. The MIT License does not replace or override the notices and terms retained in third-party files.
