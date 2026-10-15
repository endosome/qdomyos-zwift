# Windows PowerShell 5.1 / native WinRT. Probe requires explicit write approval.
param(
    [ValidateSet('scan', 'info', 'probe', 'control-test')][string]$Command = 'scan',
    [ValidateRange(1, 1800)][int]$Timeout = 15,
    [string]$Device = '',
    [ValidateSet('unknown', 'public', 'random')][string]$AddressType = 'unknown',
    [switch]$ConfirmDiagnostics,
    [string]$Confirmation = '',
    [string]$ExpectedName = '',
    [switch]$Bridge,
    [switch]$ConfirmMotor,
    [switch]$ObserveFtms,
    [switch]$StatusHeartRateZero,
    [switch]$StatusDataSport,
    [switch]$MeasureSleepTimer,
    [string]$TargetSequence = '',
    [ValidateSet('start-stop', 'speed-12', 'speed-15', 'speed-20', 'speed-30', 'speed-40', 'speed-50', 'speed-60', 'workout',
                 'incline-1', 'incline-2', 'incline-3', 'incline-4', 'incline-5', 'incline-6', 'incline-7', 'incline-8', 'pause', 'incline-timing-4', 'incline-timing-8', 'incline-measure-4', 'incline-measure-8', 'ready-idle', 'ready-wake')]
    [string]$Trial = 'start-stop'
)
$ErrorActionPreference = 'Stop'
if (([int][bool]$StatusHeartRateZero + [int][bool]$StatusDataSport + [int][bool]$MeasureSleepTimer) -gt 1) { throw 'Select only one experimental status mode' }
if ($MeasureSleepTimer -and $Command -ne 'probe') { throw 'MeasureSleepTimer applies only to nonmotor probe' }
if ($StatusDataSport -and $Command -ne 'probe') { throw 'StatusDataSport applies only to nonmotor probe' }
if ($StatusHeartRateZero -and $Command -ne 'probe') { throw 'StatusHeartRateZero applies only to nonmotor probe' }
if ($Command -ne 'control-test' -and ($Trial -ne 'start-stop' -or $TargetSequence -ne '')) { throw 'Trial/TargetSequence applies only to control-test' }
if ($Command -eq 'control-test' -and (($Trial -eq 'workout') -ne ($TargetSequence -ne ''))) { throw 'Workout requires TargetSequence; other trials reject it' }
if ($ObserveFtms -and $Command -ne 'probe' -and $Command -ne 'control-test') { throw 'ObserveFtms applies only to probe/control-test' }
[Console]::OutputEncoding = New-Object System.Text.UTF8Encoding($false)
if (($Command -eq 'scan' -or $Command -eq 'info') -and $Timeout -gt 60) { throw 'Scan/info timeout is limited to 60 seconds' }
if ($Command -eq 'probe' -and $Timeout -gt $(if ($MeasureSleepTimer) { 900 } else { 600 })) {
    throw 'Probe timeout exceeds the command-specific bound (600 or 900 seconds)'
}
if ($Command -eq 'control-test' -and $Trial -eq 'workout' -and $Timeout -gt 1800) { throw 'Workout timeout is limited to 1800 seconds' }
if ($Command -eq 'control-test' -and $Trial -ne 'workout' -and $Timeout -gt 60) {
    # Existing bounded motor plans have their own fixed 120/240 second budgets.
    $Timeout = 240
}
if ($Command -eq 'control-test') {
    if ($Trial -eq 'ready-idle' -or $Trial -eq 'ready-wake') { throw 'READY trials retired: W1 entered countdown and stop was not verified. No Bluetooth I/O.' }
    if (-not $ConfirmMotor -or $ConfirmDiagnostics -or $Confirmation -cne 'MOTOR' -or
        $ExpectedName -eq '' -or $Device -eq '' -or -not $Bridge) {
        @{ command = 'control-test'; backend = 'windows-winrt'; status = 'error';
           error = 'Control trial requires Device/ExpectedName, Bridge, ConfirmMotor and operator Confirmation MOTOR. No writes were sent.' } |
            ConvertTo-Json -Compress
        exit 1
    }
} elseif ($ConfirmMotor) {
    throw 'ConfirmMotor applies only to control-test'
} elseif ($Command -eq 'probe') {
    if (-not $ConfirmDiagnostics -or $Confirmation -cne 'NONMOTOR' -or $ExpectedName -eq '' -or $Device -eq '') {
        @{ command = 'probe'; backend = 'windows-winrt'; status = 'error';
           error = 'Probe requires an explicit Device/ExpectedName, ConfirmDiagnostics and operator Confirmation NONMOTOR. No writes were sent.' } |
            ConvertTo-Json
        exit 1
    }
} elseif ($ConfirmDiagnostics -or $Confirmation -ne '' -or $ExpectedName -ne '' -or $Bridge) {
    throw 'Probe approval options are valid only for probe'
}
# WinRT service discovery stalled in the default STA host on this machine.
# Re-enter a fresh MTA host; keep the same validated, read-only arguments.
if ([Threading.Thread]::CurrentThread.GetApartmentState() -ne [Threading.ApartmentState]::MTA) {
    $arguments = @('-Mta', '-NoProfile', '-NonInteractive', '-ExecutionPolicy', 'Bypass',
                   '-File', $PSCommandPath, '-Command', $Command, '-Timeout', "$Timeout")
    if ($Device -ne '') { $arguments += @('-Device', $Device) }
    if ($AddressType -ne 'unknown') { $arguments += @('-AddressType', $AddressType) }
    if ($ConfirmDiagnostics) { $arguments += '-ConfirmDiagnostics' }
    if ($Confirmation -ne '') { $arguments += @('-Confirmation', $Confirmation) }
    if ($ExpectedName -ne '') { $arguments += @('-ExpectedName', $ExpectedName) }
    if ($Bridge) { $arguments += '-Bridge' }
    if ($ConfirmMotor) { $arguments += '-ConfirmMotor' }
    if ($ObserveFtms) { $arguments += '-ObserveFtms' }
    if ($StatusHeartRateZero) { $arguments += '-StatusHeartRateZero' }
    if ($StatusDataSport) { $arguments += '-StatusDataSport' }
    if ($MeasureSleepTimer) { $arguments += '-MeasureSleepTimer' }
    if ($Command -eq 'control-test') {
        $arguments += @('-Trial', $Trial)
        if ($Trial -eq 'workout') { $arguments += @('-TargetSequence', $TargetSequence) }
    }
    & (Join-Path $PSHOME 'powershell.exe') @arguments
    exit $LASTEXITCODE
}
try {
    Add-Type -AssemblyName System.Runtime.WindowsRuntime
    $metadata = Join-Path $env:WINDIR 'System32\WinMetadata'
    $references = @(
        ([System.Diagnostics.Stopwatch].Assembly.Location),
        ([System.Linq.Enumerable].Assembly.Location),
        (Join-Path $metadata 'Windows.Devices.winmd'),
        (Join-Path $metadata 'Windows.Foundation.winmd'),
        (Join-Path $metadata 'Windows.Storage.winmd')
    )
    foreach ($name in @('System.Runtime.WindowsRuntime', 'System.Runtime', 'System.Threading.Tasks', 'System.Runtime.InteropServices.WindowsRuntime')) {
        $assembly = @(Get-ChildItem (Join-Path $env:WINDIR "Microsoft.NET\assembly\GAC_MSIL\$name") -Recurse -Filter "$name.dll")
        if ($assembly.Count -eq 0) { throw "Required .NET Framework assembly missing: $name" }
        $references += $assembly[0].FullName
    }
    $parameters = New-Object System.CodeDom.Compiler.CompilerParameters
    foreach ($reference in $references) { $null = $parameters.ReferencedAssemblies.Add($reference) }
    $parameters.GenerateInMemory = $true
    Add-Type -TypeDefinition (Get-Content -Raw (Join-Path $PSScriptRoot 'windows-diagnostics.cs')) -CompilerParameters $parameters
    if ($Command -eq 'scan') {
        if ($Device -ne '' -or $AddressType -ne 'unknown') { throw 'scan does not accept Device/AddressType' }
        $report = [TreadmillDiagnostics.Reader]::Scan($Timeout)
    } elseif ($Command -eq 'info') {
        $report = [TreadmillDiagnostics.Reader]::Info($Device, $AddressType, $Timeout)
    } elseif ($Command -eq 'probe') {
        $report = [TreadmillDiagnostics.Reader]::Probe($Device, $AddressType, $ExpectedName, $Timeout, $true, [bool]$Bridge, [bool]$ObserveFtms, [bool]$StatusHeartRateZero, [bool]$StatusDataSport, [bool]$MeasureSleepTimer)
    } else {
        $report = [TreadmillDiagnostics.Reader]::ControlTest($Device, $AddressType, $ExpectedName, $true, $Trial, [bool]$ObserveFtms, $TargetSequence, $Timeout)
    }
    if ($Command -eq 'control-test') { $report | ConvertTo-Json -Depth 12 -Compress }
    else { $report | ConvertTo-Json -Depth 12 }
    if ($report.status -ne 'complete') { exit 1 }
} catch {
    @{ command = $Command; backend = 'windows-winrt'; status = 'error'; error = $_.Exception.Message } |
        ConvertTo-Json -Depth 3 -Compress
    exit 1
}
