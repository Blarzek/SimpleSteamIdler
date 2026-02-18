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
#include <cctype>
#include <algorithm>    // <-- necesario para std::all_of

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

// --- Helper: print UTF-8 string to console without newline (tries WriteConsoleW, fallback) ---
static void print_utf8(const std::string& utf8) {
    std::wstring w = utf8_to_wstring(utf8);
    HANDLE hOut = GetStdHandle(STD_OUTPUT_HANDLE);
    if (hOut != INVALID_HANDLE_VALUE && hOut != NULL) {
        DWORD written = 0;
        if (WriteConsoleW(hOut, w.c_str(), (DWORD)w.size(), &written, NULL)) {
            return;
        }
    }
    // fallback
    std::cout << utf8;
}

// --- Helper: print UTF-8 string with newline ---
static void print_utf8_line(const std::string& utf8) {
    std::wstring w = utf8_to_wstring(utf8);
    HANDLE hOut = GetStdHandle(STD_OUTPUT_HANDLE);
    if (hOut != INVALID_HANDLE_VALUE && hOut != NULL) {
        DWORD written = 0;
        if (WriteConsoleW(hOut, w.c_str(), (DWORD)w.size(), &written, NULL)) {
            const wchar_t nl = L'\n';
            WriteConsoleW(hOut, &nl, 1, &written, NULL);
            return;
        }
    }
    // fallback
    std::cout << utf8 << std::endl;
}

// --- NEW: Helper to print a wide string line reliably (use this when you want to embed non-ascii safely) ---
static void print_wline(const std::wstring& w) {
    HANDLE hOut = GetStdHandle(STD_OUTPUT_HANDLE);
    if (hOut != INVALID_HANDLE_VALUE && hOut != NULL) {
        DWORD written = 0;
        if (WriteConsoleW(hOut, w.c_str(), (DWORD)w.size(), &written, NULL)) {
            const wchar_t nl = L'\n';
            WriteConsoleW(hOut, &nl, 1, &written, NULL);
            return;
        }
    }
    // fallback: convert to UTF-8 and print
    int needed = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), NULL, 0, NULL, NULL);
    if (needed > 0) {
        std::string buf(needed, 0);
        WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), &buf[0], needed, NULL, NULL);
        std::cout << buf << std::endl;
    }
    else {
        // last resort
        std::wcout << w << std::endl;
    }
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

