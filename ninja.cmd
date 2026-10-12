@echo off
rem Usage: ninja.cmd <build-dir> [ninja args...]   (configures with cmake first)
setlocal
set "PATH=%PATH%;C:\Program Files (x86)\Microsoft Visual Studio\Installer"
call "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat" >nul || exit /b 1
cmake -S "%~dp0." -B "%~1" >nul || exit /b 1
set "B=%~1"
shift
ninja -C "%B%" %1 %2 %3 %4 %5 %6 %7 %8 %9
