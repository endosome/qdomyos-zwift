# Offline only. Run with Windows PowerShell -Mta; no BLE API is invoked.
$ErrorActionPreference = 'Stop'
if ([Threading.Thread]::CurrentThread.GetApartmentState() -ne [Threading.ApartmentState]::MTA) {
    throw 'Run this test script with powershell.exe -Mta'
}
$helper = Join-Path $PSScriptRoot '..\windows-diagnostics.ps1'
$invalid = & $helper -Command info -Device invalid | ConvertFrom-Json
if ($LASTEXITCODE -ne 1 -or -not $invalid.error.Contains('explicit AA:BB:CC:DD:EE:FF')) {
    throw 'Compilation/address validation failed'
}
# Add-Type from the helper remains in this MTA process for pure framing checks.
$expected = @{ 0x5002 = '0250025203'; 0x5003 = '0250035303'; 0x51 = '02515103' }
foreach ($query in $expected.Keys) {
    $hex = [BitConverter]::ToString([TreadmillDiagnostics.Reader]::FrameForQuery($query)).Replace('-', '').ToLowerInvariant()
    if ($hex -ne $expected[$query]) { throw "Wrong nonmotor frame for $query" }
}
foreach ($query in @(0x53, 0x5300, 0x5301, 0x5302, 0x5303, 0x5306, 0x5001, 0x5005, 0x52, 0xffff)) {
    $rejected = $false
    try { $null = [TreadmillDiagnostics.Reader]::FrameForQuery($query) }
    catch { $rejected = $true }
    if (-not $rejected) { throw "Forbidden query accepted: $query" }
}
$hex = [BitConverter]::ToString([TreadmillDiagnostics.Reader]::FrameForQuery(0x51, $true)).Replace('-', '').ToLowerInvariant()
if ($hex -ne '0251005103') { throw 'Wrong zero-heart-rate status frame' }
foreach ($query in @(0x53, 0x5300, 0x5301, 0x5302, 0x5303, 0x5306, 0x52)) {
    $rejected = $false
    try { $null = [TreadmillDiagnostics.Reader]::FrameForQuery($query, $true) } catch { $rejected = $true }
    if (-not $rejected) { throw 'Zero-heart-rate option admitted a motor command' }
}
$rejected = $false
try { & $helper -Command control-test -Device invalid -StatusHeartRateZero } catch { $rejected = $_.Exception.Message.Contains('only to nonmotor probe') }
if (-not $rejected) { throw 'Zero-heart-rate switch admitted a motor session' }
$rejected = $false
try { $null = [TreadmillDiagnostics.Reader]::Probe('invalid', 'public', 'fixture', 15, $false) }
catch { $rejected = $_.Exception.Message.Contains('approval/name required') }
if (-not $rejected) { throw 'C# approval gate failed' }
foreach ($phrase in @('', 'nonmotor', 'NO')) {
    $refused = & $helper -Command probe -Device 'invalid' -ExpectedName fixture -ConfirmDiagnostics -Confirmation $phrase | ConvertFrom-Json
    if ($LASTEXITCODE -ne 1 -or -not $refused.error.Contains('operator Confirmation NONMOTOR')) {
        throw 'PowerShell confirmation gate failed'
    }
}
$longProbe = & $helper -Command probe -Device invalid -ExpectedName fixture -ConfirmDiagnostics -Confirmation NONMOTOR -Timeout 600 | ConvertFrom-Json
if ($LASTEXITCODE -ne 1 -or -not $longProbe.error.Contains('explicit AA:BB:CC:DD:EE:FF')) {
    throw 'Bounded long nonmotor probe did not reach address validation'
}
$rejected = $false
try { $null = [TreadmillDiagnostics.Reader]::Probe('invalid', 'public', 'fixture', 601, $true) }
catch { $rejected = $_.Exception.Message.Contains('bounded scope') }
if (-not $rejected) { throw 'Nonmotor session accepted more than 600 seconds' }
function New-PreparedGate([string]$trial = 'start-stop') {
    $gate = New-Object TreadmillDiagnostics.MotorTestGate -ArgumentList $trial
    $gate.Observe([TreadmillDiagnostics.MotorTestGate]::Parse('0250023c0a006403'), 0)
    $gate.Observe([TreadmillDiagnostics.MotorTestGate]::Parse('0250030800025903'), 0)
    foreach ($time in @(0, 100, 200)) {
        $gate.Observe([TreadmillDiagnostics.MotorTestGate]::Parse('0251005103'), $time)
    }
    return $gate
}
function Assert-Rejected($gate, $hex, $time) {
    $rejected = $false
    try { $null = $gate.Approve($hex, $time) } catch { $rejected = $true }
    if (-not $rejected) { throw "Motor gate accepted forbidden frame $hex at $time" }
}
$start = '02530100000000000000005203'
$target = '0253020a005b03'
$stop = '0253035003'
$gate = New-PreparedGate
$null = $gate.Approve($start, 300)
$null = $gate.Approve($target, 400)
Assert-Rejected $gate $start 500
Assert-Rejected $gate $target 500
Assert-Rejected $gate '0253020b005a03' 500
if (-not $gate.NeedsStop(1800)) { throw 'Stale motor status did not trigger stop' }
$null = $gate.Approve($stop, 1800)
if (-not $gate.StopAttempted) { throw 'Stop attempt not recorded' }
Assert-Rejected $gate $target 1800
Assert-Rejected $gate $stop 1800
$gate = New-PreparedGate
Assert-Rejected $gate $start 1800
Assert-Rejected $gate '0250015103' 300
Assert-Rejected $gate '0253020a005a03' 300
Assert-Rejected (New-Object TreadmillDiagnostics.MotorTestGate) $start 300
$gate = New-PreparedGate
$null = $gate.Approve($start, 300)
foreach ($time in @(1000, 2000, 3000, 4000, 5000, 6000, 7000, 8000, 9000, 10000, 11000, 12000)) {
    $gate.Observe([TreadmillDiagnostics.MotorTestGate]::Parse('0251005103'), $time)
}
if (-not $gate.NeedsStop(12300)) { throw 'Independent 12-second watchdog failed' }
$gate = New-PreparedGate
$null = $gate.Approve($start, 300)
$gate.Observe([TreadmillDiagnostics.MotorTestGate]::Parse('025106025503'), 400)
if (-not $gate.NeedsStop(400)) { throw 'Safety response did not request stop' }
$gate = New-PreparedGate
$null = $gate.Approve($start, 300)
$gate.Observe([TreadmillDiagnostics.MotorTestGate]::Parse('025002780a002003'), 400)
if (-not $gate.NeedsStop(400)) { throw 'Changed bounds did not request stop' }
$rejected = $false
try { $null = [TreadmillDiagnostics.Reader]::ControlTest('invalid', 'public', 'fixture', $false) }
catch { $rejected = $_.Exception.Message.Contains('MOTOR trial approval/name required') }
if (-not $rejected) { throw 'C# motor approval gate failed' }
foreach ($phrase in @('', 'motor', 'NONMOTOR')) {
    $refused = & $helper -Command control-test -Device invalid -ExpectedName fixture -Bridge -ConfirmMotor -Confirmation $phrase | ConvertFrom-Json
    if ($LASTEXITCODE -ne 1 -or -not $refused.error.Contains('operator Confirmation MOTOR')) { throw 'PowerShell motor gate failed' }
}
foreach ($code in @(12, 15, 20, 30, 40, 50, 60)) {
    $gate = New-PreparedGate "speed-$code"
    $null = $gate.Approve($start, 300)
    $null = $gate.Approve($target, 400)
    Assert-Rejected $gate $target 450
    $step = '025302' + $code.ToString('x2') + '00' + (0x53 -bxor 2 -bxor $code).ToString('x2') + '03'
    $null = $gate.Approve($step, 500)
    $null = $gate.Approve($target, 600)
    Assert-Rejected $gate $step 700
    $null = $gate.Approve($stop, 800)
}
foreach ($code in 1..8) {
    $gate = New-PreparedGate "incline-$code"
    $null = $gate.Approve($start, 300)
    $null = $gate.Approve($target, 400)
    $step = '0253020a' + $code.ToString('x2') + (0x53 -bxor 2 -bxor 10 -bxor $code).ToString('x2') + '03'
    $null = $gate.Approve($step, 500)
    $null = $gate.Approve($target, 600)
    Assert-Rejected $gate $step 700
}
$sleepReady = New-Object TreadmillDiagnostics.MotorTestGate -ArgumentList 'ready-wake'
$sleepReady.Observe([TreadmillDiagnostics.MotorTestGate]::Parse('0250023c0a006403'), 0)
$sleepReady.Observe([TreadmillDiagnostics.MotorTestGate]::Parse('0250030800025903'), 0)
foreach ($time in @(0, 100, 200)) {
    $sleepReady.Observe([TreadmillDiagnostics.MotorTestGate]::Parse('025106025503'), $time)
}
Assert-Rejected $sleepReady $start 300
$null = $sleepReady.Approve('0253010000000080000000d203', 300)
Assert-Rejected $sleepReady $target 400
$sleepReady.Observe([TreadmillDiagnostics.MotorTestGate]::Parse('025106025503'), 500)
if ($sleepReady.NeedsStop(500)) { throw 'Bounded sleep-to-READY response window ended prematurely' }
$sleepReady.Observe([TreadmillDiagnostics.MotorTestGate]::Parse('025106025503'), 1700)
if (-not $sleepReady.NeedsStop(2300)) { throw 'Missing READY after two seconds must stop' }
$null = $sleepReady.Approve($stop, 2300)
foreach ($hex in @('025106015603', '0251065703')) {
    $gate = New-Object TreadmillDiagnostics.MotorTestGate -ArgumentList 'ready-wake'
    $gate.Observe([TreadmillDiagnostics.MotorTestGate]::Parse('0250023c0a006403'), 0)
    $gate.Observe([TreadmillDiagnostics.MotorTestGate]::Parse('0250030800025903'), 0)
    foreach ($time in @(0, 100, 200)) { $gate.Observe([TreadmillDiagnostics.MotorTestGate]::Parse($hex), $time) }
    Assert-Rejected $gate '0253010000000080000000d203' 300
}
$gate = New-PreparedGate 'ready-idle'
Assert-Rejected $gate $start 300
$null = $gate.Approve('0253010000000080000000d203', 300)
Assert-Rejected $gate $target 400
$gate.Observe([TreadmillDiagnostics.MotorTestGate]::Parse('0251095803'), 500)
if ($gate.NeedsStop(600)) { throw 'READY status must refresh the dedicated trial' }
$gate.Observe([TreadmillDiagnostics.MotorTestGate]::Parse('025102035003'), 700)
if (-not $gate.NeedsStop(700)) { throw 'READY trial must stop immediately on countdown' }
$null = $gate.Approve($stop, 700)
$gate = New-PreparedGate 'ready-idle'
$null = $gate.Approve('0253010000000080000000d203', 300)
$gate.Observe([TreadmillDiagnostics.MotorTestGate]::Parse('0251095803'), 400)
$gate.Observe([TreadmillDiagnostics.MotorTestGate]::Parse('0251005103'), 500)
if (-not $gate.NeedsStop(500)) { throw 'READY reversion must stop the dedicated trial' }
$null = $gate.Approve($stop, 500)
$gate = New-PreparedGate 'incline-measure-8'
$null = $gate.Approve($start, 300)
$null = $gate.Approve($target, 400)
Assert-Rejected $gate '0253020a045f03' 450
$null = $gate.Approve('0253020a085303', 500)
$null = $gate.Approve($target, 600)
Assert-Rejected $gate '0253020a085303' 700
$null = $gate.Approve($stop, 800)
$gate = New-PreparedGate 'incline-measure-4'
$null = $gate.Approve($start, 300)
$null = $gate.Approve($target, 400)
Assert-Rejected $gate '0253020a085303' 450
$null = $gate.Approve('0253020a045f03', 500)
$null = $gate.Approve($target, 600)
$null = $gate.Approve($stop, 800)
$gate = New-PreparedGate 'incline-timing-4'
$null = $gate.Approve($start, 300)
$null = $gate.Approve($target, 400)
Assert-Rejected $gate '0253020a085303' 450
$null = $gate.Approve('0253020a045f03', 500)
$null = $gate.Approve($target, 600)
Assert-Rejected $gate '0253020a045f03' 700
$null = $gate.Approve($stop, 800)
$gate = New-PreparedGate 'incline-timing-8'
$null = $gate.Approve($start, 300)
$null = $gate.Approve($target, 400)
Assert-Rejected $gate '0253020a015a03' 450
$null = $gate.Approve('0253020a085303', 500)
$null = $gate.Approve($target, 600)
Assert-Rejected $gate '0253020a085303' 700
$null = $gate.Approve($stop, 800)
$gate = New-PreparedGate 'pause'
$null = $gate.Approve($start, 300)
$null = $gate.Approve($target, 400)
$null = $gate.Approve('0253065503', 500)
Assert-Rejected $gate '0253065503' 600
$null = $gate.Approve($stop, 700)
Write-Output 'PASS: compilation, query/motor allowlists, approval, fresh preflight, no retries, stale/safety/changed-bounds stop and 12-second watchdog. No BLE I/O.'

