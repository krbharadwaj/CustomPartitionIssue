@echo off
REM ==========================================================================
REM  Setup: Downloads WebView2 SDK NuGet package
REM  Run this once before building.
REM ==========================================================================

echo [Setup] Downloading WebView2 SDK NuGet package...

where nuget >nul 2>&1
if errorlevel 1 (
    echo nuget.exe not found. Downloading...
    powershell -Command "Invoke-WebRequest -Uri 'https://dist.nuget.org/win-x86-commandline/latest/nuget.exe' -OutFile 'nuget.exe'"
    if errorlevel 1 (
        echo ERROR: Failed to download nuget.exe
        exit /b 1
    )
    set NUGET=nuget.exe
) else (
    set NUGET=nuget
)

%NUGET% install Microsoft.Web.WebView2 -Version 1.0.2739.15 -OutputDirectory packages
if errorlevel 1 (
    echo ERROR: Failed to download WebView2 SDK
    exit /b 1
)

echo.
echo ========================================
echo  Setup complete! Now run: build.bat
echo ========================================
