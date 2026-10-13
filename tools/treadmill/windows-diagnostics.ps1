# Windows PowerShell 5.1 / native WinRT. No pairing, writes or subscriptions.
param(
    [ValidateSet('scan', 'info')][string]$Command = 'scan',
    [ValidateRange(1, 60)][int]$Timeout = 15,
    [string]$Device = '',
    [ValidateSet('unknown', 'public', 'random')][string]$AddressType = 'unknown'
)
$ErrorActionPreference = 'Stop'
[Console]::OutputEncoding = New-Object System.Text.UTF8Encoding($false)
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
    } else {
        $report = [TreadmillDiagnostics.Reader]::Info($Device, $AddressType, $Timeout)
    }
    $report | ConvertTo-Json -Depth 12
    if ($report.status -ne 'complete') { exit 1 }
} catch {
    @{ command = $Command; backend = 'windows-winrt'; status = 'error'; error = $_.Exception.Message } |
        ConvertTo-Json -Depth 3
    exit 1
}
