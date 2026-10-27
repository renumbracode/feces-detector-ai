param(
    [int]$ChunkEpochs = 2,
    [int]$Threads = 8,
    [int]$Workers = 2,
    [double]$ThermalLimitC = 85,
    [double]$MaxStartTempC = 60,
    [int]$PollSeconds = 30,
    [switch]$AllowBattery,
    [int]$MinBatteryPercent = 25
)

$ErrorActionPreference = "Stop"
$trainingDir = $PSScriptRoot
$projectRoot = Split-Path -Parent $trainingDir
$runDir = Join-Path $trainingDir "runs\detect"
$timestamp = Get-Date -Format "yyyyMMdd_HHmmss"
$stdoutLog = Join-Path $runDir "chunk_${timestamp}.out.log"
$stderrLog = Join-Path $runDir "chunk_${timestamp}.err.log"
$thermalLog = Join-Path $runDir "chunk_${timestamp}.thermal.csv"
$batteryLog = Join-Path $runDir "chunk_${timestamp}.battery.csv"

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
    if (-not $AllowBattery) {
        throw "AC power is not connected. Battery=$($battery.EstimatedChargeRemaining)%."
    }
    if ($null -ne $battery -and $battery.EstimatedChargeRemaining -le $MinBatteryPercent) {
        throw "Battery is too low to start: $($battery.EstimatedChargeRemaining)% (floor ${MinBatteryPercent}%)."
    }
    Write-Output "AC disconnected; running on battery with floor ${MinBatteryPercent}%."
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
"Timestamp,Percent,DischargeRate_mW,PowerOnline" | Set-Content -LiteralPath $batteryLog
Add-Content -LiteralPath $batteryLog -Value "$(Get-Date -Format o),$($battery.EstimatedChargeRemaining),$($power.DischargeRate),$($power.PowerOnline)"

$arguments = @(
    (Join-Path $trainingDir "train.py"),
    "--resume",
    "--chunk-epochs", $ChunkEpochs,
    "--threads", $Threads,
    "--workers", $Workers,
    "--device", "cpu"
)
$process = Start-Process -FilePath "python" -ArgumentList $arguments -WorkingDirectory $projectRoot -RedirectStandardOutput $stdoutLog -RedirectStandardError $stderrLog -PassThru -WindowStyle Hidden

Write-Output "Started chunk PID=$($process.Id)"
Write-Output "Chunk epochs: $ChunkEpochs"
Write-Output "Threads: $Threads"
Write-Output "Workers: $Workers"
Write-Output "Start temp: ${startTemp}C"
Write-Output "Stdout: $stdoutLog"
Write-Output "Stderr: $stderrLog"
Write-Output "Thermal log: $thermalLog"
Write-Output "Battery log: $batteryLog"
Write-Output "Battery floor: ${MinBatteryPercent}%"

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

    $bs = Get-CimInstance -Namespace root\wmi -ClassName BatteryStatus -ErrorAction SilentlyContinue | Select-Object -First 1
    $battNow = Get-CimInstance Win32_Battery -ErrorAction SilentlyContinue | Select-Object -First 1
    if ($null -ne $battNow) {
        $pct = $battNow.EstimatedChargeRemaining
        $online = $null
        $rate = $null
        if ($null -ne $bs) {
            $online = $bs.PowerOnline
            $rate = $bs.DischargeRate
        }
        Add-Content -LiteralPath $batteryLog -Value "$(Get-Date -Format o),$pct,$rate,$online"
        Write-Output "Battery: ${pct}% (online=$online, rate=${rate}mW)"
        if ($null -ne $online -and -not $online -and $pct -le $MinBatteryPercent) {
            Write-Output "Battery floor reached. Stopping training PID=$($process.Id)."
            Stop-Process -Id $process.Id -Force -ErrorAction SilentlyContinue
            break
        }
    }
}

$process.WaitForExit()
Write-Output "Exit code: $($process.ExitCode)"
Write-Output "Done."
