<#
.SYNOPSIS
  Zero-touch one-line bootstrap installer for ag-mesh on Windows (binary + SHA-256 verify + skill + MCP + service + optional cluster join).
.EXAMPLE
  irm https://raw.githubusercontent.com/synchron1q/Antigravity-Telepathy_Skill/main/scripts/bootstrap.ps1 | iex
.EXAMPLE
  .\scripts\bootstrap.ps1 -Join "agmesh://join/v1/..."
.EXAMPLE
  .\scripts\bootstrap.ps1 -LocalBinary .\build\windows-msvc-release\Release\ag-mesh.exe -Join "10.8.0.1" -Pin "482-915"
#>
param(
    [string]$Repo = "synchron1q/Antigravity-Telepathy_Skill",
    [string]$Version = "latest",
    [string]$InstallDir = "$env:LOCALAPPDATA\ag-mesh\bin",
    [string]$LocalBinary = "",
    [string]$Join = "",
    [string]$Pin = "",
    [string]$NodeId = ""
)

$ErrorActionPreference = "Stop"

function Get-HostArch {
    $arch = [System.Runtime.InteropServices.RuntimeInformation]::OSArchitecture.ToString().ToLowerInvariant()
    switch ($arch) {
        "x64"   { return "x86_64" }
        "arm64" { return "arm64" }
        default { throw "Unsupported Windows architecture: $arch" }
    }
}

$Arch = Get-HostArch
$Target = "windows-$Arch"
New-Item -ItemType Directory -Force -Path $InstallDir | Out-Null
$ExePath = Join-Path $InstallDir "ag-mesh.exe"
$SkillSrcDir = $null

$TempRoot = Join-Path ([System.IO.Path]::GetTempPath()) ("agmesh-bootstrap-" + [System.Guid]::NewGuid().ToString("N"))
New-Item -ItemType Directory -Force -Path $TempRoot | Out-Null

try {
    if ($LocalBinary -ne "") {
        if (-not (Test-Path $LocalBinary)) {
            throw "Local binary not found: $LocalBinary"
        }
        Write-Host "[1/4] Installing offline binary from $LocalBinary -> $ExePath"
        Copy-Item -Force $LocalBinary $ExePath
        $RepoSkill = Join-Path (Split-Path -Parent $PSScriptRoot) "skill\antigravity-mesh"
        if (Test-Path $RepoSkill) {
            $SkillSrcDir = $RepoSkill
        }
    } else {
        Write-Host "[1/4] Querying GitHub Releases ($Repo, version=$Version)..."
        if ($Version -eq "latest") {
            $Release = Invoke-RestMethod -Uri "https://api.github.com/repos/$Repo/releases/latest" -Headers @{ "User-Agent" = "ag-mesh-bootstrap" }
            $Tag = $Release.tag_name
        } else {
            $Tag = $Version
        }
        if (-not $Tag) { throw "Unable to resolve release tag for $Repo" }

        $AssetName = "ag-mesh-$Tag-$Target.zip"
        $BaseUrl = "https://github.com/$Repo/releases/download/$Tag"
        $ZipPath = Join-Path $TempRoot $AssetName
        $ShaPath = Join-Path $TempRoot "$AssetName.sha256"

        Write-Host "Downloading $AssetName..."
        Invoke-WebRequest -Uri "$BaseUrl/$AssetName" -OutFile $ZipPath
        Invoke-WebRequest -Uri "$BaseUrl/$AssetName.sha256" -OutFile $ShaPath

        $ExpectedHash = ((Get-Content -Raw $ShaPath).Trim().Split(" ")[0]).ToLowerInvariant()
        $ActualHash = (Get-FileHash -Path $ZipPath -Algorithm SHA256).Hash.ToLowerInvariant()
        if ($ExpectedHash -ne $ActualHash) {
            throw "SHA-256 mismatch for $AssetName! Expected=$ExpectedHash Actual=$ActualHash"
        }
        Write-Host "SHA-256 verified: $ActualHash"

        $ExtractDir = Join-Path $TempRoot "extracted"
        Expand-Archive -Path $ZipPath -DestinationPath $ExtractDir -Force
        $PkgDir = Join-Path $ExtractDir "ag-mesh-$Tag-$Target"
        if (-not (Test-Path $PkgDir)) {
            $PkgDir = (Get-ChildItem -Path $ExtractDir -Directory | Select-Object -First 1).FullName
        }
        Copy-Item -Force (Join-Path $PkgDir "ag-mesh.exe") $ExePath
        $CandSkill = Join-Path $PkgDir "skill\antigravity-mesh"
        if (Test-Path $CandSkill) { $SkillSrcDir = $CandSkill }
    }

    # Ensure InstallDir is on user PATH
    $UserPath = [Environment]::GetEnvironmentVariable("Path", "User")
    if ($UserPath -notlike "*$InstallDir*") {
        [Environment]::SetEnvironmentVariable("Path", "$UserPath;$InstallDir", "User")
        $env:Path = "$env:Path;$InstallDir"
    }

    Write-Host "[2/4] Installing Antigravity skill..."
    $AgDir = if ($env:ANTIGRAVITY_HOME) { $env:ANTIGRAVITY_HOME } else { Join-Path $env:USERPROFILE ".gemini\antigravity" }
    $SkillDst = Join-Path $AgDir "skills\antigravity-mesh"
    New-Item -ItemType Directory -Force -Path $SkillDst | Out-Null
    if ($SkillSrcDir -and (Test-Path $SkillSrcDir)) {
        Copy-Item -Recurse -Force "$SkillSrcDir\*" $SkillDst
    }

    Write-Host "[3/4] Running ag-mesh init --auto..."
    $InitArgs = @("init", "--auto")
    if ($NodeId -ne "") { $InitArgs += @("--node-id", $NodeId) }
    & $ExePath @InitArgs

    if ($Join -ne "") {
        Write-Host "[4/4] Joining cluster..."
        $JoinArgs = @("join", $Join)
        if ($Pin -ne "") { $JoinArgs += @("--pin", $Pin) }
        & $ExePath @JoinArgs
    } else {
        Write-Host "[4/4] Done! Run 'ag-mesh pair' to invite other hosts or 'ag-mesh join <token>' to join a cluster."
    }
} finally {
    Remove-Item -Recurse -Force -ErrorAction SilentlyContinue $TempRoot
}
