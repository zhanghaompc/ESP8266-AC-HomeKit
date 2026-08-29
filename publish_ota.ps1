param(
    [string]$Version,

    [string]$Repo = 'zhanghaompc/ESP8266-AC-HomeKit',
    [string]$Branch = 'master',
    [string]$PanelPath = 'D:\ESP-Project\15.ESP32_AC_USB _ble\15.ESP32_AC_USB _ble\MQTT_CONTROL\index.html',
    [switch]$UpdatePanel
)

$ErrorActionPreference = 'Stop'

if (-not $Version) {
    $Version = Read-Host 'Enter version to publish, for example 1.0.11'
}

if ($Version -notmatch '^\d+\.\d+\.\d+$') {
    throw "[Publish failed] Bad version format: $Version. Use a format like 1.0.11."
}

function Step([string]$Message) {
    Write-Host ""
    Write-Host "==> $Message" -ForegroundColor Cyan
}

function Fail([string]$Message) {
    throw "[Publish failed] $Message"
}

function Require-Command([string]$Name) {
    if (-not (Get-Command $Name -ErrorAction SilentlyContinue)) {
        Fail "Command not found: $Name"
    }
}

function Get-PlatformIo {
    $pio = Get-Command platformio -ErrorAction SilentlyContinue
    if ($pio) { return $pio.Source }

    $fallback = Join-Path $env:USERPROFILE '.platformio\penv\Scripts\platformio.exe'
    if (Test-Path $fallback) { return $fallback }

    Fail "PlatformIO was not found. Please check platformio.exe installation."
}

function Set-FirmwareVersion([string]$Value) {
    $path = Join-Path $PSScriptRoot 'src\DeviceConfig.h'
    # Latin-1 gives a byte-for-byte round trip, including legacy malformed comment bytes.
    $encoding = [Text.Encoding]::GetEncoding(28591)
    $text = $encoding.GetString([IO.File]::ReadAllBytes($path))
    if ($text -notmatch '#define FW_VERSION ".*"') {
        Fail "FW_VERSION was not found in src\DeviceConfig.h"
    }
    $next = $text -replace '#define FW_VERSION ".*"', ('#define FW_VERSION "' + $Value + '"')
    if ($next -ne $text) {
        [IO.File]::WriteAllBytes($path, $encoding.GetBytes($next))
    }
}

function Set-OtaManifest([string]$Value, [string]$Url) {
    $path = Join-Path $PSScriptRoot 'firmware\esp8266\ota.json'
    $json = @{ version = $Value; url = $Url } | ConvertTo-Json -Compress
    Set-Content -LiteralPath $path -Value $json -NoNewline
}

function Invoke-GhApiJson([string]$Path, [string]$Method, [object]$Body) {
    $json = $Body | ConvertTo-Json -Depth 20 -Compress
    return ($json | gh api $Path -X $Method --input - | ConvertFrom-Json)
}

function Invoke-Git([string[]]$Arguments) {
    $oldPreference = $ErrorActionPreference
    $ErrorActionPreference = 'Continue'
    try {
        $output = & git @Arguments 2>&1
        $exitCode = $LASTEXITCODE
    }
    finally {
        $ErrorActionPreference = $oldPreference
    }
    $output | ForEach-Object { Write-Host $_ }
    return $exitCode
}

function Publish-HeadViaGitHubApi([string]$Message) {
    $remoteHead = (gh api "repos/$Repo/git/ref/heads/$Branch" --jq .object.sha).Trim()
    $baseTree = (gh api "repos/$Repo/git/commits/$remoteHead" --jq .tree.sha).Trim()
    $remoteTree = gh api "repos/$Repo/git/trees/${baseTree}?recursive=1" | ConvertFrom-Json
    $remoteBlobs = @{}
    foreach ($entry in $remoteTree.tree) {
        if ($entry.type -eq 'blob') {
            $remoteBlobs[$entry.path] = $entry.sha
        }
    }

    $files = @()
    foreach ($line in (git -c core.quotepath=false ls-tree -r HEAD)) {
        if ($line -match '^\d+ blob ([0-9a-f]+)\t(.+)$') {
            $sha = $Matches[1]
            $path = $Matches[2]
            if (-not $remoteBlobs.ContainsKey($path) -or $remoteBlobs[$path] -ne $sha) {
                $files += $path
            }
        }
    }
    if (-not $files) {
        Fail "Local HEAD has no file differences from remote $Branch"
    }

    $tree = @()
    foreach ($file in $files) {
        $full = Join-Path $PSScriptRoot $file
        $content = [Convert]::ToBase64String([IO.File]::ReadAllBytes($full))
        $blob = Invoke-GhApiJson "repos/$Repo/git/blobs" 'POST' @{
            content = $content
            encoding = 'base64'
        }
        $tree += @{
            path = ($file -replace '\\', '/')
            mode = '100644'
            type = 'blob'
            sha = $blob.sha
        }
    }

    $newTree = Invoke-GhApiJson "repos/$Repo/git/trees" 'POST' @{
        base_tree = $baseTree
        tree = $tree
    }

    $newCommit = Invoke-GhApiJson "repos/$Repo/git/commits" 'POST' @{
        message = $Message
        tree = $newTree.sha
        parents = @($remoteHead)
        author = @{
            name = (git show -s --format=%an HEAD).Trim()
            email = (git show -s --format=%ae HEAD).Trim()
            date = (git show -s --format=%aI HEAD).Trim()
        }
        committer = @{
            name = (git show -s --format=%cn HEAD).Trim()
            email = (git show -s --format=%ce HEAD).Trim()
            date = (git show -s --format=%cI HEAD).Trim()
        }
    }

    Invoke-GhApiJson "repos/$Repo/git/refs/heads/$Branch" 'PATCH' @{
        sha = $newCommit.sha
        force = $false
    } | Out-Null

    return $newCommit.sha
}

