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

.NOTES
    Requires elevated PowerShell only if MSYS2 / Git need installing.
    If a toolchain is already present and Git is too, this can run
    non-elevated.

.EXAMPLE
    PS> Set-ExecutionPolicy -Scope Process Bypass -Force
    PS> .\Bootstrap-OrionSDK.ps1
#>

[CmdletBinding()]
param(
    [string]$WorkDir = (Join-Path $env:USERPROFILE "src"),
    [switch]$ForceInstall,
    [switch]$SkipBuild
)

$ErrorActionPreference = 'Stop'

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
        Write-Error "Need admin to install Git/MSYS2. Re-run elevated, or install them manually and retry."
        exit 1
    }
    if (-not (Test-Cmd winget)) {
        Write-Error "winget missing. Install 'App Installer' from Microsoft Store, then retry."
        exit 1
    }
}

# --- 3. Git ---
Write-Stage "Git"
if (-not (Test-Cmd git)) {
    winget install --id Git.Git -e --silent `
        --accept-package-agreements --accept-source-agreements
    Refresh-Path
} else {
    Write-Host "git present: $(git --version)"
}

# --- 4. MSYS2 only if needed ---
if ($needsInstall) {
    Write-Stage "Installing MSYS2 (no working toolchain detected)"
    $msys2Root = "C:\msys64"
    if (-not (Test-Path $msys2Root)) {
        winget install --id MSYS2.MSYS2 -e --silent `
            --accept-package-agreements --accept-source-agreements
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
    & $bash -lc "pacman -Syu --noconfirm --disable-download-timeout"
    & $bash -lc "pacman -Syu --noconfirm --disable-download-timeout"
    & $bash -lc "pacman -S --needed --noconfirm --disable-download-timeout `
        mingw-w64-x86_64-toolchain mingw-w64-x86_64-make make git"

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
    git clone https://github.com/trilliumeng/orion-sdk.git
} else {
    Set-Location $repoDir
    Write-Host "Repo present, fetching..."
    git fetch --tags --prune
}
Set-Location $repoDir

# --- 6. Pin to latest release tag ---
Write-Stage "Checking out latest release tag"
$latestTag = (git tag --sort=-creatordate | Select-Object -First 1)
if ($latestTag) {
    Write-Host "Tag: $latestTag"
    git -c advice.detachedHead=false checkout $latestTag
} else {
    Write-Warning "No tags found; staying on default branch."
}

# --- 7. ProtoGen ---
Write-Stage "Running ProtoGen"
if (-not (Test-Path ".\GenerateOrionPublicPacketWin.bat")) {
    Write-Error "GenerateOrionPublicPacketWin.bat not at repo root. Wrong branch/tag?"
    exit 1
}
cmd /c "GenerateOrionPublicPacketWin.bat"
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
        # Prefer bash — the Makefile is POSIX-style.
        $repoUnix  = ($repoDir  -replace '\\','/' -replace '^([A-Za-z]):','/$1').ToLower()
        $makeUnix  = ($tc.Make  -replace '\\','/')
        & $tc.Bash -lc "cd '$repoUnix' && '$makeUnix'"
    } else {
        # No bash. Call make directly (works for plain mingw32-make).
        & $tc.Make
    }
    if ($LASTEXITCODE -ne 0) {
        Write-Error "make exited with $LASTEXITCODE"
        exit $LASTEXITCODE
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
