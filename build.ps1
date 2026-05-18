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

if ($Deploy) {
    Write-Host "Deploying Qt runtime..." -ForegroundColor Cyan
    & windeployqt6.exe --debug $exe
    if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
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
