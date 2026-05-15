<#
.SYNOPSIS
    Bootstrap a Windows machine to build trilliumeng/orion-sdk.

.DESCRIPTION
    1. Detects any existing GNU C toolchain (gcc + GNU make) that can build
       the SDK. Scans well-known install locations and PATH, then runs a
       smoke compile to confirm it actually works.
    2. If a working toolchain is found, skips MSYS2 install and uses it.
    3. If not, installs MSYS2 + mingw-w64 toolchain via winget.
    4. Installs Git if missing.
    5. Clones orion-sdk, pins to latest release tag, runs ProtoGen,
       builds with the discovered/installed make.

    Toolchains it recognizes, in priority order:
      - MSYS2 mingw64       (C:\msys64\mingw64, C:\msys2\mingw64)
      - Standalone mingw-w64 (C:\mingw64)
      - Legacy MinGW         (C:\MinGW)
      - Strawberry Perl gcc  (C:\Strawberry\c)
      - Git for Windows MinGW64 (C:\Program Files\Git\mingw64)
      - Anything on PATH

    MSVC / nmake is intentionally NOT a candidate. The SDK's Makefiles
    are POSIX-style and would need rewriting for MSBuild.

.PARAMETER WorkDir
    Where to clone orion-sdk. Default: %USERPROFILE%\src

.PARAMETER ForceInstall
    Skip detection; install MSYS2 unconditionally.

.PARAMETER SkipBuild
    Stop after codegen; don't run make.

.PARAMETER NoSelfElevate
    Don't auto-relaunch via UAC if admin is needed. Just error out instead.
    Useful for CI or when calling from a wrapper that handles elevation.

.PARAMETER SkipWinget
    Skip winget entirely and go straight to direct GitHub-release downloads.
    Useful when winget is broken on the target box (0x8a15000f source-cache
    errors, msstore terms-of-use prompts, corporate Group Policy blocks).

.NOTES
    If installs are needed, the script will trigger a UAC prompt and
    relaunch itself elevated. If a toolchain is already present and Git
    is too, no elevation is needed and the script runs as-is.

.EXAMPLE
    PS> Set-ExecutionPolicy -Scope Process Bypass -Force
    PS> .\Bootstrap-OrionSDK.ps1
#>

[CmdletBinding()]
param(
    [string]$WorkDir = (Join-Path $env:USERPROFILE "src"),
    [switch]$ForceInstall,
    [switch]$SkipBuild,
    [switch]$NoSelfElevate,
    [switch]$SkipWinget
)

$ErrorActionPreference = 'Stop'

# Force TLS 1.2 for older Windows PowerShell that defaults to TLS 1.0/1.1
# (GitHub API requires 1.2+).
[Net.ServicePointManager]::SecurityProtocol =
    [Net.ServicePointManager]::SecurityProtocol -bor [Net.SecurityProtocolType]::Tls12

# =============================================================================
# Helpers
# =============================================================================

function Write-Stage($msg) {
    Write-Host ""
    Write-Host "=== $msg ===" -ForegroundColor Cyan
}

function Test-Cmd($cmd) {
    [bool](Get-Command $cmd -ErrorAction SilentlyContinue)
}

function Refresh-Path {
    $env:Path = [Environment]::GetEnvironmentVariable("Path", "Machine") + ";" +
                [Environment]::GetEnvironmentVariable("Path", "User")
}

function Test-Admin {
    $id = [Security.Principal.WindowsIdentity]::GetCurrent()
    (New-Object Security.Principal.WindowsPrincipal($id)).IsInRole(
        [Security.Principal.WindowsBuiltInRole]::Administrator)
}

# Same idea as Invoke-Bash but for arbitrary native commands (git, cmd,
# make, ProtoGen, etc). Drops EAP to 'Continue' so native stderr doesn't
# halt the script, then checks exit code.
function Invoke-Native {
    param(
        [Parameter(Mandatory)][scriptblock]$ScriptBlock,
        [int[]]$AllowedExitCodes = @(0),
        [string]$Description = "native command"
    )
    $prev = $ErrorActionPreference
    $ErrorActionPreference = 'Continue'
    try {
        & $ScriptBlock
        $exit = $LASTEXITCODE
    } finally {
        $ErrorActionPreference = $prev
    }
    if ($AllowedExitCodes -notcontains $exit) {
        throw "$Description failed (exit code $exit)"
    }
}

