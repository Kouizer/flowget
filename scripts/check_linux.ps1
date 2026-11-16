#Requires -Version 5.1
<#
.SYNOPSIS
    Builds and tests the Linux presets inside WSL - the same steps the CI `linux` job runs.

.DESCRIPTION
    Two stages:

    1. Mirror the Windows checkout into the WSL clone (~/flowget) with rsync. `build/` and `.git/`
       are excluded on both ends: the WSL side keeps its CMake/vcpkg/FetchContent caches and its
       own Git metadata, so a sync never throws away a warm build tree.
    2. Run `linux-x64` (debug), `linux-x64-asan` (ASan/UBSan) and `linux-x64-tsan` (ThreadSanitizer)
       configure, build and ctest through `wsl -d Ubuntu -- bash -lc`.

    Why bother: MSVC and Clang reject different code with -Werror on. Diagnostics such as
    -Wunused-lambda-capture or -Wsign-conversion are Clang-only, so a green Windows build says
    nothing about the Linux job. See tasks/lessons.md.

    Details that are easy to get wrong (all of them are handled below):

    * `bash -lc` is a *login* shell and does not read ~/.bashrc, so FLOWGET_VCPKG_ROOT exported
      there is invisible. The script exports it (unless the caller already set it) before
      configuring, otherwise CMake stops with "No vcpkg root found".
    * Each stage is written to a temporary file and run as `bash <file>`. Passing it as one
      `bash -lc "<script>"` argument looked natural but silently broke: PowerShell 5.1 does not
      escape embedded double quotes for native commands and wsl.exe re-splits the command line, so
      bash received only the first word (`set`) and every stage exited 0 without doing anything - a
      green result for no work. A file removes the quoting layer entirely.
    * The temporary script is normalised to LF before it reaches bash. A CRLF script makes bash pass
      `linux-x64\r` to `cmake --preset`, which fails with a misleading "unknown preset" error.
    * The temporary script file is passed last on the command line as a single plain path; there is
      no wrapper quoting to get wrong.
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

# wsl.exe strips backslashes from arguments before the Linux side sees them (observed as
# "C:UsersEnesAppData..." reaching wslpath), so Windows -> /mnt/<drive> translation is done here
# instead of shelling out to wslpath. The script only ever deals with local temp/repo paths.
function ConvertTo-WslPath {
    [CmdletBinding()]
    param([Parameter(Mandatory)] [string] $WindowsPath)

    $full = [System.IO.Path]::GetFullPath($WindowsPath)
    if ($full -notmatch '^([A-Za-z]):\\(.*)$') {
        throw "'$WindowsPath' is not an absolute local path"
    }
    $drive = $Matches[1].ToLowerInvariant()
    $rest = $Matches[2] -replace '\\', '/'
    "/mnt/$drive/$rest"
}

function Invoke-WslScript {
    [CmdletBinding()]
    param(
        [Parameter(Mandatory)] [string] $Script,
        [Parameter(Mandatory)] [string] $FailureMessage
    )

    # The script is written to a temporary file and executed with `bash <file>` instead of being
    # passed as one `bash -lc "<script>"` argument. A multi-line argument containing double quotes
    # is split by PowerShell 5.1's native-command quoting and by wsl.exe's argv handling: bash then
    # receives only the first word ("set") and the whole stage silently does nothing while still
    # exiting 0 - a false green. A file has no quoting layer at all.
    $normalized = $Script -replace "`r`n", "`n"
    $scriptFile = Join-Path ([System.IO.Path]::GetTempPath()) `
        ("flowget-check-" + [guid]::NewGuid().ToString('N') + ".sh")
    [System.IO.File]::WriteAllText($scriptFile, $normalized, [System.Text.UTF8Encoding]::new($false))

    try {
        $linuxPath = ConvertTo-WslPath $scriptFile

        # cmake and ctest report progress on stderr. Windows PowerShell 5.1 turns native stderr
        # writes into a terminating NativeCommandError when the preference variable is `Stop`, which
        # would abort a perfectly healthy build, so the native call runs in `Continue` scope.
        $ErrorActionPreference = 'Continue'
        & wsl -d $Distro -- bash $linuxPath
        $status = $LASTEXITCODE
        if ($status -ne 0) {
            throw "$FailureMessage (exit code $status)"
        }
    }
    finally {
        Remove-Item -LiteralPath $scriptFile -ErrorAction SilentlyContinue
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

    $source = ConvertTo-WslPath $repoRoot
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
# The trailing slash on '$source/' is load-bearing: without it rsync copies the directory itself
# into ~/flowget/flowget and the build tree keeps testing November sources while every stage exits
# 0 (observed on 2026-11-16, see tasks/lessons.md).
rsync -a --checksum --delete --exclude /build/ --exclude /.git/ '$source/' ~/flowget/
"@

    Write-Host '==> linux-x64 (debug) + linux-x64-asan (ASan/UBSan) + linux-x64-tsan (TSan)' -ForegroundColor Cyan
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

echo '=== linux-x64-tsan: configure ==='
cmake --preset linux-x64-tsan
echo '=== linux-x64-tsan: build ==='
cmake --build --preset linux-x64-tsan
echo '=== linux-x64-tsan: ctest ==='
ctest --test-dir build/linux-x64-tsan --output-on-failure

echo 'Linux presets OK: linux-x64 (debug), linux-x64-asan (ASan + UBSan) and linux-x64-tsan (TSan)'
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
