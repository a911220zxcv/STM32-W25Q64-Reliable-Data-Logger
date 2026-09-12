# Changelog

All notable changes to this project are documented in this file.

## [1.0.0] - 2026-09-13

First production-ready portfolio release.

### Added

- W25Q64BV SPI NOR driver with timeout, range/alignment validation, page splitting and read-back verification.
- Fixed 28-byte sensor record codec with CRC-32/ISO-HDLC.
- 10 Hz MPU6050 data logger driven by TIM4 uptime.
- 298,716-record circular buffer across 2046 log sectors.
- Round-robin sector allocator with RAM erase-count diagnostics.
- Dual A/B metadata with generation, CRC and boot-time tail replay.
- Full-scan recovery when both metadata copies are invalid.
- USART1 command-line interface for logging, inspection, verification and diagnostics.
- 4 KiB read/program/erase benchmark.
- Host-side NOR, sensor and UART models with fault injection.
- Five debug-only reset injection points at durability boundaries.
- GitHub Actions workflow for host regression tests.
- Trimmed vendored SPL and startup sources to the modules used by this target.
- MIT License for project-specific code and documentation, with vendor-code exceptions documented separately.

### Validated

- GCC host regression with warnings treated as errors.
- ARMCC 5.06 target and test-enabled fixture with zero errors and zero warnings.
- Physical JEDEC, read/write/erase, cross-page, MPU6050, circular wrap, wear diagnostics and UART CLI tests.
- Physical benchmark: 89 KiB/s read, 36 KiB/s program and 90 ms sector erase + verify.
- Complete power removal and reboot recovery of 62 records, followed by sequence continuation from 62.
