@echo off

set "ELEVATED_MARKER=__elevated"

if /I "%~1"=="%ELEVATED_MARKER%" goto :build

net session >NUL 2>NUL
if errorlevel 1 (
	echo Requesting administrator privileges...
	powershell -NoProfile -ExecutionPolicy Bypass -Command "try { Start-Process -FilePath 'cmd.exe' -Verb RunAs -ArgumentList '/c','cd /d ""%~dp0"" && call ""%~f0"" %ELEVATED_MARKER%'; exit 0 } catch { exit 1 }"
	if errorlevel 1 (
		echo UAC elevation was canceled or failed.
		exit /b 1
	)
	exit /b 0
)

:build

taskkill /F /IM AutoMuteApp.exe >NUL 2>NUL
taskkill /F /IM AutoMuteApp_test.exe >NUL 2>NUL

tasklist /FI "IMAGENAME eq AutoMuteApp.exe" | find /I "AutoMuteApp.exe" >NUL
if %ERRORLEVEL%==0 (
	echo AutoMuteApp.exe is still running. Close it manually or run this script as Administrator.
	exit /b 1
)

tasklist /FI "IMAGENAME eq AutoMuteApp_test.exe" | find /I "AutoMuteApp_test.exe" >NUL
if %ERRORLEVEL%==0 (
	echo AutoMuteApp_test.exe is still running. Close it manually or run this script as Administrator.
	exit /b 1
)

windres app.rc -O coff -o app.res
if errorlevel 1 (
	echo Failed to compile app.rc resource file.
	exit /b 1
)

gcc src/*.c app.res -Iinclude -lole32 -luuid -lpropsys -lpsapi -ladvapi32 -mwindows -o AutoMuteApp.exe