param(
    [Parameter(Mandatory = $true)]
    [ValidatePattern('^\d+\.\d+\.\d+$')]
    [string]$Version,

    [string]$Repo = 'zhanghaompc/ESP8266-AC-HomeKit',
    [string]$Branch = 'master',
    [string]$PanelPath = 'D:\ESP-Project\15.ESP32_AC_USB _ble\15.ESP32_AC_USB _ble\MQTT_CONTROL\index.html',
    [switch]$SkipPanelUpdate
)

$ErrorActionPreference = 'Stop'

function Step([string]$Message) {
    Write-Host ""
    Write-Host "==> $Message" -ForegroundColor Cyan
}

function Fail([string]$Message) {
    throw "[发布失败] $Message"
}

function Require-Command([string]$Name) {
    if (-not (Get-Command $Name -ErrorAction SilentlyContinue)) {
        Fail "找不到命令：$Name"
    }
}

function Get-PlatformIo {
    $pio = Get-Command platformio -ErrorAction SilentlyContinue
    if ($pio) { return $pio.Source }

    $fallback = Join-Path $env:USERPROFILE '.platformio\penv\Scripts\platformio.exe'
    if (Test-Path $fallback) { return $fallback }

    Fail "找不到 PlatformIO。请确认 platformio.exe 已安装。"
}

function Set-FirmwareVersion([string]$Value) {
    $path = Join-Path $PSScriptRoot 'src\DeviceConfig.h'
    $text = Get-Content -LiteralPath $path -Raw
    $next = $text -replace '#define FW_VERSION ".*"', ('#define FW_VERSION "' + $Value + '"')
    if ($next -eq $text) {
        Fail "没有在 src\DeviceConfig.h 找到 FW_VERSION"
    }
    Set-Content -LiteralPath $path -Value $next -NoNewline
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

function Publish-HeadViaGitHubApi([string]$Message) {
    $remoteHead = (gh api "repos/$Repo/git/ref/heads/$Branch" --jq .object.sha).Trim()
    $baseTree = (gh api "repos/$Repo/git/commits/$remoteHead" --jq .tree.sha).Trim()
    $files = git diff-tree --no-commit-id --name-only -r HEAD
    if (-not $files) {
        Fail "当前 HEAD 没有可发布文件"
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
    git push origin $Branch
    if ($LASTEXITCODE -eq 0) {
        return (git rev-parse HEAD).Trim()
    }

    Write-Warning "git push 失败，改用 GitHub API 发布。"
    return Publish-HeadViaGitHubApi $Message
}

function Commit-And-Publish([string[]]$Files, [string]$Message) {
    git add -- $Files
    git commit -m $Message
    if ($LASTEXITCODE -ne 0) {
        Fail "git commit 失败：$Message"
    }
    return Push-Or-PublishViaApi $Message
}

function Update-PanelManifestUrl([string]$ManifestCommit) {
    if ($SkipPanelUpdate) {
        Write-Host "跳过网页控制台更新。"
        return
    }
    if (-not (Test-Path $PanelPath)) {
        Write-Warning "找不到网页控制台：$PanelPath"
        return
    }

    $url = "https://fastly.jsdelivr.net/gh/$Repo@$ManifestCommit/firmware/esp8266/ota.json"
    $escapedRepo = [regex]::Escape($Repo)
    $pattern = "https://fastly\.jsdelivr\.net/gh/$escapedRepo@[^`"']+/firmware/esp8266/ota\.json"
    $text = Get-Content -LiteralPath $PanelPath -Raw
    $next = [regex]::Replace($text, $pattern, $url)
    if ($next -eq $text) {
        Write-Warning "网页控制台里没有找到旧 ESP8266 OTA 清单地址，请手动检查。"
        return
    }
    Set-Content -LiteralPath $PanelPath -Value $next -NoNewline
    Write-Host "网页控制台已更新到清单 commit：$ManifestCommit"
}

Set-Location $PSScriptRoot

Step "检查工具"
Require-Command git
Require-Command gh
$platformio = Get-PlatformIo
gh auth status | Out-Host

Step "检查工作区"
$dirty = git status --porcelain -- firmware/esp8266/esp8266_wifi.bin firmware/esp8266/ota.json src/DeviceConfig.h src/OtaManager.cpp src/main.cpp src/MqttManager.cpp src/MqttManager.h src/OtaManager.h src/CommandHandler.cpp
if ($dirty) {
    Fail "OTA 相关文件还有未提交改动，请先处理：`n$dirty"
}

Step "设置固件版本 $Version"
Set-FirmwareVersion $Version

Step "编译固件"
& $platformio run -e esp8266_homekit
if ($LASTEXITCODE -ne 0) {
    Fail "PlatformIO 编译失败"
}

Step "复制固件到发布目录"
$buildBin = Join-Path $PSScriptRoot '.pio\build\esp8266_homekit\firmware.bin'
$releaseBin = Join-Path $PSScriptRoot 'firmware\esp8266\esp8266_wifi.bin'
Copy-Item -LiteralPath $buildBin -Destination $releaseBin -Force

Step "提交并发布固件 $Version"
$placeholderUrl = "https://fastly.jsdelivr.net/gh/$Repo@$Branch/firmware/esp8266/esp8266_wifi.bin"
Set-OtaManifest $Version $placeholderUrl
$releaseCommit = Commit-And-Publish @(
    'src/DeviceConfig.h',
    'firmware/esp8266/esp8266_wifi.bin',
    'firmware/esp8266/ota.json'
) "Release ESP8266 firmware v$Version"

Step "回填固定固件 URL"
$fixedBinUrl = "https://fastly.jsdelivr.net/gh/$Repo@$releaseCommit/firmware/esp8266/esp8266_wifi.bin"
Set-OtaManifest $Version $fixedBinUrl
$manifestCommit = Commit-And-Publish @('firmware/esp8266/ota.json') "Pin ESP8266 v$Version OTA binary URL"

Step "更新网页控制台"
Update-PanelManifestUrl $manifestCommit

Step "验证远端清单"
$manifestUrl = "https://fastly.jsdelivr.net/gh/$Repo@$manifestCommit/firmware/esp8266/ota.json"
$remoteManifest = Invoke-RestMethod -Uri "$manifestUrl?ts=$(Get-Date -Format yyyyMMddHHmmss)"
$remoteManifest | ConvertTo-Json -Compress | Write-Host

Write-Host ""
Write-Host "发布完成：" -ForegroundColor Green
Write-Host "版本：$Version"
Write-Host "清单：$manifestUrl"
Write-Host "固件：$fixedBinUrl"
Write-Host ""
Write-Host "测试：刷新网页控制台，选择 ESP8266 空调，点击检查更新。"
