#Requires -Version 5.1
<#
.SYNOPSIS
    Builds and tests the Linux presets inside WSL - the same steps the CI `linux` job runs.

.DESCRIPTION
    Two stages:

    1. Mirror the Windows checkout into the WSL clone (~/flowget) with rsync. `build/` and `.git/`
       are excluded on both ends: the WSL side keeps its CMake/vcpkg/FetchContent caches and its
       own Git metadata, so a sync never throws away a warm build tree.
    2. Run `linux-x64` (debug) and `linux-x64-asan` (sanitizers) configure, build and ctest through
       `wsl -d Ubuntu -- bash -lc`.

    Why bother: MSVC and Clang reject different code with -Werror on. Diagnostics such as
    -Wunused-lambda-capture or -Wsign-conversion are Clang-only, so a green Windows build says
    nothing about the Linux job. See tasks/lessons.md.

    Details that are easy to get wrong (all of them are handled below):

    * `bash -lc` is a *login* shell and does not read ~/.bashrc, so FLOWGET_VCPKG_ROOT exported
      there is invisible. The script exports it (unless the caller already set it) before
      configuring, otherwise CMake stops with "No vcpkg root found".
    * The remote script is normalised to LF before it reaches bash. A CRLF script makes bash pass
      `linux-x64\r` to `cmake --preset`, which fails with a misleading "unknown preset" error.
    * wsl.exe joins the remaining argv into one command line, so each remote script is handed over
      as a single argument wrapped in double quotes; without that bash only sees the first word.
    * rsync compares content (--checksum), not size/mtime: WSL's drvfs layer was seen serving a
      just-updated timestamp stale, which made rsync skip a fresh edit while ninja then reported
      "no work to do" - a green result for old sources. mtimes still propagate from the source
      (-a) so ninja rebuilds exactly the files that were transferred.

.PARAMETER Distro
    WSL distribution name. Default: Ubuntu.

.EXAMPLE
    ./scripts/check_linux.ps1

.EXAMPLE
    ./scripts/check_linux.ps1 -Distro Ubuntu-24.04

.OUTPUTS
    Exit code 0 when both presets build and their test suites pass, 1 otherwise.
#>
[CmdletBinding()]
param(
    [ValidateNotNullOrEmpty()]
    [string] $Distro = 'Ubuntu'
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$repoRoot = (Resolve-Path -LiteralPath (Join-Path $PSScriptRoot '..')).Path

function Invoke-WslScript {
    [CmdletBinding()]
    param(
        [Parameter(Mandatory)] [string] $Script,
        [Parameter(Mandatory)] [string] $FailureMessage
    )

    # Normalise line endings: the file may be checked out as CRLF (.gitattributes) and bash would
    # treat the trailing CR as part of the last word on every line.
    $normalized = $Script -replace "`r`n", "`n"

    # cmake and ctest report progress on stderr. Windows PowerShell 5.1 turns native stderr writes
    # into a terminating NativeCommandError when the preference variable is `Stop`, which would
    # abort a perfectly healthy build, so the native call runs in `Continue` scope.
    $ErrorActionPreference = 'Continue'
    & wsl -d $Distro -- bash -lc "`"$normalized`""
    $status = $LASTEXITCODE
    if ($status -ne 0) {
        throw "$FailureMessage (exit code $status)"
    }
}

try {
    if (-not (Get-Command wsl -ErrorAction SilentlyContinue)) {
        throw 'wsl.exe was not found. Install WSL2 (wsl --install) and retry.'
    }

    Write-Host "==> WSL distro '$Distro'" -ForegroundColor Cyan
    Invoke-WslScript -FailureMessage "Cannot run a shell inside the WSL distro '$Distro' (check 'wsl -l -v')" -Script @'
set -e
for tool in cmake ctest ninja clang++ rsync; do
    command -v $tool > /dev/null || {
        echo "error: '$tool' is missing on the Linux side."
        echo '       CI installs the toolchain with: sudo apt-get install -y ninja-build clang'
        exit 1
    }
done
echo "cmake:   $(cmake --version | head -n 1)"
echo "clang++: $(clang++ --version | head -n 1)"
echo "ninja:   $(ninja --version)"
'@

    $source = (& wsl -d $Distro -- bash -lc "`"wslpath -a '$repoRoot'`"").Trim()
    if ($LASTEXITCODE -ne 0 -or [string]::IsNullOrWhiteSpace($source)) {
        throw "wslpath could not translate '$repoRoot' to a Linux path"
    }

    Write-Host "==> rsync $repoRoot -> $source => ~/flowget (excluding build/ and .git/)" -ForegroundColor Cyan
    Invoke-WslScript -FailureMessage 'rsync into the WSL clone failed' -Script @"
set -e
mkdir -p ~/flowget
# --checksum: rsync normally decides "unchanged" from size + mtime, and those values travel
# through WSL's drvfs/9p layer, which was observed serving a just-updated timestamp stale for a
# couple of seconds - the edit was skipped and ninja then reported "no work to do", i.e. a green
# result for old sources. Comparing content removes that failure mode. mtimes still come from the
# source (-a implies -t) so ninja rebuilds exactly the files that were transferred; do not add
# --no-times here, it updates content but leaves the old destination mtime and ninja skips it.
rsync -a --checksum --delete --exclude /build/ --exclude /.git/ '$source' ~/flowget/
"@

    Write-Host '==> linux-x64 (debug) + linux-x64-asan (sanitizers)' -ForegroundColor Cyan
    $watch = [System.Diagnostics.Stopwatch]::StartNew()
    Invoke-WslScript -FailureMessage 'Linux build/test failed - see the log above' -Script @'
set -e
export FLOWGET_VCPKG_ROOT=${FLOWGET_VCPKG_ROOT:-$HOME/vcpkg}
if [ ! -f "$FLOWGET_VCPKG_ROOT/scripts/buildsystems/vcpkg.cmake" ]; then
    echo "error: no vcpkg installation at $FLOWGET_VCPKG_ROOT"
    echo '       clone it (git clone https://github.com/microsoft/vcpkg ~/vcpkg) and run bootstrap-vcpkg.sh'
    exit 1
fi
echo "vcpkg: $FLOWGET_VCPKG_ROOT"
cd ~/flowget

echo '=== linux-x64: configure ==='
cmake --preset linux-x64
echo '=== linux-x64: build ==='
cmake --build --preset linux-x64
echo '=== linux-x64: ctest ==='
ctest --test-dir build/linux-x64 --output-on-failure

echo '=== linux-x64-asan: configure ==='
cmake --preset linux-x64-asan
echo '=== linux-x64-asan: build ==='
cmake --build --preset linux-x64-asan
echo '=== linux-x64-asan: ctest ==='
ctest --test-dir build/linux-x64-asan --output-on-failure

echo 'Linux presets OK: linux-x64 (debug) and linux-x64-asan (ASan + UBSan)'
'@
    $watch.Stop()

    $elapsed = '{0:0.0}' -f $watch.Elapsed.TotalMinutes
    Write-Host "==> PASSED in $elapsed min" -ForegroundColor Green
    exit 0
}
catch {
    Write-Host ''
    Write-Host "==> FAILED: $($_.Exception.Message)" -ForegroundColor Red
    exit 1
}