# Run a bash command and stream its output. Native binaries like bash,
# pacman, and make routinely write informational output to stderr; with
# $ErrorActionPreference='Stop' set globally, PS5.1 converts native stderr
# into a terminating "NativeCommandError" even when the command exits 0.
# This helper locally drops EAP to 'Continue' around the call so the
# command runs to completion, then drives the success check off the
# actual exit code.
function Invoke-Bash {
    param(
        [Parameter(Mandatory)][string]$BashPath,
        [Parameter(Mandatory)][string]$Command,
        [int[]]$AllowedExitCodes = @(0),
        [string]$Description = "bash command"
    )
    $prev = $ErrorActionPreference
    $ErrorActionPreference = 'Continue'
    try {
        & $BashPath -lc $Command 2>&1 | Out-Host
        $exit = $LASTEXITCODE
    } finally {
        $ErrorActionPreference = $prev
    }
    if ($AllowedExitCodes -notcontains $exit) {
        throw "$Description failed (exit code $exit)"
    }
}

# Relaunch the script via UAC. Forwards all parameters. The new admin
# window stays open (-NoExit) so the user can see the install/build output.
function Invoke-SelfElevate {
    $scriptPath = $PSCommandPath
    if (-not $scriptPath) {
        Write-Error "Cannot determine script path for self-elevation. Re-run elevated manually."
        exit 1
    }

    $argList = @(
        "-NoProfile",
        "-ExecutionPolicy", "Bypass",
        "-NoExit",
        "-File", "`"$scriptPath`""
    )

    # Forward bound params.
    if ($PSBoundParameters.ContainsKey('WorkDir')) {
        $argList += @("-WorkDir", "`"$WorkDir`"")
    }
    if ($ForceInstall)   { $argList += "-ForceInstall" }
    if ($SkipBuild)      { $argList += "-SkipBuild" }
    if ($SkipWinget)     { $argList += "-SkipWinget" }
    # NOT forwarding -NoSelfElevate -- if we got here we want to elevate.

    Write-Host ""
    Write-Host "Elevation required to install Git / MSYS2." -ForegroundColor Yellow
    Write-Host "Approve the UAC prompt; the elevated window will continue from here." -ForegroundColor Yellow
    try {
        Start-Process -FilePath "powershell.exe" `
                      -ArgumentList $argList `
                      -Verb RunAs -ErrorAction Stop | Out-Null
    } catch {
        Write-Error "UAC declined or elevation failed: $_"
        exit 1
    }
    Write-Host "Elevated session launched. This window can be closed." -ForegroundColor Green
    exit 0
}

# Compile a trivial C file with the given gcc. Returns $true on success.
function Test-CompilerWorks([string]$gccPath) {
    if (-not (Test-Path $gccPath)) { return $false }
    $tmp = Join-Path $env:TEMP "orion-cc-test-$(Get-Random)"
    New-Item -ItemType Directory -Path $tmp -Force | Out-Null
    try {
        $src = Join-Path $tmp "hello.c"
        $exe = Join-Path $tmp "hello.exe"
        'int main(void){return 0;}' | Set-Content $src -Encoding ASCII
        $null = & $gccPath -o $exe $src 2>&1
        return ($LASTEXITCODE -eq 0 -and (Test-Path $exe))
    } catch {
        return $false
    } finally {
        Remove-Item -Recurse -Force $tmp -ErrorAction SilentlyContinue
    }
}

# Verify the binary at $makePath is GNU Make (not nmake, not dmake).
function Test-IsGnuMake([string]$makePath) {
    if (-not (Test-Path $makePath)) { return $false }
    try {
        $out = (& $makePath --version 2>&1) -join "`n"
        return ($LASTEXITCODE -eq 0 -and $out -match 'GNU Make')
    } catch {
        return $false
    }
}

# =============================================================================
# Install helpers -- winget with repair + direct-download fallbacks
# =============================================================================

