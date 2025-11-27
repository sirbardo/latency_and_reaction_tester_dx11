@echo off
REM Debug build script for Latency Tester
REM Run from a Visual Studio Developer Command Prompt

echo Building Latency Tester (Debug)...

cl.exe /nologo /EHsc /Od /MTd /W4 /Zi ^
    /D "WIN32" /D "_DEBUG" /D "_WINDOWS" /D "_UNICODE" /D "UNICODE" ^
    main.cpp ^
    /link /SUBSYSTEM:WINDOWS /DEBUG ^
    d3d11.lib dxgi.lib d2d1.lib dwrite.lib user32.lib ^
    /OUT:LatencyTester_debug.exe

if %ERRORLEVEL% EQU 0 (
    echo Build successful! Run LatencyTester_debug.exe
) else (
    echo Build failed with error %ERRORLEVEL%
)
