@echo off
setlocal
cd /d "%~dp0"
set MSBUILD=C:\Program Files\Microsoft Visual Studio\2022\Community\MSBuild\Current\Bin\amd64\MSBuild.exe
if not exist "%MSBUILD%" set MSBUILD=C:\Program Files (x86)\Microsoft Visual Studio\18\BuildTools\MSBuild\Current\Bin\amd64\MSBuild.exe
if not exist "%MSBUILD%" (
  echo MSBuild not found.
  pause
  exit /b 1
)
"%MSBUILD%" CodexAssumptionNative.vcxproj /p:Configuration=Release /p:Platform=x64 /m
pause