# Attempt to repair a broken winget source cache. Returns $true if repair
# completed without error. Resolves the common 0x8a15000f failure.
function Repair-WingetSource {
    Write-Host "Resetting winget sources..." -ForegroundColor Yellow
    try {
        winget source reset --force 2>&1 | Out-Host
        winget source update 2>&1 | Out-Host
        return ($LASTEXITCODE -eq 0)
    } catch {
        Write-Warning "winget source repair threw: $_"
        return $false
    }
}

# Download a GitHub release asset matching a regex. Returns the local path.
function Get-GitHubReleaseAsset {
    param(
        [Parameter(Mandatory)][string]$Repo,        # e.g. "git-for-windows/git"
        [Parameter(Mandatory)][string]$NameRegex,   # asset filename regex
        [string]$DestDir = $env:TEMP
    )
    $api = "https://api.github.com/repos/$Repo/releases/latest"
    $headers = @{ "User-Agent" = "orion-sdk-bootstrap" }
    Write-Host "Querying GitHub: $api"
    $release = Invoke-RestMethod -Uri $api -Headers $headers -UseBasicParsing
    $asset = $release.assets | Where-Object { $_.name -match $NameRegex } | Select-Object -First 1
    if (-not $asset) {
        throw "No asset in $Repo latest release matched /$NameRegex/"
    }
    $dest = Join-Path $DestDir $asset.name
    if (Test-Path $dest) { Remove-Item $dest -Force }
    $sizeMB = [math]::Round($asset.size / 1MB, 1)
    Write-Host "Downloading $($asset.name) ($sizeMB MB)..."
    Invoke-WebRequest -Uri $asset.browser_download_url -OutFile $dest `
        -UseBasicParsing -Headers $headers
    return $dest
}

# Direct download + silent install of Git for Windows.
function Install-GitDirect {
    $installer = Get-GitHubReleaseAsset `
        -Repo "git-for-windows/git" `
        -NameRegex '^Git-.*-64-bit\.exe$'
    Write-Host "Running Git installer silently..."
    $proc = Start-Process -FilePath $installer -PassThru -Wait -ArgumentList @(
        "/VERYSILENT", "/NORESTART", "/NOCANCEL", "/SP-",
        "/CLOSEAPPLICATIONS", "/RESTARTAPPLICATIONS",
        '/COMPONENTS="icons,ext\reg\shellhere,assoc,assoc_sh"'
    )
    Remove-Item $installer -Force -ErrorAction SilentlyContinue
    if ($proc.ExitCode -ne 0) {
        throw "Git installer exited with code $($proc.ExitCode)"
    }
}

# Direct download + silent install of MSYS2.
# Uses the .sfx.exe self-extractor rather than the Qt GUI installer --
# the SFX is a 7-zip self-extractor with reliable silent flags, while the
# Qt installer's --confirm-command silent mode is finicky and version-
# dependent. Tradeoff: SFX skips Start Menu shortcuts and Programs &
# Features registration, but the SDK build doesn't care.
function Install-MSYS2Direct {
    $installer = Get-GitHubReleaseAsset `
        -Repo "msys2/msys2-installer" `
        -NameRegex '^msys2-base-x86_64-.*\.sfx\.exe$'

    Write-Host "Extracting MSYS2 base to C:\ (archive contains msys64\)..."
    # 7-zip SFX switches: -y assume yes, -o<path> output directory.
    $proc = Start-Process -FilePath $installer -PassThru -Wait `
        -ArgumentList @("-y", "-oC:\")
    Remove-Item $installer -Force -ErrorAction SilentlyContinue
    if ($proc.ExitCode -ne 0) {
        throw "MSYS2 SFX extractor exited with code $($proc.ExitCode)"
    }

    $bash = "C:\msys64\usr\bin\bash.exe"
    if (-not (Test-Path $bash)) {
        throw "MSYS2 extracted but $bash missing -- archive layout changed?"
    }

    # First-launch initialization creates /etc/fstab, user home, etc.
    # Without this, pacman calls later in the script will fail.
    # The init prints "MSYS2 is starting for the first time..." to stderr,
    # which is informational -- Invoke-Bash tolerates that gracefully.
    Write-Host "Running MSYS2 first-launch initialization..."
    Invoke-Bash -BashPath $bash -Command "true" -Description "MSYS2 first-launch init"
}

# Install Git: try winget, repair sources on failure, fall back to direct.
# Verifies success by checking that `git` is on PATH (post-install).
function Install-Git {
    if ((Test-Cmd winget) -and -not $script:SkipWinget) {
        # winget attempt 1
        Write-Host "Attempting winget install of Git..."
        winget install --id Git.Git -e --silent `
            --accept-package-agreements --accept-source-agreements 2>&1 | Out-Host
        Refresh-Path
        if (Test-Cmd git) { Write-Host "Git installed via winget." -ForegroundColor Green; return }

        # Repair + winget attempt 2
        if (Repair-WingetSource) {
            Write-Host "Retrying winget install of Git..."
            winget install --id Git.Git -e --silent `
                --accept-package-agreements --accept-source-agreements 2>&1 | Out-Host
            Refresh-Path
            if (Test-Cmd git) { Write-Host "Git installed via winget (post-repair)." -ForegroundColor Green; return }
        }
        Write-Warning "winget unable to install Git. Falling back to direct GitHub download."
    } elseif ($script:SkipWinget) {
        Write-Host "-SkipWinget set. Using direct GitHub download for Git."
    } else {
        Write-Host "winget unavailable. Using direct GitHub download for Git."
    }

    Install-GitDirect
    Refresh-Path
    if (-not (Test-Cmd git)) {
        throw "Git install failed via both winget and direct download."
    }
    Write-Host "Git installed via direct download." -ForegroundColor Green
}

