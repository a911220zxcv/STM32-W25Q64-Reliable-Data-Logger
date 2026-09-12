param(
    [string]$UV4 = 'C:\Keil_v5\UV4\UV4.exe',
    [string]$ArmBin = 'C:\Keil_v5\ARM\ARMCC\bin',
    [switch]$CompileFixture
)
$ErrorActionPreference = 'Stop'
$repo = Split-Path -Parent $PSScriptRoot
Push-Location $repo
try {
    New-Item -ItemType Directory -Force build | Out-Null
    if ($CompileFixture) {
        # Reuse the normal build's startup, SPL and generated scatter file.
        # Replace only project-owned objects and link to a separate fixture output.
        if (-not (Test-Path Objects/project.lnp)) { throw 'Run the default Keil build first' }
        New-Item -ItemType Directory -Force build/keil-fixture | Out-Null
        $sources = @('User/main.c', 'User/stm32f10x_it.c',
                     'User/Drivers/w25q64.c', 'User/Drivers/mpu6050.c',
                     'User/Storage/crc32.c', 'User/Storage/flash_manager.c',
                     'User/Storage/wear_leveling.c', 'User/App/data_logger.c',
                     'User/App/flash_benchmark.c', 'User/App/cli.c',
                     'System/uart_port_stm32.c',
                     'System/w25q64_port_stm32.c',
                     'System/mpu6050_port_stm32.c', 'System/system_time.c',
                     'User/Tests/flash_test.c', 'User/Tests/flash_manager_test.c',
                     'User/Tests/data_logger_test.c',
                     'User/Tests/power_fail_test.c')
        $link = Get-Content Objects/project.lnp -Raw
        $log = @()
        foreach ($source in $sources) {
            $name = [IO.Path]::GetFileNameWithoutExtension($source)
            $object = 'build/keil-fixture/' + $name + '.o'
            $output = & (Join-Path $ArmBin 'armcc.exe') --cpu Cortex-M3 --c99 --strict `
                --diag_error=warning --split_sections -g -O0 --apcs=interwork `
                --preinclude tests/compile_board_config.h -DUSE_STDPERIPH_DRIVER -DSTM32F10X_MD `
                -IStart -IUser -ILibrary -ISystem -IUser/App -IUser/Drivers -IUser/Storage -IUser/Tests `
                -c $source -o $object 2>&1
            $log += $output
            if ($LASTEXITCODE -ne 0) { throw ($output -join "`n") }
            $link = $link.Replace('.\objects\' + $name + '.o', $object)
        }
        $link = $link.Replace('.\Listings\project.map', 'build/keil-fixture/project.map')
        $link = $link.Replace('.\Objects\project.axf', 'build/keil-fixture/project.axf')
        Set-Content build/keil-fixture/project.lnp -Value $link -Encoding ascii
        $output = & (Join-Path $ArmBin 'armlink.exe') --via build/keil-fixture/project.lnp 2>&1
        $log += $output
        $log | Set-Content build/keil-fixture.log
        $log | Write-Output
        if ($LASTEXITCODE -ne 0) { throw 'Fixture link failed' }
        Write-Output 'Configured SPL port and debugger tests compiled and linked successfully (compile-only test sector).'
    } else {
        $log = Join-Path $repo 'build/keil-build.log'
        if (Test-Path -LiteralPath $log) { Remove-Item -LiteralPath $log }
        $arguments = '-b project.uvprojx -t "Target 1" -o build\keil-build.log'
        $process = Start-Process -FilePath $UV4 -ArgumentList $arguments -WorkingDirectory $repo `
            -WindowStyle Hidden -PassThru
        if (-not $process.WaitForExit(60000)) { throw 'Keil is still running; inspect toolchain and build log.' }
        if (-not (Test-Path -LiteralPath $log)) { throw 'Keil did not produce a build log' }
        $result = Get-Content -LiteralPath $log -Raw
        Write-Output $result
        if (($process.ExitCode -ne 0) -or ($result -notmatch '0 Error\(s\), 0 Warning\(s\)')) {
            throw 'Keil build failed or produced warnings'
        }
    }
} finally {
    Pop-Location
}
