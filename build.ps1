param(
    [switch]$NoLaunch,
    [switch]$Deploy,
    [switch]$RunTests
)

$ErrorActionPreference = "Stop"
$env:PATH = "C:\msys64\mingw64\bin;" + $env:PATH

$buildDir = Join-Path $PSScriptRoot "build"

if (-not (Test-Path "$buildDir\CMakeCache.txt")) {
    Write-Host "Configuring..." -ForegroundColor Cyan
    & cmake -S $PSScriptRoot -B $buildDir -G Ninja `
        -DCMAKE_C_COMPILER=gcc `
        -DCMAKE_CXX_COMPILER=g++ `
        -DCMAKE_BUILD_TYPE=Debug
    if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
}

Write-Host "Building..." -ForegroundColor Cyan
& cmake --build $buildDir
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }

$exe = Join-Path $buildDir "MalloyStudio.exe"
if (-not (Test-Path $exe)) {
    Write-Host "Build succeeded but $exe not found" -ForegroundColor Yellow
    exit 1
}

# Copies every non-system DLL the given binaries need into their own directory.
#
# Windows searches the application directory before PATH, so a build that
# carries its dependencies starts the same way no matter what else is
# installed. Without this the app loads whichever MinGW runtime the ambient
# PATH happens to find first: a shell with another toolchain in front of MSYS2
# (Git Bash ships one) makes it fail at startup with "Entry Point Not Found",
# naming a symbol rather than the real problem.
function Copy-RuntimeDependencies {
    param(
        [Parameter(Mandatory = $true)][string]$Directory,
        [string]$MingwBin = "C:\msys64\mingw64\bin"
    )

    if (-not (Test-Path $MingwBin)) {
        Write-Host "  MinGW bin not found at $MingwBin, skipping" -ForegroundColor Yellow
        return
    }

    $seen = @{}
    $queue = New-Object System.Collections.Queue
    foreach ($f in Get-ChildItem -Path $Directory -Recurse -Include *.exe, *.dll) {
        $queue.Enqueue($f.FullName)
    }

    $copied = 0
    while ($queue.Count -gt 0) {
        $file = $queue.Dequeue()
        if (-not (Test-Path $file)) { continue }

        $imports = & objdump -p $file 2>$null |
            Select-String -Pattern '^\s*DLL Name:\s*(.+)$' |
            ForEach-Object { $_.Matches[0].Groups[1].Value.Trim() }

        foreach ($name in $imports) {
            $key = $name.ToLowerInvariant()
            if ($seen.ContainsKey($key)) { continue }
            $seen[$key] = $true

            $source = Join-Path $MingwBin $name
            # Anything not in the MinGW tree is a Windows system DLL and must
            # NOT be copied: those come from the OS.
            if (-not (Test-Path $source)) { continue }

            $target = Join-Path (Split-Path -Parent $file) $name
            if (-not (Test-Path $target)) {
                $target = Join-Path $Directory $name
            }
            if (-not (Test-Path $target)) {
                Copy-Item -LiteralPath $source -Destination $target
                $copied++
            }
            $queue.Enqueue($target)
        }
    }
    Write-Host "  $copied runtime DLL(s) copied" -ForegroundColor DarkGray
}

if ($Deploy) {
    Write-Host "Deploying Qt runtime..." -ForegroundColor Cyan
    # --compiler-runtime brings libstdc++, libgcc and libwinpthread; the sweep
    # below picks up everything else Qt pulls in, ICU included.
    & windeployqt6.exe --debug --compiler-runtime $exe
    if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }

    Write-Host "Resolving remaining runtime dependencies..." -ForegroundColor Cyan
    Copy-RuntimeDependencies -Directory $buildDir
}

if ($RunTests) {
    Write-Host "Running tests..." -ForegroundColor Cyan
    & ctest --test-dir $buildDir --output-on-failure
    if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
}

if ($NoLaunch -or $RunTests) {
    Write-Host "Build complete: $exe" -ForegroundColor Green
} else {
    Write-Host "Launching $exe" -ForegroundColor Green
    & $exe
}
