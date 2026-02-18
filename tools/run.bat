@echo off
setlocal EnableExtensions

set "DEFAULT_APPID="

if exist steam_appid.txt (
    set /p DEFAULT_APPID=<steam_appid.txt
)

if defined DEFAULT_APPID (
    echo.
    echo AppID guardado anteriormente: %DEFAULT_APPID%
    echo.
    set /p "INPUT_APPID=Introduce el AppID del juego de Steam (ENTER para usar el guardado): "
) else (
    echo.
    set /p "INPUT_APPID=Introduce el AppID del juego de Steam: "
)

if "%INPUT_APPID%"=="" (
    if defined DEFAULT_APPID (
        set "INPUT_APPID=%DEFAULT_APPID%"
    ) else (
        echo.
        echo No se introdujo AppID. Saliendo.
        pause
        exit /b 1
    )
)

echo %INPUT_APPID%>steam_appid.txt

echo.
echo Lanzando SimpleSteamIdler para el juego con AppID %INPUT_APPID%...
echo.

SimpleSteamIdler.exe %INPUT_APPID%

pause
endlocal
