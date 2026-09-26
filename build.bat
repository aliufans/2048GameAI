@echo off
rem ===========================================================================
rem  一键编译 2048（自动挑选可用的 C++ 编译器：g++ / zig / MSVC cl）
rem  会先把 app.rc（图标 + 版本信息）编译成资源，再一起链接进 exe
rem  用法：双击本文件，或在命令行执行 build.bat
rem ===========================================================================
setlocal enabledelayedexpansion
cd /d "%~dp0"

set SRC=game2048.cpp
set GUI=game2048.exe
set BENCH=game2048_bench.exe
set RES=app.res

rem ---- 先编译资源（图标 / 版本信息） ----
if exist "%RES%" del /q "%RES%"
where zig >nul 2>nul
if !errorlevel! equ 0 zig rc app.rc %RES%
if not exist "%RES%" (
    where windres >nul 2>nul
    if !errorlevel! equ 0 windres app.rc -O coff -o %RES%
)
set RESARG=
if exist "%RES%" (
    set RESARG=%RES%
) else (
    echo [警告] 资源没编译成功（需要 zig 或 windres），本次将不带图标编译。
)

where g++ >nul 2>nul
if !errorlevel! equ 0 (
    echo [g++] 编译图形版 %GUI% ...
    g++ -O2 -std=c++17 -o %GUI% %SRC% %RESARG% -mwindows -lgdi32 -luser32 -static -static-libgcc -static-libstdc++
    if !errorlevel! neq 0 goto fail
    echo [g++] 编译压测版 %BENCH% ...
    g++ -O2 -std=c++17 -DCONSOLE_BUILD -o %BENCH% %SRC% %RESARG% -static -static-libgcc -static-libstdc++
    if !errorlevel! neq 0 goto fail
    goto ok
)

where zig >nul 2>nul
if !errorlevel! equ 0 (
    echo [zig c++] 编译图形版 %GUI% ...
    zig c++ -O2 -std=c++17 -o %GUI% %SRC% %RESARG% -Wl,--subsystem,windows -lgdi32 -luser32 -static
    if !errorlevel! neq 0 goto fail
    echo [zig c++] 编译压测版 %BENCH% ...
    zig c++ -O2 -std=c++17 -DCONSOLE_BUILD -o %BENCH% %SRC% %RESARG% -static
    if !errorlevel! neq 0 goto fail
    goto ok
)

where cl >nul 2>nul
if !errorlevel! equ 0 (
    echo [MSVC cl] 编译图形版 %GUI% ...
    cl /nologo /O2 /EHsc /std:c++17 /utf-8 /DUNICODE /D_UNICODE %SRC% %RESARG% /Fe:%GUI% /Fo:obj_ /link /SUBSYSTEM:WINDOWS user32.lib gdi32.lib
    if !errorlevel! neq 0 goto fail
    echo [MSVC cl] 编译压测版 %BENCH% ...
    cl /nologo /O2 /EHsc /std:c++17 /utf-8 /DCONSOLE_BUILD %SRC% %RESARG% /Fe:%BENCH% /Fo:obj_ /link /SUBSYSTEM:CONSOLE user32.lib gdi32.lib
    if !errorlevel! neq 0 goto fail
    goto ok
)

echo.
echo 没有找到 C++ 编译器（g++ / zig / cl）。
echo 可任选一种方式安装后再运行本脚本：
echo   1) MSYS2/MinGW-w64：pacman -S mingw-w64-x86_64-gcc
echo   2) Zig（自带 clang）：pip install ziglang   然后把 zig 的目录加入 PATH
echo   3) Visual Studio 安装「使用 C++ 的桌面开发」工作负载
exit /b 1

:ok
del /q *.pdb >nul 2>nul
del /q obj_ >nul 2>nul
echo.
echo ============================================
echo  编译完成！
echo    游戏：   %GUI%          （双击即可运行）
echo    压测：   %BENCH% --bench 4 30
echo ============================================
exit /b 0

:fail
echo.
echo 编译失败，请查看上面的错误信息。
exit /b 1
