@echo off
setlocal
cd /d "%~dp0"
set VERSION=0.1.3

where cl >nul 2>nul || (
  for /f "usebackq delims=" %%i in (`"%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe" -latest -property installationPath`) do call "%%i\VC\Auxiliary\Build\vcvars64.bat" >nul
)
where cl >nul 2>nul || (echo Visual Studio C++ build tools not found. & exit /b 1)

if not exist bin mkdir bin
cl /nologo /std:c++17 /utf-8 /EHsc /W4 /O2 /DUNICODE /D_UNICODE /Fo:bin\ src\LaptopFanCurve.cpp /Fe:bin\LaptopFanCurve.exe ^
   /link /SUBSYSTEM:WINDOWS /MANIFEST:EMBED /MANIFESTUAC:"level='requireAdministrator'"
if errorlevel 1 exit /b 1
rem The administrator requirement must be embedded in the exe; a separate .manifest file is not shipped.
findstr /m /c:"requireAdministrator" bin\LaptopFanCurve.exe >nul || (echo ERROR: manifest is not embedded in bin\LaptopFanCurve.exe & exit /b 1)
del /q bin\*.obj bin\*.manifest 2>nul
copy /y third_party\PawnIO.Modules\LpcACPIEC.bin bin\ >nul

rem Release zip: exe, PawnIO module and license texts.
if not exist dist mkdir dist
set STAGE=dist\LaptopFanCurve-%VERSION%-win-x64
if exist "%STAGE%" rmdir /s /q "%STAGE%"
mkdir "%STAGE%\licenses"
copy /y bin\LaptopFanCurve.exe "%STAGE%\" >nul
copy /y bin\LpcACPIEC.bin "%STAGE%\" >nul
copy /y README.md "%STAGE%\" >nul
copy /y LICENSE "%STAGE%\licenses\LaptopFanCurve-LICENSE.txt" >nul
copy /y THIRD-PARTY-NOTICES.md "%STAGE%\licenses\" >nul
copy /y third_party\PawnIO.Modules\COPYING "%STAGE%\licenses\PawnIO.Modules-LGPL-2.1.txt" >nul
powershell -NoProfile -Command "Compress-Archive -Force -Path '%STAGE%\*' -DestinationPath '%STAGE%.zip'"
if errorlevel 1 exit /b 1
echo Built bin\LaptopFanCurve.exe and %STAGE%.zip
