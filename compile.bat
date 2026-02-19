@echo off
REM =========================================
REM SimpleSteamIdler build script - Double click friendly
REM =========================================

REM --- Configura la ruta a tu Visual Studio 2026 ---
REM Cambia la ruta si tu VS está en otra ubicación
set VS_DEV_CMD="D:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat"

if not exist %VS_DEV_CMD% (
    echo ERROR: No se encontro vcvars64.bat. Revisa la ruta en build.bat
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

echo.
echo Iniciando entorno de compilacion de Visual Studio...
call %VS_DEV_CMD%

echo.
echo Compilando recursos...
rc.exe /fo %RESOURCE_OBJ% %RESOURCES%
if errorlevel 1 (
    echo Error compilando recursos.
    pause
    exit /b 1
)

echo.
echo Compilando codigo fuente...
cl.exe /EHsc /I"%INCLUDE%" /c %SRC% >nul
if errorlevel 1 (
    echo Error compilando codigo fuente.
    pause
    exit /b 1
)

echo.
echo Linkeando ejecutable...
link %OBJ% %RESOURCE_OBJ% /OUT:%OUT% >nul
if errorlevel 1 (
    echo Error linkeando ejecutable.
    pause
    exit /b 1
)

REM --- Limpieza de archivos temporales ---
echo.
echo Limpiando archivos temporales...
del /f /q %OBJ% >nul 2>nul
del /f /q %RESOURCE_OBJ% >nul 2>nul

echo.
echo Compilacion completada correctamente: %OUT%
pause
