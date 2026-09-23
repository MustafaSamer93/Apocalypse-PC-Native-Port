@echo off
rem Builds the native Windows version of Apocalypse from your own disc image.
rem   1. put "Apocalypse (USA).cue" and its .bin files in rom\
rem   2. run build.bat
rem   3. run build-clang\apocalypse.exe (from this folder)
setlocal
cd /d "%~dp0"

set VCVARS=C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvars64.bat
if not exist "%VCVARS%" (
    for /f "usebackq tokens=*" %%i in (`"%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe" -latest -property installationPath`) do set VCVARS=%%i\VC\Auxiliary\Build\vcvars64.bat
)
call "%VCVARS%" >nul || (echo Visual Studio C++ tools not found & exit /b 1)
set PATH=C:\Program Files\Git\cmd;%PATH%

if not exist "rom\Apocalypse (USA).cue" (echo Put your disc image in rom\ first & exit /b 1)

echo [1/4] Extracting the executable from the disc...
python tools\discfs.py extract >nul || exit /b 1

echo [2/4] Recompiling MIPS code to C...
python tools\recomp\main.py --no-ra-check || exit /b 1

echo [3/4] Configuring...
cmake -S . -B build-clang -G Ninja -DCMAKE_BUILD_TYPE=Release -DRECOMP_RA_CHECK=OFF ^
      -DCMAKE_C_COMPILER=clang-cl -DCMAKE_CXX_COMPILER=clang-cl >nul || exit /b 1

echo [4/4] Building...
cmake --build build-clang || exit /b 1

echo.
echo Done: build-clang\apocalypse.exe
