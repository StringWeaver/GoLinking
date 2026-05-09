# Go c-shared + MSVC C++/WinRT Linking Test
$GoLibDir  = Join-Path $PSScriptRoot "golib"
$CppDir    = Join-Path $PSScriptRoot "cpp"
$BuildDir  = Join-Path $PSScriptRoot "build"

# Clean
if (Test-Path $BuildDir) { Remove-Item -Recurse -Force $BuildDir }
New-Item -ItemType Directory -Path $BuildDir | Out-Null

Write-Host "======================================================" -ForegroundColor Cyan
Write-Host " Go DLL + MSVC C++/WinRT Linking Test" -ForegroundColor Cyan
Write-Host "======================================================" -ForegroundColor Cyan
Write-Host ""

# ============================================================
# Step 1: Setup MSVC environment
# ============================================================
Write-Host "[Step 1] Setting up MSVC environment ..." -ForegroundColor Yellow

$VcVarsAll = "C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvarsall.bat"
if (-not (Test-Path $VcVarsAll)) {
    Write-Host "[ERROR] Cannot find vcvarsall.bat" -ForegroundColor Red
    exit 1
}

$tempFile = Join-Path $env:TEMP "vcvars_env.txt"
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

# ============================================================
# Step 2: Build Go DLL (c-shared)
# ============================================================
Write-Host "[Step 2] Building Go c-shared DLL ..." -ForegroundColor Yellow
Push-Location $GoLibDir
go build -buildmode=c-shared -o "$BuildDir\golib.dll" .
Pop-Location
if ($LASTEXITCODE -ne 0) { Write-Host "Go build FAILED" -ForegroundColor Red; exit 1 }
Write-Host "[Step 2] Done" -ForegroundColor Green
Write-Host ""

# ============================================================
# Step 3: Generate .lib import library from DLL
# ============================================================
Write-Host "[Step 3] Generating MSVC import library from DLL ..." -ForegroundColor Yellow
Push-Location $BuildDir

$exports = dumpbin /exports golib.dll | Select-String "^\s+\d+\s+[0-9A-F]+\s+[0-9A-F]+\s+(\S+)" | ForEach-Object { $_.Matches[0].Groups[1].Value }
Write-Host "  Exported symbols: $($exports -join ', ')" -ForegroundColor White

$defContent = "LIBRARY golib`nEXPORTS`n"
foreach ($sym in $exports) {
    $defContent += "    $sym`n"
}
Set-Content -Path "golib.def" -Value $defContent -Encoding ASCII

cmd /c "lib.exe /def:golib.def /out:golib.lib /machine:x64"
Pop-Location
if ($LASTEXITCODE -ne 0) { Write-Host "lib.exe FAILED" -ForegroundColor Red; exit 1 }
Write-Host "[Step 3] Done" -ForegroundColor Green
Write-Host ""

# ============================================================
# Step 4: MSVC compile + link with C++/WinRT
# ============================================================
Write-Host "[Step 4] MSVC compile + link (C++/WinRT) ..." -ForegroundColor Yellow

$compileBat = Join-Path $PSScriptRoot "compile_cpp.bat"
Push-Location $BuildDir
cmd /c """$compileBat"" ""$BuildDir"" ""$CppDir"""
Pop-Location
if ($LASTEXITCODE -ne 0) { Write-Host "MSVC compile/link FAILED" -ForegroundColor Red; exit 1 }
Write-Host "[Step 4] Done" -ForegroundColor Green
Write-Host ""

# ============================================================
# Step 5: Run the test
# ============================================================
Write-Host "[Step 5] Running test_winrt.exe (timeout 10s) ..." -ForegroundColor Yellow
Write-Host ""
Push-Location $BuildDir
$proc = Start-Process -FilePath ".\test_winrt.exe" -NoNewWindow -PassThru
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