foreach ($trial in @('ready-idle', 'ready-wake')) {
    $rejected = $false
    try { $null = [TreadmillDiagnostics.Reader]::ControlTest('invalid', 'public', 'fixture', $true, $trial) } catch { $rejected = $_.Exception.Message.Contains('READY trials retired') }
    if (-not $rejected) { throw 'Retired READY trial accepted by native reader' }
    $rejected = $false
    try { & $helper -Command control-test -Trial $trial -Device invalid } catch { $rejected = $_.Exception.Message.Contains('READY trials retired') }
    if (-not $rejected) { throw 'Retired READY trial accepted by PowerShell' }
}
Write-Output 'PASS: retired READY trials cannot open Bluetooth.'

$r = New-Object TreadmillDiagnostics.Report
$null = [TreadmillDiagnostics.Reader]::RecordIdleStatus($r, '0251005003', 0) # bad checksum
if ($r.idle_initial_state_code -ne -1) { throw 'Malformed status counted as idle evidence' }
if (-not [TreadmillDiagnostics.Reader]::RecordIdleStatus($r, '0251005103', 100)) { throw 'NORMAL rejected' }
if (-not [TreadmillDiagnostics.Reader]::RecordIdleStatus($r, '025106025503', 400)) { throw 'Documented sleep rejected' }
if ($r.idle_normal_reports -ne 1 -or -not $r.idle_sleep_observed -or $r.idle_sleep_after_normal_ms -ne 300) { throw 'Idle interval lost' }
$r = New-Object TreadmillDiagnostics.Report
$null = [TreadmillDiagnostics.Reader]::RecordIdleStatus($r, '025106025503', 0)
if ($r.idle_sleep_after_normal_ms -ne -1 -or $r.idle_initial_sleep_ms -ne 0) { throw 'Already-sleeping probe invented a standby interval or lost initial sleep time' }
foreach ($hex in @('025106015603', '0251065703', '0251030a005803', '0251025303')) {
    if ([TreadmillDiagnostics.Reader]::RecordIdleStatus($r, $hex, 500)) { throw 'Unsafe/nonidle state admitted to idle experiment' }
}
Write-Output 'PASS: idle probe rejects motion/key/unknown states and measures only validated NORMAL-to-sleep intervals.'