// --- Helper: comprobar si el JSON de appdetails indica success:true para el appid ---
static bool resp_indicates_success(const string& resp, const string& appid) {
    if (resp.empty() || appid.empty()) return false;
    string key = "\"" + appid + "\"";
    size_t pos = resp.find(key);
    if (pos == string::npos) return false;
    size_t successPos = resp.find("\"success\"", pos);
    if (successPos == string::npos) return false;
    size_t colonPos = resp.find(':', successPos);
    if (colonPos == string::npos) return false;
    // look ahead a bit for "true"
    size_t checkEnd = (resp.size() < colonPos + 50) ? resp.size() : (colonPos + 50);
    string snippet = resp.substr(colonPos, checkEnd - colonPos);
    return (snippet.find("true") != string::npos);
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

// --- util: trim ---
static string trim(const string& s) {
    size_t a = s.find_first_not_of(" \t\r\n");
    if (a == string::npos) return "";
    size_t b = s.find_last_not_of(" \t\r\n");
    return s.substr(a, b - a + 1);
}

int main(int argc, char* argv[]) {
    // Mejorar compatibilidad Unicode en consola
    SetConsoleOutputCP(CP_UTF8);
    SetConsoleCP(CP_UTF8);

    std::string appid;

    // 1) Obtener AppID (argv[1] > steam_appid.txt > pedir por stdin)
    if (argc > 1 && argv[1] && argv[1][0] != '\0') {
        appid = trim(argv[1]);
    }
    else {
        // intentar leer steam_appid.txt
        std::ifstream ifs("steam_appid.txt");
        if (ifs) {
            std::string tmp;
            std::getline(ifs, tmp);
            appid = trim(tmp);
        }

        // solicitar en bucle hasta que el usuario introduzca algo válido o decida salir
        while (appid.empty()) {
            print_utf8("Introduce el AppID del juego de Steam (ENTER para usar el guardado / Q para salir): ");
            std::string line;
            std::getline(std::cin, line);
            line = trim(line);
            if (line.empty()) {
                print_utf8_line("No hay AppID guardado. Por favor introduce uno o pulsa Q para salir.");
                continue;
            }
            if (line.size() == 1 && (line[0] == 'Q' || line[0] == 'q')) {
                print_utf8_line("Saliendo.");
                return 0;
            }
            appid = line;
        }
    }

    // Si vino por argv, o quedó tras lectura, permitir validación/reintentos:
    while (true) {
        // trim
        appid = trim(appid);
        // validar que sea numérico
        bool is_digits = !appid.empty() && std::all_of(appid.begin(), appid.end(), [](unsigned char c) { return std::isdigit(c); });
        if (!is_digits) {
            print_utf8_line("El AppID debe ser un numero (solo digitos).");
            print_utf8("Introduce un AppID valido (Q para salir): ");
            std::string line;
            std::getline(std::cin, line);
            line = trim(line);
            if (line.size() == 1 && (line[0] == 'Q' || line[0] == 'q')) {
                print_utf8_line("Saliendo.");
                return 0;
            }
            appid = line;
            continue;
        }

        // pedir a la Store API si existe
        print_utf8_line("Comprobando AppID en Steam Store...");
        std::string resp;
        bool fetched = http_get_appdetails(appid, resp);
        bool exists = false;
        std::string gamename;
        if (fetched) {
            exists = resp_indicates_success(resp, appid);
            if (exists) gamename = extract_game_name(resp, appid);
        }

        if (!fetched) {
            print_utf8_line("No se pudo contactar con la Steam Store (comprobar conexion).");
            print_utf8("Do you want to retry? (Y/N): ");
            std::string ans; std::getline(std::cin, ans);
            if (!ans.empty() && (ans[0] == 'Y' || ans[0] == 'y')) {
                // retry (ask for appid again)
                print_utf8("Introduce AppID (Q para salir): ");
                std::string line; std::getline(std::cin, line);
                line = trim(line);
                if (line.size() == 1 && (line[0] == 'Q' || line[0] == 'q')) { print_utf8_line("Saliendo."); return 0; }
                appid = line;
                continue;
            }
            else {
                print_utf8_line("Continuando sin comprobar AppID (puede fallar SteamAPI_Init).");
                break;
            }
        }

        if (!exists) {
            print_utf8_line("AppID no encontrado en Steam Store.");
            print_utf8("Introduce otro AppID o pulsa Q para salir: ");
            std::string line; std::getline(std::cin, line);
            line = trim(line);
            if (line.size() == 1 && (line[0] == 'Q' || line[0] == 'q')) { print_utf8_line("Saliendo."); return 0; }
            appid = line;
            continue;
        }

        // si llegamos aquí, appid es numerico y existe
        if (!gamename.empty()) {
            std::string msg = "Ejecutando el juego \"" + gamename + "\" (AppID " + appid + ")...";
            print_utf8_line(msg);
        }
        else {
            std::string msg = "Ejecutando AppID " + appid + " (nombre no encontrado) ...";
            print_utf8_line(msg);
        }
        break;
    }

    // Guardar appid en steam_appid.txt (persistente)
    {
        std::ofstream ofs("steam_appid.txt", std::ios::trunc);
        if (ofs) ofs << appid << std::endl;
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

    // *** REPLACE: print the failure message using a wide literal with escapes to ensure the inverted question mark prints correctly ***
    if (!initSuccess) {
        // Use a wide literal and unicode escapes for non-ascii characters
        print_wline(L"Hubo un problema al iniciar el juego. Realice las siguientes comprobaciones:");
        print_wline(L"- El cliente de Steam est\u00E1 ejecut\u00E1ndose y con la sesi\u00F3n iniciada");
        print_wline(L"- La AppID introducida se corresponde con la de alg\u00FAn juego registrado en la cuenta de Steam");
        FreeLibrary(h);
        return 4;
    }

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
