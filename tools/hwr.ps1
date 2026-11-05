param(
    [ValidateSet('inspect','extract-exefs','analyze','disassemble','translate','validate','test','help')]
    [string]$Command = 'help',
    [Parameter(ValueFromRemainingArguments = $true)]
    [string[]]$ToolArgs
)

$ErrorActionPreference = 'Stop'
$projectRoot = Split-Path -Parent $PSScriptRoot
$pythonCommand = (Get-Command python.exe -ErrorAction Stop).Source
$oldPythonPath = $env:PYTHONPATH
$oldPythonUtf8 = $env:PYTHONUTF8
$toolExitCode = 1
Push-Location -LiteralPath $projectRoot
try {
    $sourceDirectory = Join-Path $projectRoot 'tools'
    $env:PYTHONPATH = if ($oldPythonPath) { $sourceDirectory + [IO.Path]::PathSeparator + $oldPythonPath } else { $sourceDirectory }
    $env:PYTHONUTF8 = '1'
    switch ($Command) {
        'test' { & $pythonCommand -m unittest discover -s tests/prototype -v @ToolArgs }
        'validate' { & $pythonCommand -m hwrecomp.validate @ToolArgs }
        'help' { & $pythonCommand -m hwrecomp --help }
        default { & $pythonCommand -m hwrecomp $Command @ToolArgs }
    }
    $toolExitCode = $LASTEXITCODE
} finally {
    $env:PYTHONPATH = $oldPythonPath
    $env:PYTHONUTF8 = $oldPythonUtf8
    Pop-Location
}
exit $toolExitCode
