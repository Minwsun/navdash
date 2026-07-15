$root = Split-Path -Parent $PSScriptRoot
$logPath = Join-Path $root 'captures\com4.log'

New-Item -ItemType Directory -Path (Split-Path -Parent $logPath) -Force | Out-Null
Set-Location $root
python -u (Join-Path $PSScriptRoot 'capture_com.py') COM4 $logPath
