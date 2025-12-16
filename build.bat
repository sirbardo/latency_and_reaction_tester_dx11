@echo off
setlocal EnableDelayedExpansion
REM Build script for Latency Tester and Reaction Tester
REM Tries MSVC (cl.exe) first, falls back to GCC (g++) if not found

echo Building Latency Tester and Reaction Tester...

REM Check if cl.exe is available
where cl.exe >nul 2>&1
if %ERRORLEVEL% EQU 0 (
    echo Using MSVC compiler...

    echo.
    echo [1/2] Building LatencyTester.exe...
    cl.exe /nologo /EHsc /O2 /MT /W4 ^
        /D "WIN32" /D "NDEBUG" /D "_WINDOWS" /D "_UNICODE" /D "UNICODE" ^
        main.cpp ^
        /link /SUBSYSTEM:WINDOWS ^
        d3d11.lib dxgi.lib d2d1.lib dwrite.lib user32.lib ^
        /OUT:LatencyTester.exe

    if !ERRORLEVEL! EQU 0 (
        echo LatencyTester.exe built successfully
    ) else (
        echo LatencyTester.exe build failed with error !ERRORLEVEL!
    )

    echo.
    echo [2/2] Building ReactionTester.exe...
    cl.exe /nologo /EHsc /O2 /MT /W4 ^
        /D "WIN32" /D "NDEBUG" /D "_WINDOWS" /D "_UNICODE" /D "UNICODE" ^
        reaction.cpp ^
        /link /SUBSYSTEM:WINDOWS ^
        d3d11.lib dxgi.lib d2d1.lib dwrite.lib user32.lib ole32.lib ^
        /OUT:ReactionTester.exe

    if !ERRORLEVEL! EQU 0 (
        echo ReactionTester.exe built successfully
    ) else (
        echo ReactionTester.exe build failed with error !ERRORLEVEL!
    )

    del *.obj 2>nul
    echo.
    echo Done!
    goto :eof
)

REM Check if g++ is available
where g++.exe >nul 2>&1
if %ERRORLEVEL% EQU 0 (
    echo Using GCC/MinGW compiler...

    echo.
    echo [1/2] Building LatencyTester.exe...
    g++.exe -o LatencyTester.exe main.cpp ^
        -O3 -Wall -mwindows -municode ^
        -DWIN32 -DNDEBUG -D_WINDOWS -DUNICODE -D_UNICODE ^
        -ld3d11 -ldxgi -ld2d1 -ldwrite -luser32 -lole32 -luuid

    if !ERRORLEVEL! EQU 0 (
        echo LatencyTester.exe built successfully
    ) else (
        echo LatencyTester.exe build failed with error !ERRORLEVEL!
    )

    echo.
    echo [2/2] Building ReactionTester.exe...
    g++.exe -o ReactionTester.exe reaction.cpp ^
        -O3 -Wall -mwindows -municode ^
        -DWIN32 -DNDEBUG -D_WINDOWS -DUNICODE -D_UNICODE ^
        -ld3d11 -ldxgi -ld2d1 -ldwrite -luser32 -lole32 -luuid

    if !ERRORLEVEL! EQU 0 (
        echo ReactionTester.exe built successfully
    ) else (
        echo ReactionTester.exe build failed with error !ERRORLEVEL!
    )

    echo.
    echo Done!
    goto :eof
)

echo ERROR: No compiler found!
echo Please either:
echo   - Run from a Visual Studio Developer Command Prompt (for cl.exe)
echo   - Install MinGW-w64 and add it to PATH (for g++)
exit /b 1
