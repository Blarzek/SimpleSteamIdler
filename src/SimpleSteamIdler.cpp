// SimpleSteamIdler.cpp
// Builds with: cl /O2 /EHsc SimpleSteamIdler.cpp winhttp.lib
// (Use x64 Native Tools if you want an x64 exe and steam_api64.dll)
//
// Purpose:
// - Ask the user for a Steam AppID, validate it, confirm it exists in the Steam Store
//   and that the local Steam client can initialize that AppID via steam_api*.dll.
// - If any step fails (invalid format, not found on store, Steam init fails) the program
//   prompts the user again (or allows exit).
// - Displays game name using proper UTF-8 -> UTF-16 conversion so CMD shows characters
//   like ™ correctly.
// - Suppresses steam_api.dll internal messages while calling SteamAPI_Init().
//
// Notes on style / safety:
// - Avoids `while(true)` by using boolean loop conditions.
// - Adds explanatory comments and defensive checks.
// - Keeps functions small and testable-ish to follow static-analysis friendly layout.

#define NOMINMAX

#include "../resources/resource.h"

#include <windows.h>
#include <winhttp.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <fstream>
#include <functional>
#include <io.h>
#include <iostream>
#include <limits>
#include <string>
#include <thread>
#include <vector>

#pragma comment(lib, "winhttp.lib")
#pragma comment(lib, "user32.lib")

using std::string;

// --------------------------- Utility helpers ---------------------------

// Redirect stdout/stderr to NUL while executing a callable.
// This is used to suppress messages printed by steam_api.dll during initialization.
static void suppress_console_output(const std::function<void()>& fn)
{
    // Duplicate file descriptors for stdout/stderr
    int stdout_backup = _dup(_fileno(stdout));
    int stderr_backup = _dup(_fileno(stderr));

    // Open NUL and redirect stdout/stderr there
    FILE* nul = nullptr;
    freopen_s(&nul, "NUL", "w", stdout);
    freopen_s(&nul, "NUL", "w", stderr);

    // Execute the provided callable while output is suppressed
    fn();

    // Restore original stdout/stderr
    _dup2(stdout_backup, _fileno(stdout));
    _dup2(stderr_backup, _fileno(stderr));
    _close(stdout_backup);
    _close(stderr_backup);
}

// Convert UTF-8 string to wstring (UTF-16) using Win32 API.
// Returns empty wstring on failure.
static std::wstring utf8_to_wstring(const std::string& utf8)
{
    if (utf8.empty()) {
        return std::wstring();
    }
    int needed = MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), NULL, 0);
    if (needed <= 0) {
        return std::wstring();
    }
    std::vector<wchar_t> buf(static_cast<size_t>(needed) + 1);
    int converted = MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), buf.data(), needed);
    if (converted == 0) {
        return std::wstring();
    }
    return std::wstring(buf.data(), converted);
}

// Print a UTF-8 string followed by newline to console robustly.
// Tries WriteConsoleW (preferred). Falls back to std::cout (UTF-8 bytes).
static void print_utf8_line(const std::string& utf8)
{
    std::wstring w = utf8_to_wstring(utf8);
    HANDLE hOut = GetStdHandle(STD_OUTPUT_HANDLE);
    if (hOut != INVALID_HANDLE_VALUE && hOut != NULL) {
        DWORD written = 0;
        if (WriteConsoleW(hOut, w.c_str(), static_cast<DWORD>(w.size()), &written, NULL)) {
            const wchar_t nl = L'\n';
            WriteConsoleW(hOut, &nl, 1, &written, NULL);
            return;
        }
    }
    // Fallback: print raw UTF-8 bytes (works if CP_UTF8 is set or output is redirected)
    std::cout << utf8 << '\n';
}

// Print without newline (for prompts). Uses same robust approach as print_utf8_line.
static void print_utf8(const std::string& utf8)
{
    std::wstring w = utf8_to_wstring(utf8);
    HANDLE hOut = GetStdHandle(STD_OUTPUT_HANDLE);
    if (hOut != INVALID_HANDLE_VALUE && hOut != NULL) {
        DWORD written = 0;
        if (WriteConsoleW(hOut, w.c_str(), static_cast<DWORD>(w.size()), &written, NULL)) {
            return;
        }
    }
    std::cout << utf8;
}

// Trim whitespace (space, tab, CR, LF) from both ends.
static string trim(const string& s)
{
    size_t a = s.find_first_not_of(" \t\r\n");
    if (a == string::npos) {
        return "";
    }
    size_t b = s.find_last_not_of(" \t\r\n");
    return s.substr(a, b - a + 1);
}

