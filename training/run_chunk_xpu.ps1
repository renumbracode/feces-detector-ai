param(
    [int]$ChunkEpochs = 1,
    [int]$Threads = 4,
    [int]$Workers = 4,
    [double]$ThermalLimitC = 85,
    [double]$MaxStartTempC = 65,
    [int]$PollSeconds = 20
)

$ErrorActionPreference = "Stop"
$trainingDir = $PSScriptRoot
$projectRoot = Split-Path -Parent $trainingDir
$runDir = Join-Path $trainingDir "runs\detect"
$pythonExe = Join-Path $env:TEMP "opencode\feces-xpu-venv\Scripts\python.exe"
$timestamp = Get-Date -Format "yyyyMMdd_HHmmss"
$stdoutLog = Join-Path $runDir "xpu_${timestamp}.out.log"
$stderrLog = Join-Path $runDir "xpu_${timestamp}.err.log"
$thermalLog = Join-Path $runDir "xpu_${timestamp}.thermal.csv"

if (-not (Test-Path -LiteralPath $pythonExe)) {
    throw "XPU Python not found: $pythonExe"
}

& $pythonExe -c "import torch; raise SystemExit(0 if torch.xpu.is_available() else 1)"
if ($LASTEXITCODE -ne 0) {
    throw "torch.xpu.is_available() is false. Check the Intel GPU driver."
}

function Get-ThermalCelsius {
    $samples = Get-Counter '\Thermal Zone Information(*)\Temperature' -ErrorAction SilentlyContinue
    if (-not $samples) {
        return $null
    }
    return [math]::Round($samples.CounterSamples[0].CookedValue - 273.15, 1)
}

$power = Get-CimInstance -Namespace root\wmi -ClassName BatteryStatus -ErrorAction SilentlyContinue | Select-Object -First 1
$battery = Get-CimInstance Win32_Battery -ErrorAction SilentlyContinue | Select-Object -First 1
if ($null -ne $power -and -not $power.PowerOnline) {
    throw "AC power is not connected. Battery=$($battery.EstimatedChargeRemaining)%."
}

$startTemp = Get-ThermalCelsius
if ($null -eq $startTemp) {
    throw "Could not read the thermal sensor."
}
if ($startTemp -gt $MaxStartTempC) {
    throw "Laptop is too warm to start: ${startTemp}C (limit ${MaxStartTempC}C)."
}

"Timestamp,ThermalCelsius" | Set-Content -LiteralPath $thermalLog
Add-Content -LiteralPath $thermalLog -Value "$(Get-Date -Format o),$startTemp"

$arguments = @(
    (Join-Path $trainingDir "train.py"),
    "--resume",
    "--chunk-epochs", $ChunkEpochs,
    "--threads", $Threads,
    "--workers", $Workers,
    "--device", "xpu"
)
$process = Start-Process -FilePath $pythonExe -ArgumentList $arguments -WorkingDirectory $projectRoot -RedirectStandardOutput $stdoutLog -RedirectStandardError $stderrLog -PassThru -WindowStyle Hidden

Write-Output "Started XPU chunk PID=$($process.Id)"
Write-Output "Python: $pythonExe"
Write-Output "Device: xpu"
Write-Output "Chunk epochs: $ChunkEpochs"
Write-Output "Workers: $Workers"
Write-Output "Start temp: ${startTemp}C"
Write-Output "Stdout: $stdoutLog"
Write-Output "Stderr: $stderrLog"
Write-Output "Thermal log: $thermalLog"

while (-not $process.HasExited) {
    Start-Sleep -Seconds $PollSeconds
    $temp = Get-ThermalCelsius
    if ($null -ne $temp) {
        Add-Content -LiteralPath $thermalLog -Value "$(Get-Date -Format o),$temp"
        Write-Output "Temp: ${temp}C"
        if ($temp -ge $ThermalLimitC) {
            Write-Output "Thermal limit reached. Stopping training PID=$($process.Id)."
            Stop-Process -Id $process.Id -Force -ErrorAction SilentlyContinue
            break
        }
    }
}

$process.WaitForExit()
Write-Output "Exit code: $($process.ExitCode)"
Write-Output "Done."