function Push-Or-PublishViaApi([string]$Message) {
    $pushExit = Invoke-Git -Arguments @('push', 'origin', $Branch)
    if ($pushExit -eq 0) {
        return (git rev-parse HEAD).Trim()
    }

    Write-Warning "git push failed. Publishing with GitHub API instead."
    return Publish-HeadViaGitHubApi $Message
}

function Resolve-CommitSha([object[]]$Output, [string]$Context) {
    $sha = $Output |
        ForEach-Object { $_.ToString().Trim() } |
        Where-Object { $_ -match '^[0-9a-f]{40}$' } |
        Select-Object -Last 1
    if (-not $sha) {
        Fail "No valid commit SHA returned for $Context"
    }
    return $sha
}

function Commit-And-Publish([string[]]$Files, [string]$Message) {
    $addExit = Invoke-Git -Arguments (@('add', '--') + $Files)
    if ($addExit -ne 0) {
        Fail "git add failed: $Message"
    }

    & git diff --cached --quiet 2>&1 | Out-Null
    if ($LASTEXITCODE -eq 0) {
        Write-Host "No new file changes; retry publishing current HEAD."
    }
    else {
        $commitExit = Invoke-Git -Arguments @('commit', '-m', $Message)
        if ($commitExit -ne 0) {
            Fail "git commit failed: $Message"
        }
    }
    return Push-Or-PublishViaApi $Message
}

function Update-PanelManifestUrl([string]$ManifestCommit) {
    if (-not $UpdatePanel) {
        Write-Host "Skip panel update. Use -UpdatePanel if you want to update the web panel manifest URL."
        return
    }
    if (-not (Test-Path $PanelPath)) {
        Write-Warning "Panel file was not found: $PanelPath"
        return
    }

    $url = "https://fastly.jsdelivr.net/gh/$Repo@$ManifestCommit/firmware/esp8266/ota.json"
    $escapedRepo = [regex]::Escape($Repo)
    $pattern = "https://fastly\.jsdelivr\.net/gh/$escapedRepo@[^`"']+/firmware/esp8266/ota\.json"
    $text = Get-Content -LiteralPath $PanelPath -Raw
    $next = [regex]::Replace($text, $pattern, $url)
    if ($next -eq $text) {
        Write-Warning "Old ESP8266 OTA manifest URL was not found in the panel. Please check manually."
        return
    }
    Set-Content -LiteralPath $PanelPath -Value $next -NoNewline
    Write-Host "Panel manifest commit updated: $ManifestCommit"
}

Set-Location $PSScriptRoot

Step "Check tools"
Require-Command git
Require-Command gh
$platformio = Get-PlatformIo
gh auth status | Out-Host

Step "Check working tree"
$dirty = git status --porcelain -- firmware/esp8266/esp8266_wifi.bin firmware/esp8266/ota.json src/DeviceConfig.h src/OtaManager.cpp src/main.cpp src/MqttManager.cpp src/MqttManager.h src/OtaManager.h src/CommandHandler.cpp
if ($dirty) {
    Fail "OTA related files have uncommitted changes. Please handle them first:`n$dirty"
}

Step "Set firmware version $Version"
Set-FirmwareVersion $Version

Step "Build firmware"
& $platformio run -e esp8266_homekit
if ($LASTEXITCODE -ne 0) {
    Fail "PlatformIO build failed"
}

Step "Copy firmware to release path"
$buildBin = Join-Path $PSScriptRoot '.pio\build\esp8266_homekit\firmware.bin'
$releaseBin = Join-Path $PSScriptRoot 'firmware\esp8266\esp8266_wifi.bin'
Copy-Item -LiteralPath $buildBin -Destination $releaseBin -Force

Step "Commit and publish firmware $Version"
$placeholderUrl = "https://fastly.jsdelivr.net/gh/$Repo@$Branch/firmware/esp8266/esp8266_wifi.bin"
Set-OtaManifest $Version $placeholderUrl
$releaseOutput = @(Commit-And-Publish @(
    'src/DeviceConfig.h',
    'firmware/esp8266/esp8266_wifi.bin',
    'firmware/esp8266/ota.json'
) "Release ESP8266 firmware v$Version")
$releaseCommit = Resolve-CommitSha $releaseOutput 'firmware release'

Step "Pin firmware URL"
$fixedBinUrl = "https://fastly.jsdelivr.net/gh/$Repo@$releaseCommit/firmware/esp8266/esp8266_wifi.bin"
Set-OtaManifest $Version $fixedBinUrl
$manifestOutput = @(Commit-And-Publish @('firmware/esp8266/ota.json') "Pin ESP8266 v$Version OTA binary URL")
$manifestCommit = Resolve-CommitSha $manifestOutput 'manifest pin'

Step "Update web panel"
Update-PanelManifestUrl $manifestCommit

Step "Verify remote manifest"
$manifestUrl = "https://fastly.jsdelivr.net/gh/$Repo@$manifestCommit/firmware/esp8266/ota.json"
$remoteManifest = Invoke-RestMethod -Uri "${manifestUrl}?ts=$(Get-Date -Format yyyyMMddHHmmss)"
$remoteManifest | ConvertTo-Json -Compress | Write-Host

Write-Host ""
Write-Host "Publish finished:" -ForegroundColor Green
Write-Host "Version: $Version"
Write-Host "Manifest: $manifestUrl"
Write-Host "Firmware: $fixedBinUrl"
Write-Host ""
Write-Host "Test: refresh the web panel, select ESP8266 AC, then click check update."
