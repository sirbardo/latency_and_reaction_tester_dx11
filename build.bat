@echo off
REM Build script for Latency Tester
REM Run from a Visual Studio Developer Command Prompt

echo Building Latency Tester...

cl.exe /nologo /EHsc /O2 /MT /W4 ^
    /D "WIN32" /D "NDEBUG" /D "_WINDOWS" /D "_UNICODE" /D "UNICODE" ^
    main.cpp ^
    /link /SUBSYSTEM:WINDOWS ^
    d3d11.lib dxgi.lib d2d1.lib dwrite.lib user32.lib ^
    /OUT:LatencyTester.exe

if %ERRORLEVEL% EQU 0 (
    echo Build successful! Run LatencyTester.exe
    del *.obj 2>nul
) else (
    echo Build failed with error %ERRORLEVEL%
)
