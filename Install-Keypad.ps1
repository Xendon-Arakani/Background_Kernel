[CmdletBinding()]
param()

$ErrorActionPreference = 'Stop'
$taskName = 'KeypadControl'
$workspace = Split-Path -Parent $MyInvocation.MyCommand.Path
$controller = Join-Path $workspace 'KeypadControl.exe'
if (-not (Test-Path -LiteralPath $controller -PathType Leaf)) {
    $controller = Join-Path $workspace 'build\Release\KeypadControl.exe'
}

if (-not (Test-Path -LiteralPath $controller -PathType Leaf)) {
    throw 'controller_missing'
}

& $controller --stop
$priorControllers = Get-Process -Name KeypadControl -ErrorAction SilentlyContinue |
    Where-Object { $_.Path -eq $controller }
if ($priorControllers) {
    foreach ($priorController in $priorControllers) {
        $priorController.WaitForExit(45000)
    }
    $priorControllers = Get-Process -Name KeypadControl -ErrorAction SilentlyContinue |
        Where-Object { $_.Path -eq $controller }
    if ($priorControllers) {
        $priorControllers | Stop-Process -Force
        $priorControllers | Wait-Process -Timeout 10 -ErrorAction Stop
    }
}
Stop-ScheduledTask -TaskName $taskName -ErrorAction SilentlyContinue

$identity = [System.Security.Principal.WindowsIdentity]::GetCurrent()
$accountSid = $identity.User.Value
$action = New-ScheduledTaskAction -Execute $controller -WorkingDirectory $workspace
$trigger = New-ScheduledTaskTrigger -AtLogOn -User $accountSid
$principal = New-ScheduledTaskPrincipal `
    -UserId $accountSid `
    -LogonType Interactive `
    -RunLevel Limited
$settings = New-ScheduledTaskSettingsSet `
    -MultipleInstances IgnoreNew `
    -AllowStartIfOnBatteries `
    -DontStopIfGoingOnBatteries `
    -StartWhenAvailable `
    -RestartCount 3 `
    -RestartInterval (New-TimeSpan -Minutes 1) `
    -Priority 4 `
    -ExecutionTimeLimit ([TimeSpan]::Zero)
$definition = New-ScheduledTask `
    -Action $action `
    -Trigger $trigger `
    -Principal $principal `
    -Settings $settings

Register-ScheduledTask -TaskName $taskName -InputObject $definition -Force | Out-Null
Start-ScheduledTask -TaskName $taskName
$deadline = (Get-Date).AddSeconds(15)
$ready = $false
do {
    $runningController = Get-Process -Name KeypadControl -ErrorAction SilentlyContinue |
        Where-Object { $_.Path -eq $controller } |
        Select-Object -First 1
    try {
        $signal = [System.Threading.EventWaitHandle]::OpenExisting(
            'Local\BkKeyStop')
        $signal.Dispose()
        $ready = $null -ne $runningController
    }
    catch {
        Start-Sleep -Milliseconds 100
    }
} while (-not $ready -and (Get-Date) -lt $deadline)
if (-not $ready) {
    throw 'controller_ready=false'
}
