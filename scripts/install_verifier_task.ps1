# Install (or remove) a Windows scheduled task that starts the YOLOv8
# verifier when the user logs on.
#
# The verifier is a server that has to be running before the camera board
# can do its YOLOv8 leg. Making it start by itself means one less thing to
# remember after a reboot -- which matters during hardware bring-up, when
# reboots are frequent.
#
# The task runs scripts\start_verifier.cmd, which:
#   - refuses to start a second copy if one is already running,
#   - launches the server with NO console (see start_verifier.cmd), so the
#     task's own window closing cannot kill it,
#   - waits for /health to report model_loaded:true before giving up.
#
# Usage:
#   powershell -ExecutionPolicy Bypass -File scripts\install_verifier_task.ps1
#   powershell -ExecutionPolicy Bypass -File scripts\install_verifier_task.ps1 -Remove
#   powershell -ExecutionPolicy Bypass -File scripts\install_verifier_task.ps1 -Status

[CmdletBinding()]
param(
    [switch]$Remove,
    [switch]$Status
)

$ErrorActionPreference = 'Stop'

$TaskName = 'FecesDetectorVerifier'
$Repo = Split-Path -Parent $PSScriptRoot
$Launcher = Join-Path $Repo 'scripts\start_verifier.cmd'

if ($Status) {
    $t = Get-ScheduledTask -TaskName $TaskName -ErrorAction SilentlyContinue
    if (-not $t) {
        Write-Output "[task] '$TaskName' is not installed."
        exit 0
    }
    $t | Select-Object TaskName, State | Format-Table -AutoSize
    Get-ScheduledTaskInfo -TaskName $TaskName |
        Select-Object LastRunTime, LastTaskResult, NextRunTime |
        Format-List
    exit 0
}

if ($Remove) {
    $t = Get-ScheduledTask -TaskName $TaskName -ErrorAction SilentlyContinue
    if (-not $t) {
        Write-Output "[task] '$TaskName' is not installed; nothing to remove."
        exit 0
    }
    Unregister-ScheduledTask -TaskName $TaskName -Confirm:$false
    Write-Output "[task] '$TaskName' removed."
    exit 0
}

if (-not (Test-Path $Launcher)) {
    Write-Error "[task] launcher not found: $Launcher"
    exit 1
}

# The launcher is a .cmd, so cmd.exe runs it. The quotes around the path are
# what let it survive the space in "C:\Users\Brent Jim Tingzon".
$action = New-ScheduledTaskAction `
    -Execute 'cmd.exe' `
    -Argument ('/c "' + $Launcher + '"') `
    -WorkingDirectory $Repo

$trigger = New-ScheduledTaskTrigger -AtLogOn

$settings = New-ScheduledTaskSettingsSet `
    -AllowStartIfOnBatteries `
    -DontStopIfGoingOnBatteries `
    -StartWhenAvailable `
    -ExecutionTimeLimit ([TimeSpan]::Zero) `
    -MultipleInstances IgnoreNew

$principal = New-ScheduledTaskPrincipal `
    -UserId ([System.Security.Principal.WindowsIdentity]::GetCurrent().Name) `
    -LogonType Interactive `
    -RunLevel Limited

Register-ScheduledTask `
    -TaskName $TaskName `
    -Action $action `
    -Trigger $trigger `
    -Settings $settings `
    -Principal $principal `
    -Description 'Starts the YOLOv8 verifier (server/yolo_verify) at logon.' `
    -Force | Out-Null

Write-Output "[task] '$TaskName' installed -> $Launcher"
Write-Output '[task] it starts at the next logon; run the launcher by hand to start it now.'
