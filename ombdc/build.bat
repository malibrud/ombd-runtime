@echo off
rem ---------------------------------------------------------------------------
rem  build.bat -- unity build of ombdc, the OMBD front end, and its tests.
rem
rem  Each program is one translation unit, compiled from scratch:
rem      src\unity.cpp          -^> build\ombdc.exe
rem      tests\unity_tests.cpp  -^> build\ombdc_tests.exe
rem  Then the checks run, and any failure fails the build:
rem      the unit tests and the golden dumps (tests\*.tokens, *.ast, *.names, *.types)
rem      every .ombd under ..\..\OMBD_Specification\Examples must parse
rem
rem      build.bat            release
rem      build.bat debug      /Od /Zi
rem
rem  ASCII + CRLF only: cmd mis-splits an LF-only batch file.
rem ---------------------------------------------------------------------------
setlocal
cd /d "%~dp0"

set "EXAMPLES=%~dp0..\..\OMBD_Specification\Examples"

rem --- Find cl: use it from PATH, or set up VS's x64 tools via vswhere ------
where cl >nul 2>nul
if not errorlevel 1 goto build

set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if not exist "%VSWHERE%" (
    echo build: vswhere.exe not found - install Visual Studio with the C++ workload.
    exit /b 1
)
for /f "usebackq tokens=*" %%i in (`"%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VSINSTALL=%%i"
if not defined VSINSTALL (
    echo build: Visual Studio C++ tools not found.
    exit /b 1
)
call "%VSINSTALL%\VC\Auxiliary\Build\vcvars64.bat" >nul 2>nul
if errorlevel 1 exit /b 1

:build
if not exist build mkdir build

rem  /W4 /WX      every warning, as an error
rem  /utf-8       sources and literals are UTF-8 (the lexer tests hold non-ASCII operators)
set FLAGS=/nologo /std:c++20 /W4 /WX /permissive- /utf-8 /D_CRT_SECURE_NO_WARNINGS
if /I "%~1"=="debug" (
    set FLAGS=%FLAGS% /Od /Zi /MDd
) else (
    set FLAGS=%FLAGS% /O2 /MD /DNDEBUG
)

echo === ombdc ===
cl %FLAGS% /Fobuild\ombdc.obj /Fdbuild\ombdc.pdb /Fe:build\ombdc.exe src\unity.cpp
if errorlevel 1 (
    echo build: ombdc FAILED
    exit /b 1
)

echo === ombdc_tests ===
cl %FLAGS% /Fobuild\ombdc_tests.obj /Fdbuild\ombdc_tests.pdb /Fe:build\ombdc_tests.exe tests\unity_tests.cpp
if errorlevel 1 (
    echo build: ombdc_tests FAILED
    exit /b 1
)

echo === unit tests ===
build\ombdc_tests.exe "%EXAMPLES%" tests
if errorlevel 1 (
    echo build: UNIT TESTS FAILED
    exit /b 1
)

echo === parse every example ===
set FAILED=0
for /r "%EXAMPLES%" %%f in (*.ombd) do (
    build\ombdc.exe --ast "%%f" >nul
    if errorlevel 1 (
        set FAILED=1
    ) else (
        echo   ok  %%f
    )
)
if not "%FAILED%"=="0" (
    echo build: AN EXAMPLE FAILED TO PARSE
    exit /b 1
)

echo build: build\ombdc.exe, all checks passed
endlocal
