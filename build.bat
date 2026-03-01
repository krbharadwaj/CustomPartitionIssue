@echo off
REM ==========================================================================
REM  Build script for PartitionDemo
REM  Requires: Visual Studio with C++ workload
REM
REM  Option A: Run setup.bat first to download WebView2 NuGet package
REM  Option B: Set WV2_HEADERS and WV2_LIB env vars to point to your headers
REM ==========================================================================

echo [1/3] Setting up Visual Studio environment...
if not defined VSCMD_VER (
    REM Try VS 2022+ paths
    for %%V in (
        "C:\Program Files\Microsoft Visual Studio\2022\Enterprise\VC\Auxiliary\Build\vcvars64.bat"
        "C:\Program Files\Microsoft Visual Studio\2022\Professional\VC\Auxiliary\Build\vcvars64.bat"
        "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat"
        "C:\Program Files\Microsoft Visual Studio\18\Enterprise\VC\Auxiliary\Build\vcvars64.bat"
    ) do (
        if exist %%V (
            call %%V >nul 2>&1
            goto :vs_found
        )
    )
    echo ERROR: Could not find Visual Studio. Open a Developer Command Prompt instead.
    exit /b 1
)
:vs_found

REM Detect WebView2 headers location
if not defined WV2_HEADERS (
    REM Check for NuGet package first
    for /d %%D in (packages\Microsoft.Web.WebView2.*) do (
        set WV2_HEADERS=%%D\build\native\include
        set WV2_LIB=%%D\build\native\x64
    )
)

if not defined WV2_HEADERS (
    echo ERROR: WebView2 headers not found.
    echo   Run setup.bat first, or set WV2_HEADERS and WV2_LIB env vars.
    exit /b 1
)

echo   Headers: %WV2_HEADERS%
echo   Lib:     %WV2_LIB%

echo [2/3] Compiling main.cpp...
cl.exe /nologo /EHsc /std:c++17 /W3 /O2 ^
    main.cpp ^
    /I"%WV2_HEADERS%" ^
    /Fe:PartitionDemo.exe ^
    /link ^
    /LIBPATH:"%WV2_LIB%" ^
    WebView2LoaderStatic.lib ^
    user32.lib ole32.lib oleaut32.lib ^
    shlwapi.lib advapi32.lib

if errorlevel 1 (
    echo.
    echo BUILD FAILED
    exit /b 1
)

echo [3/3] Cleaning up...
del /q main.obj 2>nul

echo.
echo ========================================
echo  BUILD SUCCEEDED: PartitionDemo.exe
echo  Run: PartitionDemo.exe
echo ========================================
