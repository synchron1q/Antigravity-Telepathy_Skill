param(
    [string]$BinaryPath = "C:\Program Files\ag-mesh\ag-mesh.exe",
    [string]$ConfigPath = "$env:APPDATA\ag-mesh\config.toml"
)

$ErrorActionPreference = "Stop"
& $BinaryPath service install --config $ConfigPath
