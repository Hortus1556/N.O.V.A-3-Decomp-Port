@echo off
rem Usage: build.cmd <build-dir> <gen-dir> [cmake --build args...]
setlocal
set "SRC=%~dp0."
set "BUILD=%~1"
set "GEN=%~2"
set "PATH=%PATH%;C:\Program Files (x86)\Microsoft Visual Studio\Installer"
call "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat" >nul || exit /b 1
cmake -S "%SRC%" -B "%BUILD%" -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_C_COMPILER="C:/Program Files/LLVM/bin/clang-cl.exe" -DCMAKE_CXX_COMPILER="C:/Program Files/LLVM/bin/clang-cl.exe" -DCMAKE_LINKER="C:/Program Files/LLVM/bin/lld-link.exe" -DGEN_DIR="%GEN%" >nul || exit /b 1
shift & shift
cmake --build "%BUILD%" %1 %2 %3 %4