# Install MSYS2: try winget, repair sources on failure, fall back to direct.
# Verifies success by checking C:\msys64\usr\bin\bash.exe exists.
function Install-MSYS2 {
    $msys2Bash = "C:\msys64\usr\bin\bash.exe"

    if ((Test-Cmd winget) -and -not $script:SkipWinget) {
        # winget attempt 1
        Write-Host "Attempting winget install of MSYS2..."
        winget install --id MSYS2.MSYS2 -e --silent `
            --accept-package-agreements --accept-source-agreements 2>&1 | Out-Host
        if (Test-Path $msys2Bash) { Write-Host "MSYS2 installed via winget." -ForegroundColor Green; return }

        # Repair + winget attempt 2
        if (Repair-WingetSource) {
            Write-Host "Retrying winget install of MSYS2..."
            winget install --id MSYS2.MSYS2 -e --silent `
                --accept-package-agreements --accept-source-agreements 2>&1 | Out-Host
            if (Test-Path $msys2Bash) { Write-Host "MSYS2 installed via winget (post-repair)." -ForegroundColor Green; return }
        }
        Write-Warning "winget unable to install MSYS2. Falling back to direct GitHub download."
    } elseif ($script:SkipWinget) {
        Write-Host "-SkipWinget set. Using direct GitHub download for MSYS2."
    } else {
        Write-Host "winget unavailable. Using direct GitHub download for MSYS2."
    }

    Install-MSYS2Direct
    if (-not (Test-Path $msys2Bash)) {
        throw "MSYS2 install failed via both winget and direct download."
    }
    Write-Host "MSYS2 installed via direct download." -ForegroundColor Green
}

# =============================================================================
# Toolchain detection
# =============================================================================