// Check whether the provided string contains only digits (0-9).
static bool is_digits_only(const string& s)
{
    if (s.empty()) return false;
    return std::all_of(s.begin(), s.end(), [](unsigned char c) { return std::isdigit(c); });
}

// Print a wide string (UTF-16) followed by newline.
// This avoids encoding issues for literals containing non-ASCII characters.
static void print_wline(const std::wstring& w)
{
    HANDLE hOut = GetStdHandle(STD_OUTPUT_HANDLE);
    if (hOut != INVALID_HANDLE_VALUE && hOut != NULL) {
        DWORD written = 0;
        if (WriteConsoleW(hOut, w.c_str(), static_cast<DWORD>(w.size()), &written, NULL)) {
            const wchar_t nl = L'\n';
            WriteConsoleW(hOut, &nl, 1, &written, NULL);
            return;
        }
    }

    // Fallback: convert to UTF-8 and print
    int needed = WideCharToMultiByte(CP_UTF8, 0, w.c_str(),
        static_cast<int>(w.size()),
        NULL, 0, NULL, NULL);

    if (needed > 0) {
        std::string buf(static_cast<size_t>(needed), 0);
        WideCharToMultiByte(CP_UTF8, 0, w.c_str(),
            static_cast<int>(w.size()),
            &buf[0], needed, NULL, NULL);
        std::cout << buf << '\n';
    }
    else {
        std::wcout << w << std::endl;
    }
}

// Cleans Steam variables
static void clear_steam_env() {
    SetEnvironmentVariableA("SteamAppId", NULL);
    SetEnvironmentVariableA("SteamGameId", NULL);
}

// --------------------------- HTTP / Store helpers ---------------------------

// Perform a GET request to store.steampowered.com/api/appdetails?appids=<appid>
// Returns true if fetch succeeded and writes response bytes (UTF-8) into outResp.
static bool http_get_appdetails(const string& appid, string& outResp)
{
    outResp.clear();

    // Build wide strings for WinHTTP functions
    std::wstring host = L"store.steampowered.com";
    std::wstring path = L"/api/appdetails?appids=" + std::wstring(appid.begin(), appid.end());

    HINTERNET hSession = WinHttpOpen(
        L"SimpleSteamIdler/1.0",
        WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
        WINHTTP_NO_PROXY_NAME,
        WINHTTP_NO_PROXY_BYPASS, 0);

    if (!hSession) {
        return false;
    }

    HINTERNET hConnect = WinHttpConnect(hSession, host.c_str(), INTERNET_DEFAULT_HTTPS_PORT, 0);
    if (!hConnect) {
        WinHttpCloseHandle(hSession);
        return false;
    }

    HINTERNET hRequest = WinHttpOpenRequest(
        hConnect,
        L"GET",
        path.c_str(),
        NULL,
        WINHTTP_NO_REFERER,
        WINHTTP_DEFAULT_ACCEPT_TYPES,
        WINHTTP_FLAG_SECURE);

    if (!hRequest) {
        WinHttpCloseHandle(hConnect);
        WinHttpCloseHandle(hSession);
        return false;
    }

    BOOL sent = WinHttpSendRequest(
        hRequest,
        WINHTTP_NO_ADDITIONAL_HEADERS, 0,
        WINHTTP_NO_REQUEST_DATA, 0,
        0, 0);

    if (!sent) {
        WinHttpCloseHandle(hRequest);
        WinHttpCloseHandle(hConnect);
        WinHttpCloseHandle(hSession);
        return false;
    }

    if (!WinHttpReceiveResponse(hRequest, NULL)) {
        WinHttpCloseHandle(hRequest);
        WinHttpCloseHandle(hConnect);
        WinHttpCloseHandle(hSession);
        return false;
    }

    // Read response in chunks
    DWORD dwSize = 0;
    do {
        DWORD dwDownloaded = 0;
        if (!WinHttpQueryDataAvailable(hRequest, &dwSize)) {
            break;
        }
        if (dwSize == 0) {
            break;
        }

        std::string buffer;
        buffer.resize(dwSize);

        if (!WinHttpReadData(hRequest, &buffer[0], dwSize, &dwDownloaded)) {
            break;
        }
        outResp.append(buffer.data(), dwDownloaded);
    } while (dwSize > 0);

    WinHttpCloseHandle(hRequest);
    WinHttpCloseHandle(hConnect);
    WinHttpCloseHandle(hSession);

    return !outResp.empty();
}

