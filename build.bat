@echo off
rem Builds bin\dinput8.dll and bin\eqcore_check.exe (x64, Release) with Visual Studio 2022.
setlocal
for /f "usebackq tokens=*" %%i in (`"%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set VSDIR=%%i
if "%VSDIR%"=="" (echo Visual Studio 2022 with C++ tools not found & exit /b 1)
call "%VSDIR%\VC\Auxiliary\Build\vcvars64.bat" >nul || exit /b 1
cmake -S "%~dp0." -B "%~dp0build" -G "Visual Studio 17 2022" -A x64 || exit /b 1
cmake --build "%~dp0build" --config Release || exit /b 1
if not exist "%~dp0bin" mkdir "%~dp0bin"
copy /y "%~dp0build\Release\dinput8.dll" "%~dp0bin\" >nul
copy /y "%~dp0build\Release\eqcore_check.exe" "%~dp0bin\" >nul
echo Built bin\dinput8.dll and bin\eqcore_check.exe
