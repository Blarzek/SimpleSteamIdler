// SimpleSteamIdler.cpp
// Compilar: cl /O2 /EHsc SimpleSteamIdler.cpp winhttp.lib
// (usar x64 Native Tools si quieres exe 64-bit y steam_api64.dll)

#define NOMINMAX

#include <windows.h>
#include <winhttp.h>
#include <iostream>
#include <fstream>
#include <string>
#include <thread>
#include <atomic>
#include <chrono>
#include <functional>
#include <cstdio>
#include <io.h>
#include <fcntl.h>
#include <vector>

#pragma comment(lib, "winhttp.lib")

using std::string;

// --- Helper: redirigir stdout/stderr temporalmente ---
void suppress_console_output(std::function<void()> f) {
    int stdout_backup = _dup(_fileno(stdout));
    int stderr_backup = _dup(_fileno(stderr));

    FILE* nul;
    freopen_s(&nul, "NUL", "w", stdout);
    freopen_s(&nul, "NUL", "w", stderr);

    f();

    _dup2(stdout_backup, _fileno(stdout));
    _dup2(stderr_backup, _fileno(stderr));
    close(stdout_backup);
    close(stderr_backup);
}

// --- Helper: solicitud HTTP GET usando WinHTTP (host: store.steampowered.com) ---
static bool http_get_appdetails(const string& appid, string& outResp) {
    outResp.clear();
    std::wstring host = L"store.steampowered.com";
    std::wstring path = L"/api/appdetails?appids=" + std::wstring(appid.begin(), appid.end());

    HINTERNET hSession = WinHttpOpen(L"SimpleSteamIdler/1.0",
        WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
        WINHTTP_NO_PROXY_NAME,
        WINHTTP_NO_PROXY_BYPASS, 0);
    if (!hSession) return false;

    HINTERNET hConnect = WinHttpConnect(hSession, host.c_str(), INTERNET_DEFAULT_HTTPS_PORT, 0);
    if (!hConnect) { WinHttpCloseHandle(hSession); return false; }

    HINTERNET hRequest = WinHttpOpenRequest(hConnect, L"GET", path.c_str(),
        NULL, WINHTTP_NO_REFERER,
        WINHTTP_DEFAULT_ACCEPT_TYPES,
        WINHTTP_FLAG_SECURE);
    if (!hRequest) { WinHttpCloseHandle(hConnect); WinHttpCloseHandle(hSession); return false; }

    BOOL bResults = WinHttpSendRequest(hRequest,
        WINHTTP_NO_ADDITIONAL_HEADERS, 0,
        WINHTTP_NO_REQUEST_DATA, 0,
        0, 0);
    if (!bResults) { WinHttpCloseHandle(hRequest); WinHttpCloseHandle(hConnect); WinHttpCloseHandle(hSession); return false; }

    if (!WinHttpReceiveResponse(hRequest, NULL)) {
        WinHttpCloseHandle(hRequest); WinHttpCloseHandle(hConnect); WinHttpCloseHandle(hSession);
        return false;
    }

    // Leer respuesta
    DWORD dwSize = 0;
    do {
        DWORD dwDownloaded = 0;
        if (!WinHttpQueryDataAvailable(hRequest, &dwSize)) break;
        if (dwSize == 0) break;

        std::string buffer;
        buffer.resize(dwSize);
        if (!WinHttpReadData(hRequest, &buffer[0], dwSize, &dwDownloaded)) break;
        outResp.append(buffer.data(), dwDownloaded);
    } while (dwSize > 0);

    WinHttpCloseHandle(hRequest);
    WinHttpCloseHandle(hConnect);
    WinHttpCloseHandle(hSession);
    return !outResp.empty();
}

// --- Helper: extraer el campo data.name del JSON (búsqueda simple) ---
static string extract_game_name(const string& resp, const string& appid) {
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

    size_t i = startQuote + 1;
    std::string name;
    for (; i < resp.size(); ++i) {
        char c = resp[i];
        if (c == '"' && resp[i - 1] != '\\') break;
        if (c == '\\' && i + 1 < resp.size()) {
            char next = resp[i + 1];
            if (next == '"' || next == '\\' || next == '/') { name.push_back(next); ++i; continue; }
            else if (next == 'n') { name.push_back('\n'); ++i; continue; }
            else if (next == 't') { name.push_back('\t'); ++i; continue; }
        }
        else name.push_back(c);
    }

    return name;
}

// --- Helper: convert UTF-8 std::string -> std::wstring (UTF-16) ---
static std::wstring utf8_to_wstring(const std::string& utf8) {
    if (utf8.empty()) return std::wstring();
    int needed = MultiByteToWideChar(CP_UTF8, 0, utf8.data(), (int)utf8.size(), NULL, 0);
    if (needed <= 0) return std::wstring();
    std::vector<wchar_t> buf(needed + 1);
    int res = MultiByteToWideChar(CP_UTF8, 0, utf8.data(), (int)utf8.size(), buf.data(), needed);
    if (res == 0) return std::wstring();
    return std::wstring(buf.data(), res);
}

