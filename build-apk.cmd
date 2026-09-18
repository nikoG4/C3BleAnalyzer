@echo off
setlocal
cd /d "%~dp0"

set "ANDROID_SDK=%LOCALAPPDATA%\Android\Sdk"

if not exist "%ANDROID_SDK%" (
  echo [ERROR] No se encontro Android SDK en:
  echo %ANDROID_SDK%
  echo.
  echo Edita build-apk.cmd o crea local.properties manualmente.
  exit /b 1
)

set "SDK_PROP=%ANDROID_SDK:\=/%"
> local.properties echo sdk.dir=%SDK_PROP%

echo [OK] Android SDK: %ANDROID_SDK%
echo [BUILD] Compilando APK debug...

gradle :app:assembleDebug
if errorlevel 1 exit /b %errorlevel%

echo.
echo [OK] APK generado:
echo %CD%\app\build\outputs\apk\debug\app-debug.apk
endlocal
