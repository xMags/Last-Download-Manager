$ErrorActionPreference = "Stop"

$cmakePath = "C:\Program Files\Microsoft Visual Studio\2022\Community\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin"
$env:PATH = "$cmakePath;$env:PATH"

Write-Host "Configuring project with CMake..." -ForegroundColor Cyan

Set-Location build

& cmake .. -DCMAKE_TOOLCHAIN_FILE=C:/vcpkg/scripts/buildsystems/vcpkg.cmake

if ($LASTEXITCODE -ne 0) {
    Write-Host "CMake configuration failed!" -ForegroundColor Red
    exit $LASTEXITCODE
}

Write-Host "`nBuilding project..." -ForegroundColor Cyan

& cmake --build . --config Release

if ($LASTEXITCODE -ne 0) {
    Write-Host "Build failed!" -ForegroundColor Red
    exit $LASTEXITCODE
}

Write-Host "`nBuild completed successfully!" -ForegroundColor Green
Write-Host "Executable location: build\Release\LastDM.exe" -ForegroundColor Green
