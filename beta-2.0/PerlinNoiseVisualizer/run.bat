@echo off
setlocal
if exist build\Release\PerlinNoiseVisualizer.exe (
    cd build\Release
    PerlinNoiseVisualizer.exe
    cd ..\..
) else (
    echo PerlinNoiseVisualizer.exe not found. Run build.bat first.
)
endlocal