function Find-Toolchain {
    $result = [pscustomobject]@{
        CC          = $null
        CCVersion   = $null
        CCDir       = $null
        Make        = $null
        MakeVersion = $null
        MakeDir     = $null
        Bash        = $null
        Source      = $null
        Verified    = $false
        Issues      = @()
    }

    # Priority-ordered candidates.
    $candidates = @(
        [pscustomobject]@{
            Source = "MSYS2 mingw64 (C:\msys64)"
            Cc     = "C:\msys64\mingw64\bin\gcc.exe"
            Mk     = "C:\msys64\usr\bin\make.exe"
            Bash   = "C:\msys64\usr\bin\bash.exe"
        }
        [pscustomobject]@{
            Source = "MSYS2 mingw64 (C:\msys2)"
            Cc     = "C:\msys2\mingw64\bin\gcc.exe"
            Mk     = "C:\msys2\usr\bin\make.exe"
            Bash   = "C:\msys2\usr\bin\bash.exe"
        }
        [pscustomobject]@{
            Source = "Standalone mingw-w64 (C:\mingw64)"
            Cc     = "C:\mingw64\bin\gcc.exe"
            Mk     = "C:\mingw64\bin\mingw32-make.exe"
            Bash   = $null
        }
        [pscustomobject]@{
            Source = "Legacy MinGW (C:\MinGW)"
            Cc     = "C:\MinGW\bin\gcc.exe"
            Mk     = "C:\MinGW\bin\mingw32-make.exe"
            Bash   = $null
        }
        [pscustomobject]@{
            Source = "Strawberry Perl gcc"
            Cc     = "C:\Strawberry\c\bin\gcc.exe"
            Mk     = "C:\Strawberry\c\bin\gmake.exe"
            Bash   = $null
        }
        [pscustomobject]@{
            Source = "Git for Windows MinGW64"
            Cc     = "C:\Program Files\Git\mingw64\bin\gcc.exe"
            Mk     = "C:\Program Files\Git\usr\bin\make.exe"
            Bash   = "C:\Program Files\Git\bin\bash.exe"
        }
    )

    foreach ($c in $candidates) {
        if (-not (Test-Path $c.Cc)) { continue }

        # Find a usable make near the compiler.
        $mkPath = $null
        $mkCandidates = @($c.Mk) + (
            @("make.exe", "mingw32-make.exe", "gmake.exe") | ForEach-Object {
                Join-Path (Split-Path $c.Mk) $_
            }
        )
        foreach ($m in $mkCandidates | Select-Object -Unique) {
            if (Test-Path $m) { $mkPath = $m; break }
        }

        if (-not $mkPath) {
            $result.Issues += "$($c.Source): gcc found at $($c.Cc) but no make alongside"
            continue
        }
        if (-not (Test-CompilerWorks $c.Cc)) {
            $result.Issues += "$($c.Source): gcc exists but smoke compile failed"
            continue
        }
        if (-not (Test-IsGnuMake $mkPath)) {
            $result.Issues += "$($c.Source): $mkPath is not GNU Make"
            continue
        }

        # Winner.
        $result.CC          = $c.Cc
        $result.CCDir       = Split-Path $c.Cc
        $result.CCVersion   = (& $c.Cc --version 2>&1 | Select-Object -First 1)
        $result.Make        = $mkPath
        $result.MakeDir     = Split-Path $mkPath
        $result.MakeVersion = (& $mkPath --version 2>&1 | Select-Object -First 1)
        $result.Bash        = if ($c.Bash -and (Test-Path $c.Bash)) { $c.Bash } else { $null }
        $result.Source      = $c.Source
        $result.Verified    = $true
        return $result
    }

    # Last resort: scan PATH.
    $pathGcc  = Get-Command gcc.exe          -ErrorAction SilentlyContinue
    $pathMake = Get-Command make.exe         -ErrorAction SilentlyContinue
    if (-not $pathMake) {
        $pathMake = Get-Command mingw32-make.exe -ErrorAction SilentlyContinue
    }
    if ($pathGcc -and $pathMake) {
        $gccOk  = Test-CompilerWorks $pathGcc.Source
        $makeOk = Test-IsGnuMake     $pathMake.Source
        if ($gccOk -and $makeOk) {
            $pathBash = Get-Command bash.exe -ErrorAction SilentlyContinue
            $result.CC          = $pathGcc.Source
            $result.CCDir       = Split-Path $pathGcc.Source
            $result.CCVersion   = (& $pathGcc.Source --version 2>&1 | Select-Object -First 1)
            $result.Make        = $pathMake.Source
            $result.MakeDir     = Split-Path $pathMake.Source
            $result.MakeVersion = (& $pathMake.Source --version 2>&1 | Select-Object -First 1)
            $result.Bash        = if ($pathBash) { $pathBash.Source } else { $null }
            $result.Source      = "PATH"
            $result.Verified    = $true
        } else {
            if (-not $gccOk)  { $result.Issues += "PATH gcc ($($pathGcc.Source)) failed smoke compile" }
            if (-not $makeOk) { $result.Issues += "PATH make ($($pathMake.Source)) is not GNU Make" }
        }
    }

    return $result
}

