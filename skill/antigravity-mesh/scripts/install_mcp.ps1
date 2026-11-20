param(
    [string]$BinaryPath = "ag-mesh.exe",
    [string]$ConfigPath = ""
)

$ErrorActionPreference = "Stop"
$AgDir = if ($env:ANTIGRAVITY_HOME) { $env:ANTIGRAVITY_HOME } else { Join-Path $env:USERPROFILE ".gemini\antigravity" }
New-Item -ItemType Directory -Force -Path $AgDir | Out-Null

$SkillSrc = Split-Path -Parent $PSScriptRoot
$SkillDst = Join-Path $AgDir "skills\antigravity-mesh"
New-Item -ItemType Directory -Force -Path $SkillDst | Out-Null
Copy-Item -Recurse -Force "$SkillSrc\*" $SkillDst

$McpPath = Join-Path $AgDir "mcp_config.json"
$McpObj = @{ mcpServers = @{} }
if (Test-Path $McpPath) {
    try {
        $Parsed = Get-Content -Raw -Path $McpPath | ConvertFrom-Json -AsHashtable
        if ($Parsed -is [hashtable]) {
            $McpObj = $Parsed
            if (-not $McpObj.ContainsKey("mcpServers")) {
                $McpObj["mcpServers"] = @{}
            }
        }
    } catch {}
}

$ArgsList = @("mcp")
if ($ConfigPath -ne "") {
    $ArgsList += @("--config", $ConfigPath)
}

$McpObj["mcpServers"]["ag-mesh"] = @{
    command = $BinaryPath
    args = $ArgsList
}

$McpObj | ConvertTo-Json -Depth 10 | Set-Content -Encoding UTF8 -Path $McpPath
Write-Host "Installed antigravity-mesh skill -> $SkillDst"
Write-Host "Registered ag-mesh MCP server -> $McpPath"