// Quick JSON sniff: return true if the appdetails response contains "success": true for given appid.
static bool resp_indicates_success(const string& resp, const string& appid)
{
    if (resp.empty() || appid.empty()) return false;
    string key = "\"" + appid + "\"";
    size_t pos = resp.find(key);
    if (pos == string::npos) return false;

    size_t successPos = resp.find("\"success\"", pos);
    if (successPos == string::npos) return false;

    size_t colonPos = resp.find(':', successPos);
    if (colonPos == string::npos) return false;

    size_t checkEnd = (resp.size() < colonPos + 50) ? resp.size() : (colonPos + 50);
    string snippet = resp.substr(colonPos, checkEnd - colonPos);
    return (snippet.find("true") != string::npos);
}

// Extract the game name from the appdetails JSON response (basic, not full JSON parser).
// Returns empty string on failure.
static string extract_game_name(const string& resp, const string& appid)
{
    if (resp.empty() || appid.empty()) return "";

    string key = "\"" + appid + "\"";
    size_t pos = resp.find(key);
    if (pos == string::npos) return "";

    size_t successPos = resp.find("\"success\"", pos);
    if (successPos == string::npos) return "";

    size_t colonPos = resp.find(':', successPos);
    if (colonPos == string::npos) return "";

    size_t checkEnd = (resp.size() < colonPos + 200) ? resp.size() : (colonPos + 200);
    string snippet = resp.substr(colonPos, checkEnd - colonPos);
    if (snippet.find("true") == string::npos) return "";

    size_t dataPos = resp.find("\"data\"", successPos);
    if (dataPos == string::npos) return "";

    size_t namePos = resp.find("\"name\"", dataPos);
    if (namePos == string::npos) return "";

    size_t colonAfterName = resp.find(':', namePos);
    if (colonAfterName == string::npos) return "";

    size_t startQuote = resp.find('"', colonAfterName + 1);
    if (startQuote == string::npos) return "";

    // Extract the quoted string (handle basic escapes)
    size_t i = startQuote + 1;
    std::string name;
    for (; i < resp.size(); ++i) {
        char c = resp[i];
        if (c == '"' && resp[i - 1] != '\\') {
            break;
        }
        if (c == '\\' && i + 1 < resp.size()) {
            char next = resp[i + 1];
            if (next == '"' || next == '\\' || next == '/') {
                name.push_back(next);
                ++i;
                continue;
            }
            else if (next == 'n') {
                name.push_back('\n');
                ++i;
                continue;
            }
            else if (next == 't') {
                name.push_back('\t');
                ++i;
                continue;
            }
            // other escapes: skip the backslash and take the next char
        }
        else {
            name.push_back(c);
        }
    }

    return name;
}

// --------------------------- File helpers ---------------------------

// Read the single-line steam_appid.txt file if present, trim and return contents.
// Returns empty string if file not present or empty.
static string read_appid_from_file(const char* filename = "steam_appid.txt")
{
    std::ifstream ifs(filename);
    if (!ifs) return "";
    string tmp;
    std::getline(ifs, tmp);
    return trim(tmp);
}

// Save AppID to steam_appid.txt (overwrite).
static void save_appid_to_file(const string& appid, const char* filename = "steam_appid.txt")
{
    std::ofstream ofs(filename, std::ios::trunc);
    if (ofs) {
        ofs << appid << '\n';
    }
}

// --------------------------- Main program flow ---------------------------