# =============================================================================
# Main flow
# =============================================================================

# --- 1. Detect ---
Write-Stage "Detecting C toolchain"
$tc = $null
if ($ForceInstall) {
    Write-Host "-ForceInstall set: skipping detection, installing MSYS2."
} else {
    $tc = Find-Toolchain
    if ($tc.Verified) {
        Write-Host "Working toolchain found:" -ForegroundColor Green
        Write-Host "  Source:   $($tc.Source)"
        Write-Host "  CC:       $($tc.CC)"
        Write-Host "  CC ver:   $($tc.CCVersion)"
        Write-Host "  Make:     $($tc.Make)"
        Write-Host "  Make ver: $($tc.MakeVersion)"
        if ($tc.Bash) { Write-Host "  Bash:     $($tc.Bash)" }
    } else {
        Write-Host "No working toolchain found."
        if ($tc.Issues.Count -gt 0) {
            Write-Host "Detection notes:"
            $tc.Issues | ForEach-Object { Write-Host "  - $_" -ForegroundColor Yellow }
        }
    }
}

$needsInstall = $ForceInstall -or -not ($tc -and $tc.Verified)

# --- 2. Admin check, only if installing ---
if ($needsInstall -or -not (Test-Cmd git)) {
    if (-not (Test-Admin)) {
        if ($NoSelfElevate) {
            Write-Error "Need admin to install Git/MSYS2. Re-run elevated, or install them manually and retry. (-NoSelfElevate is set, so not relaunching automatically.)"
            exit 1
        }
        Invoke-SelfElevate
        # Invoke-SelfElevate exits; control does not return.
    }
    if (-not (Test-Cmd winget)) {
        Write-Warning "winget not found. Will use direct GitHub-release downloads instead."
    }
}

# --- 3. Git ---
Write-Stage "Git"
if (-not (Test-Cmd git)) {
    Install-Git
} else {
    Write-Host "git present: $(git --version)"
}

# --- 4. MSYS2 only if needed ---
if ($needsInstall) {
    Write-Stage "Installing MSYS2 (no working toolchain detected)"
    $msys2Root = "C:\msys64"
    if (-not (Test-Path $msys2Root)) {
        Install-MSYS2
        if (-not (Test-Path $msys2Root)) {
            Write-Error "MSYS2 install reported success but $msys2Root missing."
            exit 1
        }
    }
    $bash = Join-Path $msys2Root "usr\bin\bash.exe"
    if (-not (Test-Path $bash)) {
        Write-Error "MSYS2 bash.exe missing. Reinstall MSYS2."
        exit 1
    }

    Write-Stage "Updating MSYS2 + installing toolchain"
    # First-pass: may self-update pacman and force-exit the shell.
    Invoke-Bash -BashPath $bash `
        -Command "pacman -Syu --noconfirm --disable-download-timeout" `
        -Description "pacman first-pass update"
    # Second-pass: finishes the system update.
    Invoke-Bash -BashPath $bash `
        -Command "pacman -Syu --noconfirm --disable-download-timeout" `
        -Description "pacman second-pass update"
    # Toolchain install.
    Invoke-Bash -BashPath $bash `
        -Command "pacman -S --needed --noconfirm --disable-download-timeout mingw-w64-x86_64-toolchain mingw-w64-x86_64-make make git" `
        -Description "pacman toolchain install"

    Write-Stage "Adding MSYS2 to system PATH"
    $paths = @("$msys2Root\mingw64\bin", "$msys2Root\usr\bin")
    $machinePath = [Environment]::GetEnvironmentVariable("Path", "Machine")
    foreach ($p in $paths) {
        if (($machinePath -split ';') -notcontains $p) {
            $machinePath = "$machinePath;$p"
            Write-Host "  + $p"
        }
    }
    [Environment]::SetEnvironmentVariable("Path", $machinePath, "Machine")
    Refresh-Path

    # Re-detect now that MSYS2 is installed.
    $tc = Find-Toolchain
    if (-not $tc.Verified) {
        Write-Error "MSYS2 installed but toolchain still not verified. Aborting."
        exit 1
    }
    Write-Host "Post-install toolchain:" -ForegroundColor Green
    Write-Host "  $($tc.CCVersion)"
    Write-Host "  $($tc.MakeVersion)"
}