// --- Helper: print UTF-8 string to console robustly ---
static void print_utf8_line(const std::string& utf8) {
    // Convert to wide
    std::wstring w = utf8_to_wstring(utf8);
    HANDLE hOut = GetStdHandle(STD_OUTPUT_HANDLE);
    if (hOut != INVALID_HANDLE_VALUE && hOut != NULL) {
        DWORD written = 0;
        // Try WriteConsoleW (works when attached to interactive console)
        if (WriteConsoleW(hOut, w.c_str(), (DWORD)w.size(), &written, NULL)) {
            // write a newline as wide
            const wchar_t nl = L'\n';
            WriteConsoleW(hOut, &nl, 1, &written, NULL);
            return;
        }
    }
    // Fallback: print raw utf-8 bytes (may work if console CP set to UTF-8)
    std::cout << utf8 << std::endl;
}

int main(int argc, char* argv[]) {
    // Set console to UTF-8 output/input so WriteConsoleW and UTF-8 fallback work better.
    SetConsoleOutputCP(CP_UTF8);
    SetConsoleCP(CP_UTF8);

    std::string appid;

    // 1) Obtener AppID
    if (argc > 1 && argv[1] && argv[1][0] != '\0') appid = argv[1];
    else {
        std::ifstream ifs("steam_appid.txt");
        if (ifs) {
            std::string tmp; std::getline(ifs, tmp);
            size_t first = tmp.find_first_not_of(" \t\r\n");
            size_t last = tmp.find_last_not_of(" \t\r\n");
            if (first != std::string::npos && last != std::string::npos && last >= first)
                appid = tmp.substr(first, last - first + 1);
        }
        if (appid.empty()) {
            std::cout << "Introduce AppID de Steam: ";
            std::getline(std::cin, appid);
            if (appid.empty()) { std::cerr << "No se proporciono AppID. Saliendo.\n"; return 1; }
        }
    }

    // Guardar AppID persistente
    std::ofstream ofs("steam_appid.txt", std::ios::trunc);
    if (ofs) ofs << appid << std::endl;

    // 2) Obtener nombre desde la Store API
    std::string resp, gamename;
    if (http_get_appdetails(appid, resp)) gamename = extract_game_name(resp, appid);

    if (!gamename.empty()) {
        std::string msg = "Ejecutando el juego \"" + gamename + "\" (AppID " + appid + ")...";
        print_utf8_line(msg);
    }
    else {
        std::string msg = "Ejecutando AppID " + appid + " (nombre no encontrado o sin conexion)...";
        print_utf8_line(msg);
    }

    // 3) Cargar steam_api DLL y silenciar mensajes
    HMODULE h = LoadLibraryA("steam_api64.dll");
    if (!h) h = LoadLibraryA("steam_api.dll");
    if (!h) { print_utf8_line("No se pudo cargar steam_api64.dll ni steam_api.dll."); return 2; }

    typedef bool(__cdecl* SteamAPI_Init_t)();
    typedef void(__cdecl* SteamAPI_Shutdown_t)();
    typedef void(__cdecl* SteamAPI_RunCallbacks_t)();

    auto SteamAPI_Init = (SteamAPI_Init_t)GetProcAddress(h, "SteamAPI_Init");
    auto SteamAPI_Shutdown = (SteamAPI_Shutdown_t)GetProcAddress(h, "SteamAPI_Shutdown");
    auto SteamAPI_RunCallbacks = (SteamAPI_RunCallbacks_t)GetProcAddress(h, "SteamAPI_RunCallbacks");

    if (!SteamAPI_Init) { print_utf8_line("No se encontro SteamAPI_Init en la DLL."); FreeLibrary(h); return 3; }

    bool initSuccess = false;
    suppress_console_output([&]() {
        initSuccess = SteamAPI_Init();
        });

    if (!initSuccess) { print_utf8_line("SteamAPI_Init() fallo. ¿Esta el cliente de Steam ejecutandose y con la sesion iniciada?"); FreeLibrary(h); return 4; }

    // 4) Loop de simulacion
    std::atomic<bool> running(true);
    std::thread worker([&]() {
        while (running.load()) {
            if (SteamAPI_RunCallbacks) SteamAPI_RunCallbacks();
            std::this_thread::sleep_for(std::chrono::milliseconds(1000));
        }
        });

    print_utf8_line("Presiona ENTER para detener SimpleSteamIdler...");
    std::string dummy;
    std::getline(std::cin, dummy);

    running.store(false);
    if (worker.joinable()) worker.join();

    if (SteamAPI_Shutdown) SteamAPI_Shutdown();
    FreeLibrary(h);

    print_utf8_line("Se ha cerrado SimpleSteamIdler.");
    return 0;
}
