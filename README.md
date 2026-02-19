# SimpleSteamIdler

Simulates a Steam game running to unlock trading cards without launching the game.

---

## Features

- Simulate a Steam game using its AppID.
- Works even if the game is not installed.
- Fetches game name from Steam Store API.
- Saves the last AppID for convenience.
- Minimal console output; suppresses Steam internal messages.

---

## Requirements

- Windows 10/11 (x64 recommended).
- Visual Studio 2026 (or MSVC compiler) for building.
- `steam_api.dll` or `steam_api64.dll` in the same folder as the executable.
- Steam client running and logged in.
- Ownership of the game you want to simulate.
- Internet connection (optional, for fetching game names).

---

## Content

```
SimpleSteamIdler/
├─ resources/
│   ├─ resource.h
│   ├─ resources.rc
│   └─ SimpleSteamIdler.ico
├─ src/
│   ├─ SimpleSteamIdler.cpp
│   ├─ SimpleSteamIdler.sln
│   ├─ SimpleSteamIdler.vcxproj
│   └─ SimpleSteamIdler.vcxproj.filters
├─ compile.bat
└─ (Output executable)
```

---

## Compilation

Simply run `compile.bat`

Or:

1. Open **x64 Native Tools Command Prompt for VS**.

2. Move to the directory with the code, for example `C:/SimpleSteamIdler`.

```bat
cd C:/SimpleSteamIdler
```

3. Run the following commands:

```bat
rc.exe /fo resources\resources.res resources\resources.rc
cl.exe /EHsc /Iresources /c src\SimpleSteamIdler.cpp
link SimpleSteamIdler.obj resources\resources.res /OUT:SimpleSteamIdler.exe
```

4. Optionally, delete temporary files:

```bat
del SimpleSteamIdler.obj
del resources\resources.res
```

This will generate `SimpleSteamIdler.exe` in the main directory.

---

## Run

1. Open Steam.
2. Simply run the file `run.bat` and follow the instructions.

Or manually modify the file `steam_appid.txt` with the desired game AppID and execute `SimpleSteamIdler.exe`.

The AppID can be provided:

- As a command-line argument
- In the file `steam_appid.txt` in the same location
- Or entered when prompted

Press ENTER to stop the program.

---

## Notes

- Only works with games you own
- The game is not actually launched
- Steam must be running
