[CmdletBinding()]
param()

$ErrorActionPreference = 'Stop'
$taskName = 'KeypadControl'
$root = Split-Path -Parent $MyInvocation.MyCommand.Path
$local = Join-Path $root 'KeypadControl.exe'
$task = Get-ScheduledTask -TaskName $taskName -ErrorAction SilentlyContinue
$targets = @($local)
if ($task -and $task.Actions[0].Execute) {
    $targets += $task.Actions[0].Execute
}
foreach ($target in $targets | Select-Object -Unique) {
    if (Test-Path -LiteralPath $target -PathType Leaf) {
        & $target --stop
    }
}
if ($task) {
    Stop-ScheduledTask -TaskName $taskName -ErrorAction SilentlyContinue
    Unregister-ScheduledTask -TaskName $taskName -Confirm:$false
}
