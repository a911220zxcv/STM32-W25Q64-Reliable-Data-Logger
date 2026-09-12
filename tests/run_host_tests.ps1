param([string]$Compiler = 'gcc')
$ErrorActionPreference = 'Stop'
$repo = Split-Path -Parent $PSScriptRoot
Push-Location $repo
try {
    New-Item -ItemType Directory -Force build | Out-Null
    & $Compiler -std=c99 -Wall -Wextra -Werror -Wconversion -Wshadow -pedantic -O2 `
        -IUser -IUser/App -IUser/Drivers -IUser/Storage -IUser/Tests -ISystem `
        -DFLASH_TEST_ENABLE=1 -DFLASH_TEST_SECTOR_ADDR=0x2000UL `
        -DFLASH_MANAGER_TEST_ENABLE=1 -DFLASH_MANAGER_TEST_SECTOR_INDEX=0UL `
        -DDATA_LOGGER_TEST_ENABLE=1 -DCLI_TEST_ENABLE=1 `
        -DPOWER_FAIL_TEST_ENABLE=1 -DPOWER_FAIL_TEST_HOST_MODEL=1 `
        User/Drivers/w25q64.c User/Drivers/mpu6050.c `
        User/Storage/crc32.c User/Storage/flash_manager.c `
        User/Storage/wear_leveling.c User/App/data_logger.c `
        User/App/flash_benchmark.c User/App/cli.c `
        User/Tests/flash_test.c User/Tests/flash_manager_test.c `
        User/Tests/data_logger_test.c User/Tests/power_fail_test.c `
        tests/test_w25q64.c `
        -o build/test_w25q64.exe
    if ($LASTEXITCODE -ne 0) { throw 'Host compilation failed' }
    & ./build/test_w25q64.exe
    if ($LASTEXITCODE -ne 0) { throw 'Host tests failed' }
} finally {
    Pop-Location
}
