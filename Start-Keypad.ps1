[CmdletBinding()]
param()

$ErrorActionPreference = 'Stop'
$taskName = 'KeypadControl'
$workspace = Split-Path -Parent $MyInvocation.MyCommand.Path
$controller = Join-Path $workspace 'KeypadControl.exe'
if (-not (Test-Path -LiteralPath $controller -PathType Leaf)) {
    $controller = Join-Path $workspace 'build\Release\KeypadControl.exe'
}
$installScript = Join-Path $workspace 'Install-Keypad.ps1'

if (-not (Test-Path -LiteralPath $controller -PathType Leaf)) {
    throw 'controller_missing'
}

$task = Get-ScheduledTask -TaskName $taskName -ErrorAction SilentlyContinue
$registeredExecutable = if ($task) { $task.Actions[0].Execute } else { $null }
$identity = [System.Security.Principal.WindowsIdentity]::GetCurrent()
$startsAtUserLogon = $task -and @($task.Triggers | Where-Object {
    $_.CimClass.CimClassName -eq 'MSFT_TaskLogonTrigger' -and $_.Enabled -and
    $_.UserId -in @($identity.Name, $identity.User.Value)
}).Count -gt 0
$resilientTask = $task -and $task.Settings.StartWhenAvailable -and
    $task.Settings.RestartCount -ge 3 -and $task.Settings.Priority -eq 4
if (-not $task -or $registeredExecutable -ne $controller -or
    -not $startsAtUserLogon -or -not $resilientTask) {
    & $installScript
    return
}

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
