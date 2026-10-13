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
Write-Output 'PASS: compilation, address validation, 3 exact frames, 10 forbidden queries and approval gates. No BLE I/O.'
