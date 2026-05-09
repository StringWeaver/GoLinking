# Purego + MSVC C++/WinRT Build Script (Zero CGO!)
$RootDir     = $PSScriptRoot
$CppDir      = Join-Path $RootDir "cpp_dll"
$GoDir       = Join-Path $RootDir "gomain"
$BuildDir    = Join-Path $RootDir "build"

Write-Host "======================================================" -ForegroundColor Cyan
Write-Host " Purego + C++/WinRT Build (Zero CGO!)" -ForegroundColor Cyan
Write-Host "======================================================" -ForegroundColor Cyan
Write-Host ""

if (Test-Path $BuildDir) { Remove-Item -Recurse -Force $BuildDir }
New-Item -ItemType Directory -Path $BuildDir | Out-Null

Write-Host "[Step 1] Setting up MSVC environment ..." -ForegroundColor Yellow
$VcVarsAll = "C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvarsall.bat"
if (-not (Test-Path $VcVarsAll)) {
    $VcVarsAll = "C:\Program Files\Microsoft Visual Studio\17\Community\VC\Auxiliary\Build\vcvarsall.bat"
}
if (-not (Test-Path $VcVarsAll)) {
    Write-Host "[ERROR] Cannot find vcvarsall.bat" -ForegroundColor Red
    exit 1
}
$tempFile = Join-Path $env:TEMP "vcvars_env_purego.txt"
cmd /c """$VcVarsAll"" x64 >nul 2>&1 && set > ""$tempFile"""
Get-Content $tempFile | ForEach-Object {
    $idx = $_.IndexOf("=")
    if ($idx -gt 0) {
        $key = $_.Substring(0, $idx)
        $val = $_.Substring($idx + 1)
        [System.Environment]::SetEnvironmentVariable($key, $val, "Process")
    }
}
Remove-Item $tempFile -ErrorAction SilentlyContinue
Write-Host "[Step 1] Done" -ForegroundColor Green
Write-Host ""

Write-Host "[Step 2] Compiling C++ DLL (C++/WinRT) ..." -ForegroundColor Yellow
Push-Location $CppDir
$cppSources = @("dllmain.cpp", "winrt_exports.cpp") | ForEach-Object { Join-Path $CppDir $_ }
$outputDll = Join-Path $BuildDir "winrt_dll.dll"
cl.exe /EHsc /std:c++20 /LD $cppSources /link WindowsApp.lib /OUT:$outputDll /DLL
Pop-Location
if ($LASTEXITCODE -ne 0) { Write-Host "C++ DLL build FAILED" -ForegroundColor Red; exit 1 }
Write-Host "[Step 2] Done" -ForegroundColor Green
Write-Host ""

Write-Host "[Step 3] Building Go program (purego, NO CGO!) ..." -ForegroundColor Yellow
Push-Location $GoDir
$env:CGO_ENABLED = "0"
go mod tidy
go build -o (Join-Path $BuildDir "purego_winrt.exe") .
Pop-Location
if ($LASTEXITCODE -ne 0) { Write-Host "Go build FAILED" -ForegroundColor Red; exit 1 }
Write-Host "[Step 3] Done" -ForegroundColor Green
Write-Host ""

Write-Host "[Step 4] Running purego_winrt.exe (timeout 10s) ..." -ForegroundColor Yellow
Write-Host ""
Push-Location $BuildDir
$proc = Start-Process -FilePath ".\purego_winrt.exe" -NoNewWindow -PassThru
$exited = $proc.WaitForExit(10000)
if (-not $exited) {
    Write-Host "Program HUNG! Killing ..." -ForegroundColor Red
    Stop-Process -Id $proc.Id -Force -ErrorAction SilentlyContinue
    Write-Host "RESULT: Program hangs at runtime" -ForegroundColor Red
} else {
    Write-Host ""
    Write-Host "======================================================" -ForegroundColor Green
    Write-Host " Test completed! Exit code: $($proc.ExitCode)" -ForegroundColor Green
    Write-Host "======================================================" -ForegroundColor Green
}
Pop-Location
