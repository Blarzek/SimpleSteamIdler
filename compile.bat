@ECHO OFF
TITLE SimpleSteamIdle compilation
setlocal EnableDelayedExpansion
chcp 65001 >nul

REM =========================================
REM SimpleSteamIdler build script - Double click friendly
REM =========================================

REM Colores
for /F %%a in ('echo prompt $E ^| cmd') do set "_esc=%%a"
set red_on=%_esc%[91m
set green_on=%_esc%[92m
set yellow_on=%_esc%[33m
set blue_on=%_esc%[36m
set light_blue_on=%_esc%[96m
set color_off=%_esc%[0m

echo %yellow_on%===========================================================%color_off%
echo %yellow_on%=== Welcome to SimpleSteamIdler compilation, by Blarzek ===%color_off%
echo %yellow_on%===========================================================%color_off%
echo.

REM --- Configura la ruta a tu Visual Studio 2026 ---
REM Cambia la ruta si tu VS está en otra ubicación
set VS_DEV_CMD="D:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvars64.bat"

if not exist %VS_DEV_CMD% (
    echo %red_on%ERROR: No se encontró vcvars64.bat. Revisa la ruta en build.bat%color_off%
    echo.
    pause
    exit /b 1
)

REM --- Configura variables del proyecto ---
set SRC=src\SimpleSteamIdler.cpp
set RESOURCES=resources\resources.rc
set RESOURCE_OBJ=resources\resources.res
set OBJ=SimpleSteamIdler.obj
set OUT=SimpleSteamIdler.exe
set INCLUDE=resources


echo %light_blue_on%(1/5) Iniciando entorno de compilación de Visual Studio...%color_off%
call %VS_DEV_CMD% >nul

echo %light_blue_on%(2/5) Compilando recursos...%color_off%
rc.exe /fo %RESOURCE_OBJ% %RESOURCES% >nul
if errorlevel 1 (
    echo %red_on%Error compilando recursos.
    pause
    exit /b 1
)

echo %light_blue_on%(3/5) Compilando código fuente...%color_off%
cl.exe /EHsc /I"%INCLUDE%" /c %SRC% >nul 2>&1
if errorlevel 1 (
    echo %red_on%Error compilando código fuente.%color_off%
    pause
    exit /b 1
)

echo %light_blue_on%(4/5) Linkeando ejecutable...%color_off%
link %OBJ% %RESOURCE_OBJ% /OUT:%OUT% >nul
if errorlevel 1 (
    echo %red_on%Error linkeando ejecutable.%color_off%
    pause
    exit /b 1
)

REM --- Limpieza de archivos temporales ---
echo %light_blue_on%(5/5) Limpiando archivos temporales...%color_off%
del /f /q %OBJ% >nul 2>nul
del /f /q %RESOURCE_OBJ% >nul 2>nul

echo.
echo %green_on%Compilación completada correctamente: %OUT%%color_off%
echo.
pause