$hex = [BitConverter]::ToString([TreadmillDiagnostics.Reader]::FrameForQuery(0x5200)).Replace('-', '').ToLowerInvariant()
if ($hex -ne '0252005203') { throw 'Wrong documented current-exercise-data frame' }
$rejected = $false
try { $null = [TreadmillDiagnostics.Reader]::Probe('invalid', 'public', 'fixture', 10, $true, $true, $false, $true, $true) } catch { $rejected = $_.Exception.Message.Contains('only one experimental') }
if (-not $rejected) { throw 'Concurrent experimental query variants admitted' }
$rejected = $false
try { & $helper -Command control-test -Device invalid -StatusDataSport } catch { $rejected = $_.Exception.Message.Contains('only to nonmotor probe') }
if (-not $rejected) { throw 'Current-exercise-data query admitted to motor session' }
Write-Output 'PASS: documented current-exercise-data frame remains in isolated nonmotor observer.'

$rejected = $false
try { & $helper -Command control-test -Device invalid -MeasureSleepTimer } catch { $rejected = $_.Exception.Message.Contains('only to nonmotor probe') }
if (-not $rejected) { throw 'Sleep timer option admitted to motor session' }
$rejected = $false
try { $null = [TreadmillDiagnostics.Reader]::Probe('invalid', 'public', 'fixture', 10, $true, $true, $false, $false, $true, $true) } catch { $rejected = $_.Exception.Message.Contains('only one experimental') }
if (-not $rejected) { throw 'Sleep timer combined with other experimental mode' }
Write-Output 'PASS: sleep timer remains isolated from motor sessions and other query variants.'

$longTimer = & $helper -Command probe -Device invalid -ExpectedName fixture -ConfirmDiagnostics -Confirmation NONMOTOR -MeasureSleepTimer -Timeout 900 | ConvertFrom-Json
if ($LASTEXITCODE -ne 1 -or -not $longTimer.error.Contains('explicit AA:BB:CC:DD:EE:FF')) { throw '900-second sleep timer did not reach address validation' }
$rejected = $false
try { $null = [TreadmillDiagnostics.Reader]::Probe('invalid', 'public', 'fixture', 901, $true, $true, $false, $false, $false, $true) }
catch { $rejected = $_.Exception.Message.Contains('bounded scope') }
if (-not $rejected) { throw 'Sleep timer accepted over 900 seconds' }
Write-Output 'PASS: extended sleep timer is capped at 900 seconds.'