int main(int argc, char* argv[])
{
    // Welcome message
    print_utf8_line("===============================================");
    print_utf8_line("=   Welcome to SimpleSteamIdler, by Blarzek   =");
    print_utf8_line("===============================================");

    // Empty line below for spacing
    print_utf8_line("");

    // Ensure console uses UTF-8 code page so printing UTF-8 bytes works when fallback is used.
    // WriteConsoleW will be used for interactive console output but setting CP_UTF8 helps
    // in some fallback/redirection scenarios.
    SetConsoleOutputCP(CP_UTF8);
    SetConsoleCP(CP_UTF8);

    // Title and icon
    SetConsoleTitleW(L"SimpleSteamIdler");

    HICON hIcon = LoadIcon(GetModuleHandle(NULL), MAKEINTRESOURCE(IDI_APP_ICON));
    if (hIcon) {
        HWND hwndConsole = GetConsoleWindow();
        if (hwndConsole) {
            SendMessage(hwndConsole, WM_SETICON, ICON_BIG, (LPARAM)hIcon);
            SendMessage(hwndConsole, WM_SETICON, ICON_SMALL, (LPARAM)hIcon);
        }
    }

    // Candidate appid: priority argv[1] > steam_appid.txt > user input
    string candidate_appid;

    if (argc > 1 && argv[1] && argv[1][0] != '\0') {
        candidate_appid = trim(string(argv[1]));
    }
    else {
        candidate_appid = read_appid_from_file();
    }

    // Loop condition flag: we attempt to obtain a valid AppID and ensure Steam init works
    bool have_valid_setup = false;

    // We'll repeat prompting while we don't have a valid setup.
    // Use a max iteration guard to be defensive (although user can exit with Q).
    const int MAX_ATTEMPTS = 1000; // practically unlimited but avoids accidental infinite loops
    int attempts = 0;

    while (!have_valid_setup && attempts < MAX_ATTEMPTS) {
        ++attempts;

        // ---- Step 1: Acquire AppID from user if candidate is empty ----
        if (candidate_appid.empty()) {
            print_utf8("Enter Steam AppID (or Q to quit): ");
            std::string line;
            std::getline(std::cin, line);
            candidate_appid = trim(line);
            if (candidate_appid.size() == 1 &&
                (candidate_appid[0] == 'Q' || candidate_appid[0] == 'q')) {
                print_utf8_line("Exiting.");
                return 0;
            }
        }

        // ---- Step 2: Validate numeric format ----
        if (!is_digits_only(candidate_appid)) {
            print_utf8_line("Error: AppID must contain digits only.");
            candidate_appid.clear();
            continue; // prompt again
        }

        // ---- Step 3: Query Steam Store to check existence ----
        print_utf8_line("Checking Steam Store for AppID...");
        string store_response;
        bool fetched = http_get_appdetails(candidate_appid, store_response);

        if (!fetched) {
            print_utf8_line("Warning: Could not contact Steam Store (network issue?).");
            print_utf8("Retry? (Y to retry, N to continue without Store check, Q to quit): ");
            std::string choice;
            std::getline(std::cin, choice);
            if (!choice.empty() && (choice[0] == 'Q' || choice[0] == 'q')) {
                print_utf8_line("Exiting.");
                return 0;
            }
            if (!choice.empty() && (choice[0] == 'Y' || choice[0] == 'y')) {
                candidate_appid.clear();
                continue; // re-prompt or re-check
            }
            // If N or anything else, proceed without store validation (Steam API will still fail if invalid)
        }
        else {
            // If fetched, check success flag in the JSON
            bool exists = resp_indicates_success(store_response, candidate_appid);
            if (!exists) {
                print_utf8_line("AppID not found or store reports no data for this AppID.");
                candidate_appid.clear();
                continue; // ask again
            }
        }

        // Extract name if present (optional, helpful UX)
        string gamename = extract_game_name(store_response, candidate_appid);

        // ---- Step 4: Try to load steam_api DLL and initialize Steam API ----

        // Ensure Steam uses the correct AppID for this attempt.
        // Steam reads AppID from:
        //   - steam_appid.txt
        //   - SteamAppId environment variable
        // We explicitly set both to avoid stale state from previous attempts.

        save_appid_to_file(candidate_appid);

        // Clear any previous environment variable and set the new one
        SetEnvironmentVariableA("SteamAppId", candidate_appid.c_str());
        SetEnvironmentVariableA("SteamGameId", candidate_appid.c_str());

        // Load the steam_api DLL (prefer 64-bit name first)
        HMODULE hSteam = LoadLibraryA("steam_api64.dll");
        if (!hSteam) {
            hSteam = LoadLibraryA("steam_api.dll");
        }

        if (!hSteam) {
            print_utf8_line("Error: Could not find steam_api64.dll or steam_api.dll in the current folder.");
            print_utf8("Place the appropriate DLL and press ENTER to retry, or Q to quit: ");
            std::string resp_line;
            std::getline(std::cin, resp_line);
            if (!resp_line.empty() && (resp_line[0] == 'Q' || resp_line[0] == 'q')) {
                return 0;
            }
            // Try again (user may have placed DLL)
            candidate_appid.clear();
            if (hSteam) {
                clear_steam_env();
                FreeLibrary(hSteam);
            }
            continue;
        }

        // Get function pointers dynamically (we don't link to Steam SDK)
        typedef bool(__cdecl* SteamAPI_Init_t)();
        typedef void(__cdecl* SteamAPI_Shutdown_t)();
        typedef void(__cdecl* SteamAPI_RunCallbacks_t)();
        typedef bool(__cdecl* SteamAPI_IsSteamRunning_t)();
        typedef void* (__cdecl* SteamAPI_SteamUser_t)();
        typedef bool(__cdecl* SteamAPI_ISteamUser_BLoggedOn_t)(void*);

        SteamAPI_Init_t SteamAPI_Init = reinterpret_cast<SteamAPI_Init_t>(GetProcAddress(hSteam, "SteamAPI_Init"));
        SteamAPI_Shutdown_t SteamAPI_Shutdown = reinterpret_cast<SteamAPI_Shutdown_t>(GetProcAddress(hSteam, "SteamAPI_Shutdown"));
        SteamAPI_RunCallbacks_t SteamAPI_RunCallbacks = reinterpret_cast<SteamAPI_RunCallbacks_t>(GetProcAddress(hSteam, "SteamAPI_RunCallbacks"));

        if (!SteamAPI_Init) {
            print_utf8_line("Error: steam_api DLL loaded but SteamAPI_Init not found (incompatible DLL?).");
            clear_steam_env();
            FreeLibrary(hSteam);
            candidate_appid.clear();
            continue;
        }

        // Call SteamAPI_Init while suppressing any noisy internal output
        bool init_ok = false;
        suppress_console_output([&]() {
            init_ok = SteamAPI_Init();
            });

        if (!init_ok) {
            bool steam_running = false;
            bool logged_on = false;

            auto SteamAPI_IsSteamRunning =
                reinterpret_cast<SteamAPI_IsSteamRunning_t>(
                    GetProcAddress(hSteam, "SteamAPI_IsSteamRunning"));

            auto SteamAPI_SteamUser =
                reinterpret_cast<SteamAPI_SteamUser_t>(
                    GetProcAddress(hSteam, "SteamAPI_SteamUser"));

            auto SteamAPI_ISteamUser_BLoggedOn =
                reinterpret_cast<SteamAPI_ISteamUser_BLoggedOn_t>(
                    GetProcAddress(hSteam, "SteamAPI_ISteamUser_BLoggedOn"));

            if (SteamAPI_IsSteamRunning) {
                steam_running = SteamAPI_IsSteamRunning();
            }

            if (steam_running && SteamAPI_SteamUser && SteamAPI_ISteamUser_BLoggedOn) {
                void* user = SteamAPI_SteamUser();
                if (user) {
                    logged_on = SteamAPI_ISteamUser_BLoggedOn(user);
                }
            }

            if (!steam_running) {
                print_wline(L"Steam client is not running with a valid user session.");
                print_wline(L"Please start Steam and log in before trying again.");
            }
            else {
                print_wline(L"The AppID appears valid but the game is not owned by the logged-in account.");

                std::string out = "Cannot execute game \"" + gamename + "\" (AppID " + candidate_appid + ") - Not owned by this Steam account.";
                print_utf8_line(out);
            }

            print_utf8("Enter a different AppID to try again, or Q to quit: ");

            std::string line;
            std::getline(std::cin, line);
            line = trim(line);

            if (!line.empty() && (line[0] == 'Q' || line[0] == 'q')) {
                clear_steam_env();
                FreeLibrary(hSteam);
                print_utf8_line("Exiting.");
                return 0;
            }

            candidate_appid = line;
            clear_steam_env();
            FreeLibrary(hSteam);
            continue;
        }

        // If we are here, SteamAPI_Init succeeded.
        // Save the AppID persistently, show friendly message (with UTF handling).
        save_appid_to_file(candidate_appid);

        if (!gamename.empty()) {
            std::string out = "Executing game \"" + gamename + "\" (AppID " + candidate_appid + ")...";
            print_utf8_line(out);
        }
        else {
            std::string out = "Executing AppID " + candidate_appid + " (name not found)...";
            print_utf8_line(out);
        }

        // Start callback thread and wait for user to press ENTER to stop.
        std::atomic<bool> running{ true };
        std::thread callback_thread([&]() {
            while (running.load()) {
                if (SteamAPI_RunCallbacks) {
                    SteamAPI_RunCallbacks();
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(1000));
            }
            });

        print_utf8_line("Press ENTER to stop the simulation and exit.");
        // Wait for user input (this will pause the main thread)
        std::string dummy;
        std::getline(std::cin, dummy);

        // Stop callback thread and cleanup Steam API
        running.store(false);
        if (callback_thread.joinable()) {
            callback_thread.join();
        }

        if (SteamAPI_Shutdown) {
            SteamAPI_Shutdown();
        }

        clear_steam_env();
        FreeLibrary(hSteam);

        print_utf8_line("Simulation stopped. Exiting.");
        have_valid_setup = true;
    } // end main attempts loop

    if (!have_valid_setup) {
        print_utf8_line("Aborting: too many attempts or unrecoverable error.");
        return 2;
    }

    return 0;
}