# Ensure the active toolchain bin dirs are on this session's PATH.
foreach ($d in @($tc.CCDir, $tc.MakeDir) | Select-Object -Unique) {
    if ($d -and ($env:Path -split ';') -notcontains $d) {
        $env:Path = "$d;$env:Path"
    }
}

# --- 5. Clone ---
Write-Stage "Cloning orion-sdk"
New-Item -ItemType Directory -Force -Path $WorkDir | Out-Null
$repoDir = Join-Path $WorkDir "orion-sdk"
if (-not (Test-Path $repoDir)) {
    Set-Location $WorkDir
    Invoke-Native -Description "git clone" -ScriptBlock {
        git clone https://github.com/trilliumeng/orion-sdk.git
    }
} else {
    Set-Location $repoDir
    Write-Host "Repo present, fetching..."
    Invoke-Native -Description "git fetch" -ScriptBlock {
        git fetch --tags --prune
    }
}
Set-Location $repoDir

# --- 6. Pin to latest release tag ---
Write-Stage "Checking out latest release tag"
$latestTag = (git tag --sort=-creatordate | Select-Object -First 1)
if ($latestTag) {
    Write-Host "Tag: $latestTag"
    Invoke-Native -Description "git checkout $latestTag" -ScriptBlock {
        git -c advice.detachedHead=false checkout $latestTag
    }
} else {
    Write-Warning "No tags found; staying on default branch."
}

# --- 7. ProtoGen ---
Write-Stage "Running ProtoGen"
if (-not (Test-Path ".\GenerateOrionPublicPacketWin.bat")) {
    Write-Error "GenerateOrionPublicPacketWin.bat not at repo root. Wrong branch/tag?"
    exit 1
}
Invoke-Native -Description "ProtoGen codegen" -ScriptBlock {
    cmd /c "GenerateOrionPublicPacketWin.bat"
}
$generated = Get-ChildItem ".\Communications\*.h" -ErrorAction SilentlyContinue
if (-not $generated) {
    Write-Error "ProtoGen produced no headers. Inspect ProtoGen.exe output above."
    exit 1
}
Write-Host "Generated headers in Communications\:"
$generated | ForEach-Object { Write-Host "  $($_.Name)" }
if (-not (Test-Path ".\Communications\CameraInformation.h")) {
    Write-Warning "CameraInformation.h NOT generated. ProtoGen may be too old to honor split-file attributes."
}

# --- 8. Build ---
if ($SkipBuild) {
    Write-Stage "Skipping build (-SkipBuild)"
} else {
    Write-Stage "Building SDK"
    if ($tc.Bash) {
        # Prefer bash -- the Makefile is POSIX-style.
        $repoUnix  = ($repoDir  -replace '\\','/' -replace '^([A-Za-z]):','/$1').ToLower()
        $makeUnix  = ($tc.Make  -replace '\\','/')
        Invoke-Bash -BashPath $tc.Bash `
            -Command "cd '$repoUnix' && '$makeUnix'" `
            -Description "make build"
    } else {
        # No bash. Call make directly (works for plain mingw32-make).
        $prevEAP = $ErrorActionPreference
        $ErrorActionPreference = 'Continue'
        try {
            & $tc.Make
            $makeExit = $LASTEXITCODE
        } finally {
            $ErrorActionPreference = $prevEAP
        }
        if ($makeExit -ne 0) {
            throw "make exited with code $makeExit"
        }
    }
}

# --- 9. Summary ---
Write-Stage "Done"
Write-Host "Repo:        $repoDir"
Write-Host "CC:          $($tc.CC)"
Write-Host "Make:        $($tc.Make)"
if ($tc.Bash) { Write-Host "Bash:        $($tc.Bash)" }
Write-Host "Toolchain:   $($tc.Source)"
Write-Host ""
Write-Host "Future builds:"
if ($tc.Bash) {
    Write-Host "  & '$($tc.Bash)' -lc 'cd $repoDir && make'"
} else {
    Write-Host "  cd '$repoDir'; & '$($tc.Make)'"
}
