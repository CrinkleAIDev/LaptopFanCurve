@echo off
setlocal
cd /d "%~dp0"
where cl >nul 2>nul || (
  for /f "usebackq delims=" %%i in (`"%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe" -latest -property installationPath`) do call "%%i\VC\Auxiliary\Build\vcvars64.bat" >nul
)
if not exist ..\bin mkdir ..\bin
cl /nologo /std:c++17 /utf-8 /EHsc /W4 /DUNICODE /D_UNICODE /Fo:..\bin\ Tests.cpp /Fe:..\bin\Tests.exe /link /SUBSYSTEM:CONSOLE
if errorlevel 1 exit /b 1
..\bin\Tests.exe
