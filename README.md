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
- Visual Studio 2022 (or MSVC compiler) for building.
- `steam_api.dll` or `steam_api64.dll` in the same folder as the executable.
- Steam client running and logged in.
- Ownership of the game you want to simulate.
- Internet connection (optional, for fetching game names).

---

## Compilation

Open **x64 Native Tools Command Prompt for VS** and run:

```bat
cl /O2 /EHsc SimpleSteamIdler.cpp winhttp.lib
```

This generates `SimpleSteamIdler.exe`.

---

## Run

1. Open Steam  
2. Simply run the file `run.bat` and follow the instructions

Or manually execute:

```bat
SimpleSteamIdler.exe
```

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
