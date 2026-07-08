#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <winhttp.h>
#include <shlobj.h>
#include <processthreadsapi.h>
#include <winsock2.h>
#include <ws2tcpip.h>
#include <wtsapi32.h>
#include <userenv.h>
#include <setupapi.h>
#include <psapi.h>
#include <sddl.h>
#include <iostream>
#pragma comment(lib, "version.lib")
#pragma comment(lib, "psapi.lib")
#include <fstream>
#include <string>
#include <vector>
#include <chrono>
#include <thread>
#include <sstream>
#include <iomanip>
#include <algorithm>
#include <unordered_map>
#include <map>
#include <cctype>
#include <ctime>
#include <random>
#include <mutex>
#include <cstdarg>
#include <atomic>
#include <wincrypt.h>
#include <cstring>
#include <cstdio>
#include <queue>
#include <condition_variable>
#include <tlhelp32.h>
#include <iphlpapi.h>
#include <lm.h>
#include <rpcdce.h>

#pragma comment(lib, "winhttp.lib")
#pragma comment(lib, "advapi32.lib")
#pragma comment(lib, "ws2_32.lib")
#pragma comment(lib, "crypt32.lib")
#pragma comment(lib, "wtsapi32.lib")
#pragma comment(lib, "userenv.lib")
#pragma comment(lib, "netapi32.lib")
#pragma comment(lib, "iphlpapi.lib")

#include "rdp_agent.h"

#ifndef SERVER_URL
#define SERVER_URL "https://localhost"
#endif
#ifndef BUILD_SLUG
#define BUILD_SLUG "1.0.0"
#endif

std::string serverURL = SERVER_URL;
std::string buildSlug = BUILD_SLUG;

// ─── Async Logger ───────────────────────────────────────────

class AsyncLogger
{
    std::ofstream m_file;
    std::thread m_thread;
    std::mutex m_mutex;
    std::condition_variable m_cv;
    std::queue<std::string> m_queue;
    std::atomic<bool> m_stop{false};

    void run()
    {
        std::vector<std::string> batch;
        batch.reserve(64);
        while (!m_stop)
        {
            {
                std::unique_lock<std::mutex> lk(m_mutex);
                m_cv.wait_for(lk, std::chrono::milliseconds(500),
                              [this] { return m_stop || !m_queue.empty(); });
                while (!m_queue.empty()) { batch.push_back(std::move(m_queue.front())); m_queue.pop(); }
            }
            if (batch.empty()) continue;
            if (m_file.is_open()) { for (auto &s : batch) m_file << s; m_file.flush(); }
            batch.clear();
        }
    }

public:
    AsyncLogger() { m_thread = std::thread([this] { run(); }); }
    ~AsyncLogger() { stop(); }
    void open(const std::string &path) { m_file.open(path, std::ios::app | std::ios::out); }
    void log(const std::string &msg)
    {
        if (!m_file.is_open()) return;
        std::lock_guard<std::mutex> lk(m_mutex);
        m_queue.push(msg);
        m_cv.notify_one();
    }
    bool is_open() const { return m_file.is_open(); }
    void stop()
    {
        m_stop = true; m_cv.notify_one();
        if (m_thread.joinable()) m_thread.join();
        if (m_file.is_open()) m_file.close();
    }
};

AsyncLogger g_logger;
std::atomic<bool> g_stopRequested(false);
std::string g_agent_uuid;
std::string g_agent_token;
std::atomic<int> g_rdp_worker_timeout{5};
std::string g_rdp_worker_codec;
std::string g_rdp_worker_encoder;
std::string g_rdp_worker_quality;
int g_rdp_worker_fps = 0;

static std::mutex g_pending_telemetry_m;
static std::string g_pending_telemetry_resp;

static HANDLE g_shm_handle = NULL;
static ActivityShm *g_shm = NULL;
static std::string g_shm_name;
static std::mutex g_shm_m;

SERVICE_STATUS serviceStatus = {0};
SERVICE_STATUS_HANDLE serviceHandle = NULL;
HANDLE stopEvent = NULL;

// RDP worker process tracking
static std::mutex g_rdp_worker_m;
static PROCESS_INFORMATION g_rdp_worker_pi = {0};
std::string g_rdp_server_host;
int g_rdp_server_port = 443;
std::string g_rdp_agent_id;
bool g_rdp_verify_cert = false;

// ─── Utility ────────────────────────────────────────────────

static std::string getExePath()
{
    char buf[MAX_PATH];
    GetModuleFileNameA(NULL, buf, sizeof(buf));
    return buf;
}

static std::string getExeDir()
{
    std::string p = getExePath();
    auto pos = p.rfind('\\');
    return (pos != std::string::npos) ? p.substr(0, pos) : p;
}

// ─── Logging ────────────────────────────────────────────────

void setupFileLogger(const std::string &name = "agent.log")
{
    std::string logPath = getExeDir() + "\\" + name;
    g_logger.open(logPath);
}

void log(const std::string &msg)
{
    auto now = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
    std::tm tm;
    localtime_s(&tm, &now);
    char buf[64];
    strftime(buf, sizeof(buf), "[%Y-%m-%d %H:%M:%S]", &tm);
    g_logger.log(std::string(buf) + " " + msg + "\n");
}

void log(const char *msg) { log(std::string(msg)); }

void logf(const char *fmt, ...)
{
    char buf[4096];
    va_list args;
    va_start(args, fmt);
    vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);
    log(std::string(buf));
}

static std::string ansiToUtf8(const std::string &ansi)
{
    int wlen = MultiByteToWideChar(CP_ACP, 0, ansi.c_str(), (int)ansi.size(), NULL, 0);
    if (wlen <= 0) return ansi;
    std::vector<wchar_t> w((size_t)wlen);
    MultiByteToWideChar(CP_ACP, 0, ansi.c_str(), (int)ansi.size(), w.data(), wlen);
    int ulen = WideCharToMultiByte(CP_UTF8, 0, w.data(), wlen, NULL, 0, NULL, NULL);
    if (ulen <= 0) return ansi;
    std::vector<char> u((size_t)ulen);
    WideCharToMultiByte(CP_UTF8, 0, w.data(), wlen, u.data(), ulen, NULL, NULL);
    return std::string(u.data());
}

static std::string loadOrCreateMachineUID()
{
    std::string path = getExeDir() + "\\machine_uid";
    std::ifstream f(path);
    if (f.is_open()) { std::string uid; std::getline(f, uid); return uid; }

    UUID uid;
    UuidCreate(&uid);
    char *s = NULL;
    UuidToStringA(&uid, (RPC_CSTR *)&s);
    std::string result = s ? s : "";
    RpcStringFreeA((RPC_CSTR *)&s);

    std::ofstream of(path);
    if (of.is_open()) of << result;
    return result;
}

static std::string getExternalIP()
{
    HINTERNET hSession = WinHttpOpen(L"RTSP_Agent/1.0", WINHTTP_ACCESS_TYPE_NO_PROXY, NULL, NULL, 0);
    if (!hSession) return "";
    HINTERNET hConnect = WinHttpConnect(hSession, L"api.ipify.org", INTERNET_DEFAULT_HTTP_PORT, 0);
    if (!hConnect) { WinHttpCloseHandle(hSession); return ""; }
    HINTERNET hRequest = WinHttpOpenRequest(hConnect, L"GET", NULL, NULL, NULL, NULL, 0);
    if (!hRequest) { WinHttpCloseHandle(hConnect); WinHttpCloseHandle(hSession); return ""; }
    if (!WinHttpSendRequest(hRequest, NULL, 0, NULL, 0, 0, 0) ||
        !WinHttpReceiveResponse(hRequest, NULL))
    {
        WinHttpCloseHandle(hRequest); WinHttpCloseHandle(hConnect); WinHttpCloseHandle(hSession);
        return "";
    }
    char buffer[64] = {0};
    DWORD bytesRead = 0;
    std::string result;
    while (WinHttpReadData(hRequest, buffer, sizeof(buffer) - 1, &bytesRead) && bytesRead > 0)
    {
        buffer[bytesRead] = 0;
        result += buffer;
    }
    WinHttpCloseHandle(hRequest); WinHttpCloseHandle(hConnect); WinHttpCloseHandle(hSession);
    // Trim trailing newline
    while (!result.empty() && (result.back() == '\n' || result.back() == '\r'))
        result.pop_back();
    return result;
}

static bool httpRequest(const std::string &verb, const std::string &url,
                        const std::string &body, std::string &responseBody, int &statusCode,
                        const std::string &contentType = "application/json")
{
    std::string host, path, query;
    int port = 443;
    bool isHttps = url.find("https://") == 0;

    size_t start = isHttps ? 8 : 7;
    size_t slash = url.find('/', start);
    if (slash == std::string::npos) return false;
    std::string hostPart = url.substr(start, slash - start);
    size_t colon = hostPart.find(':');
    if (colon != std::string::npos) {
        host = hostPart.substr(0, colon);
        port = std::stoi(hostPart.substr(colon + 1));
    } else {
        host = hostPart;
        port = isHttps ? 443 : 80;
    }
    path = url.substr(slash);
    size_t qm = path.find('?');
    if (qm != std::string::npos) { query = path.substr(qm); path = path.substr(0, qm); }

    HINTERNET hSession = WinHttpOpen(L"RTSP-Agent/1.0", WINHTTP_ACCESS_TYPE_NO_PROXY, NULL, NULL, 0);
    if (!hSession) return false;

    std::wstring whost(host.begin(), host.end());
    HINTERNET hConnect = WinHttpConnect(hSession, whost.c_str(), port, 0);
    if (!hConnect) { WinHttpCloseHandle(hSession); return false; }

    std::wstring wpath(path.begin(), path.end());
    std::wstring wquery(query.begin(), query.end());
    DWORD flags = isHttps ? WINHTTP_FLAG_SECURE : 0;
    HINTERNET hRequest = WinHttpOpenRequest(hConnect, std::wstring(verb.begin(), verb.end()).c_str(),
                                             wpath.c_str(), NULL, NULL, NULL, flags);
    if (!hRequest) { WinHttpCloseHandle(hConnect); WinHttpCloseHandle(hSession); return false; }

    if (isHttps) {
        DWORD certFlags = SECURITY_FLAG_IGNORE_UNKNOWN_CA |
                          SECURITY_FLAG_IGNORE_CERT_DATE_INVALID |
                          SECURITY_FLAG_IGNORE_CERT_CN_INVALID |
                          SECURITY_FLAG_IGNORE_CERT_WRONG_USAGE;
        WinHttpSetOption(hRequest, WINHTTP_OPTION_SECURITY_FLAGS, &certFlags, sizeof(certFlags));
    }

    std::wstring wheaders = L"Content-Type: " + std::wstring(contentType.begin(), contentType.end());
    std::string fullPath = path;
    if (!query.empty()) fullPath += query;

    if (!WinHttpSendRequest(hRequest, wheaders.c_str(), (DWORD)wheaders.size(),
                            (LPVOID)body.data(), (DWORD)body.size(), (DWORD)body.size(), 0))
    { WinHttpCloseHandle(hRequest); WinHttpCloseHandle(hConnect); WinHttpCloseHandle(hSession); return false; }

    if (!WinHttpReceiveResponse(hRequest, NULL))
    { WinHttpCloseHandle(hRequest); WinHttpCloseHandle(hConnect); WinHttpCloseHandle(hSession); return false; }

    DWORD code = 0;
    DWORD codeSize = sizeof(code);
    WinHttpQueryHeaders(hRequest, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                        NULL, &code, &codeSize, NULL);
    statusCode = (int)code;

    responseBody.clear();
    char buf[4096];
    DWORD read = 0;
    while (WinHttpReadData(hRequest, buf, sizeof(buf), &read) && read > 0) {
        responseBody.append(buf, read);
        read = 0;
    }

    WinHttpCloseHandle(hRequest);
    WinHttpCloseHandle(hConnect);
    WinHttpCloseHandle(hSession);
    return true;
}

static bool postJSON(const std::string &url, const std::string &body,
                     std::string &resp, int &code)
{
    return httpRequest("POST", url, body, resp, code, "application/json");
}

static bool parseUrl(const std::string &url, std::string &host, int &port,
                     std::string &path, std::string &query)
{
    bool https = url.find("https://") == 0;
    size_t start = https ? 8 : 7;
    size_t slash = url.find('/', start);
    if (slash == std::string::npos) return false;
    std::string hp = url.substr(start, slash - start);
    size_t colon = hp.find(':');
    if (colon != std::string::npos) {
        host = hp.substr(0, colon);
        port = std::stoi(hp.substr(colon + 1));
    } else {
        host = hp;
        port = https ? 443 : 80;
    }
    size_t qm = url.find('?', slash);
    if (qm != std::string::npos) {
        path = url.substr(slash, qm - slash);
        query = url.substr(qm);
    } else {
        path = url.substr(slash);
        query.clear();
    }
    return true;
}

static std::string sha256File(const std::string &path)
{
    HANDLE hFile = CreateFileA(path.c_str(), GENERIC_READ, FILE_SHARE_READ, NULL,
                               OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (hFile == INVALID_HANDLE_VALUE) return "";

    HCRYPTPROV prov = 0;
    HCRYPTHASH hash = 0;
    CryptAcquireContext(&prov, NULL, NULL, PROV_RSA_AES, CRYPT_VERIFYCONTEXT);
    CryptCreateHash(prov, CALG_SHA_256, 0, 0, &hash);

    char buf[4096];
    DWORD read = 0;
    while (ReadFile(hFile, buf, sizeof(buf), &read, NULL) && read > 0)
        CryptHashData(hash, (BYTE *)buf, read, 0);
    CloseHandle(hFile);

    BYTE result[32];
    DWORD resultLen = 32;
    CryptGetHashParam(hash, HP_HASHVAL, result, &resultLen, 0);
    CryptDestroyHash(hash);
    CryptReleaseContext(prov, 0);

    std::ostringstream hex;
    for (int i = 0; i < 32; ++i)
        hex << std::hex << std::setw(2) << std::setfill('0') << (int)result[i];
    return hex.str();
}

// ─── Telemetry ──────────────────────────────────────────────

struct TelemetryData
{
    std::string system;
    std::string userName;
    std::string externalIP;
    uint64_t totalMemory = 0;
    uint64_t availableMemory = 0;
    std::vector<std::string> disks;
    std::string encoder_capabilities;
    std::string name_pc;
    std::string machine_uid;
};

static TelemetryData collectTelemetry()
{
    TelemetryData td;

    char hostname[256];
    DWORD size = sizeof(hostname);
    GetComputerNameA(hostname, &size);
    td.name_pc = ansiToUtf8(hostname);

    td.machine_uid = loadOrCreateMachineUID();
    td.externalIP = getExternalIP();

    // OS version
    OSVERSIONINFOEXA osvi = {sizeof(osvi)};
    DWORDLONG cm = 0;
    if (GetVersionExA((LPOSVERSIONINFOA)&osvi)) {
        td.system = "Windows " + std::to_string(osvi.dwMajorVersion) + "." +
                     std::to_string(osvi.dwMinorVersion) + " build " +
                     std::to_string(osvi.dwBuildNumber);
    }

    // Memory
    MEMORYSTATUSEX mem = {sizeof(mem)};
    if (GlobalMemoryStatusEx(&mem)) {
        td.totalMemory = mem.ullTotalPhys / (1024 * 1024);
        td.availableMemory = mem.ullAvailPhys / (1024 * 1024);
    }

    // Disks
    DWORD drives = GetLogicalDrives();
    for (int i = 0; i < 26; ++i) {
        if (drives & (1 << i)) {
            char root[4] = {(char)('A' + i), ':', '\\', 0};
            ULARGE_INTEGER free, total;
            if (GetDiskFreeSpaceExA(root, &free, &total, NULL)) {
                std::string d = "{\"mount\":\"" + std::string(1, (char)('A' + i)) +
                    "\",\"total\":" + std::to_string(total.QuadPart / (1024 * 1024 * 1024)) +
                    ",\"free\":" + std::to_string(free.QuadPart / (1024 * 1024 * 1024)) + "}";
                td.disks.push_back(d);
            }
        }
    }

    td.encoder_capabilities = "{\"cpu\":true}";
    return td;
}

// ─── Shared Memory ──────────────────────────────────────────

static void close_activity_shm()
{
    std::lock_guard<std::mutex> lk(g_shm_m);
    if (g_shm) { UnmapViewOfFile((LPVOID)g_shm); g_shm = nullptr; }
    if (g_shm_handle) { CloseHandle(g_shm_handle); g_shm_handle = NULL; }
}

// ─── RDP Worker Management ──────────────────────────────────

static DWORD FindActiveUserSessionId()
{
    DWORD sessionId = WTSGetActiveConsoleSessionId();
    if (sessionId != 0xFFFFFFFF && sessionId != 0)
        return sessionId;

    PWTS_SESSION_INFOW pInfo = NULL;
    DWORD count = 0;
    if (!WTSEnumerateSessionsW(WTS_CURRENT_SERVER_HANDLE, 0, 1, &pInfo, &count))
        return 0xFFFFFFFF;

    DWORD result = 0xFFFFFFFF;
    for (DWORD i = 0; i < count; i++)
    {
        if (pInfo[i].State == WTSActive && pInfo[i].SessionId != 0)
        {
            result = pInfo[i].SessionId;
            break;
        }
    }
    WTSFreeMemory(pInfo);
    return result;
}

typedef BOOL (WINAPI *WTSImpersonateUserFn)(HANDLE, DWORD);
static WTSImpersonateUserFn g_WTSImpersonateUser = NULL;

static bool LoadWTSImpersonateUser()
{
    if (g_WTSImpersonateUser) return true;
    HMODULE hMod = GetModuleHandleA("wtsapi32.dll");
    if (!hMod) hMod = LoadLibraryA("wtsapi32.dll");
    if (!hMod) return false;
    g_WTSImpersonateUser = (WTSImpersonateUserFn)GetProcAddress(hMod, "WTSImpersonateUser");
    return g_WTSImpersonateUser != NULL;
}

static HANDLE GetSessionTokenFromExplorer(DWORD sessionId)
{
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return NULL;

    PROCESSENTRY32W pe = {0};
    pe.dwSize = sizeof(pe);

    if (!Process32FirstW(snap, &pe))
    {
        CloseHandle(snap);
        return NULL;
    }

    do
    {
        if (_wcsicmp(pe.szExeFile, L"explorer.exe") != 0)
            continue;
        if (pe.th32ProcessID == GetCurrentProcessId())
            continue;

        HANDLE hProcess = OpenProcess(PROCESS_QUERY_INFORMATION, FALSE, pe.th32ProcessID);
        if (!hProcess) continue;

        HANDLE hToken = NULL;
        if (!OpenProcessToken(hProcess, TOKEN_DUPLICATE | TOKEN_ASSIGN_PRIMARY | TOKEN_QUERY, &hToken))
        {
            CloseHandle(hProcess);
            continue;
        }

        DWORD tokenSessionId = 0;
        DWORD sz = sizeof(tokenSessionId);
        if (!GetTokenInformation(hToken, TokenSessionId, &tokenSessionId, sz, &sz) ||
            tokenSessionId != sessionId)
        {
            CloseHandle(hToken);
            CloseHandle(hProcess);
            continue;
        }

        HANDLE hPrimary = NULL;
        if (!DuplicateTokenEx(hToken, MAXIMUM_ALLOWED, NULL,
                              SecurityImpersonation, TokenPrimary, &hPrimary))
        {
            CloseHandle(hToken);
            CloseHandle(hProcess);
            continue;
        }

        CloseHandle(hToken);
        CloseHandle(hProcess);
        CloseHandle(snap);
        return hPrimary;
    } while (Process32NextW(snap, &pe));

    CloseHandle(snap);
    return NULL;
}

static HANDLE GetSessionToken(DWORD sessionId)
{
    HANDLE hUser = NULL;

    if (WTSQueryUserToken(sessionId, &hUser))
        return hUser;

    if (LoadWTSImpersonateUser())
    {
        if (g_WTSImpersonateUser(WTS_CURRENT_SERVER_HANDLE, sessionId))
        {
            HANDLE hImp = NULL;
            if (OpenThreadToken(GetCurrentThread(), TOKEN_QUERY | TOKEN_DUPLICATE | TOKEN_ASSIGN_PRIMARY, FALSE, &hImp))
            {
                if (DuplicateTokenEx(hImp, MAXIMUM_ALLOWED, NULL,
                                      SecurityImpersonation, TokenPrimary, &hUser))
                {
                    CloseHandle(hImp);
                    RevertToSelf();
                    return hUser;
                }
                CloseHandle(hImp);
            }
            RevertToSelf();
        }
    }

    return GetSessionTokenFromExplorer(sessionId);
}

static HANDLE GetActiveUserToken()
{
    DWORD sessionId = FindActiveUserSessionId();
    if (sessionId == 0xFFFFFFFF) return NULL;

    HANDLE hUser = GetSessionToken(sessionId);
    if (!hUser) return NULL;

    DWORD sz = 0;
    TOKEN_ELEVATION_TYPE et = TokenElevationTypeDefault;
    if (GetTokenInformation(hUser, TokenElevationType, &et, sizeof(et), &sz))
    {
        if (et == TokenElevationTypeLimited)
        {
            TOKEN_LINKED_TOKEN lt = {0};
            if (GetTokenInformation(hUser, TokenLinkedToken, &lt, sizeof(lt), &sz))
            {
                CloseHandle(hUser);
                hUser = lt.LinkedToken;
            }
        }
    }

    HANDLE hPrimary = NULL;
    if (!DuplicateTokenEx(hUser, MAXIMUM_ALLOWED, NULL,
                          SecurityImpersonation, TokenPrimary, &hPrimary))
    {
        CloseHandle(hUser);
        return NULL;
    }
    CloseHandle(hUser);
    return hPrimary;
}

static void stopRDPWorker()
{
    std::lock_guard<std::mutex> lk(g_rdp_worker_m);
    if (g_rdp_worker_pi.hProcess)
    {
        log("[worker] Stopping RDP worker...");
        TerminateProcess(g_rdp_worker_pi.hProcess, 0);
        if (WaitForSingleObject(g_rdp_worker_pi.hProcess, 5000) == WAIT_TIMEOUT)
            log("[worker] Worker did not exit in 5s");
        CloseHandle(g_rdp_worker_pi.hProcess);
        CloseHandle(g_rdp_worker_pi.hThread);
        ZeroMemory(&g_rdp_worker_pi, sizeof(g_rdp_worker_pi));
        log("[worker] RDP worker stopped");
    }
}

static bool spawnRDPWorker()
{
    std::lock_guard<std::mutex> lk(g_rdp_worker_m);
    if (g_rdp_worker_pi.hProcess)
        return true;

    // Create shared memory for activity tracking
    {
        std::lock_guard<std::mutex> slk(g_shm_m);
        if (g_shm_handle) close_activity_shm();
        g_shm_name = "Global\\RTSPAct_" + g_agent_uuid;
        SECURITY_DESCRIPTOR sd;
        InitializeSecurityDescriptor(&sd, SECURITY_DESCRIPTOR_REVISION);
        SetSecurityDescriptorDacl(&sd, TRUE, NULL, FALSE);
        SECURITY_ATTRIBUTES sa{sizeof(sa), &sd, FALSE};
        g_shm_handle = CreateFileMappingA(INVALID_HANDLE_VALUE, &sa,
                                          PAGE_READWRITE, 0, sizeof(ActivityShm),
                                          g_shm_name.c_str());
        if (g_shm_handle)
        {
            g_shm = (ActivityShm *)MapViewOfFile(g_shm_handle, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(ActivityShm));
            if (g_shm) { g_shm->last_activity_time = GetTickCount64(); g_shm->timeout_min = g_rdp_worker_timeout.load(); }
        }
    }

    std::string selfPath = getExePath();
    std::ostringstream args;
    args << "\"" << selfPath << "\""
         << " --rdp-worker"
         << " --server=" << g_rdp_server_host
         << " --port=" << g_rdp_server_port
         << " --id=" << g_rdp_agent_id
         << " --token=" << g_agent_token;
    if (!g_rdp_verify_cert) args << " --insecure";
    int timeout_min = g_rdp_worker_timeout.load();
    if (timeout_min > 0) args << " --timeout=" << timeout_min;
    if (!g_shm_name.empty()) args << " --shm=" << g_shm_name;
    if (!g_rdp_worker_codec.empty()) args << " --codec=" << g_rdp_worker_codec;
    if (!g_rdp_worker_encoder.empty()) args << " --encoder=" << g_rdp_worker_encoder;
    if (!g_rdp_worker_quality.empty()) args << " --quality=" << g_rdp_worker_quality;
    if (g_rdp_worker_fps > 0) args << " --fps=" << g_rdp_worker_fps;
    std::string cmdline = args.str();

    HANDLE hUserToken = GetActiveUserToken();
    if (!hUserToken) return false;

    LPVOID envBlock = NULL;
    if (!CreateEnvironmentBlock(&envBlock, hUserToken, FALSE)) envBlock = NULL;

    STARTUPINFOA si = {0};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;
    si.lpDesktop = (LPSTR)"winsta0\\default";

    std::vector<char> buf(cmdline.begin(), cmdline.end());
    buf.push_back(0);

    PROCESS_INFORMATION pi = {0};
    BOOL ok = CreateProcessAsUserA(hUserToken, NULL, buf.data(), NULL, NULL, FALSE,
                                   CREATE_UNICODE_ENVIRONMENT | CREATE_NO_WINDOW,
                                   envBlock, NULL, &si, &pi);
    if (envBlock) DestroyEnvironmentBlock(envBlock);
    CloseHandle(hUserToken);

    if (!ok) return false;

    g_rdp_worker_pi = pi;
    log("[worker] RDP worker process started, pid=" + std::to_string(pi.dwProcessId));
    return true;
}

// ─── Inactivity Monitor ─────────────────────────────────────

static void inactivity_monitor_thread()
{
    while (!g_stopRequested)
    {
        std::this_thread::sleep_for(std::chrono::seconds(1));
        if (g_stopRequested) break;

        std::lock_guard<std::mutex> lk(g_shm_m);
        if (!g_shm || g_shm->timeout_min <= 0) continue;

        LONG64 now = GetTickCount64();
        LONG64 elapsed = (now - g_shm->last_activity_time) / 1000 / 60;
        if (elapsed >= g_shm->timeout_min)
        {
            logf("[inactivity] No input for %lld min (timeout=%ld), stopping worker",
                 elapsed, g_shm->timeout_min);
            stopRDPWorker();
            g_shm->last_activity_time = now;
        }
    }
}

// ─── Control Command Loop (WebSocket for agent-level commands) ──
// Simplified: polls for commands via HTTP, WebSocket will be added in Stage 5

static void poll_commands()
{
    // Poll for commands via HTTP GET /api/agent/commands
    // This is a simplified fallback; full WebSocket in Stage 5
    std::string url = serverURL + "/api/agent/commands?uuid=" + g_agent_uuid + "&token=" + g_agent_token;
    std::string resp;
    int code = 0;
    if (httpRequest("GET", url, "", resp, code) && code == 200)
    {
        if (resp.find("start-rdp-worker") != std::string::npos)
        {
            log("[cmd] Received start-rdp-worker command");
            spawnRDPWorker();
        }
        if (resp.find("stop-rdp-worker") != std::string::npos)
        {
            log("[cmd] Received stop-rdp-worker command");
            stopRDPWorker();
        }
    }
}

// ─── WebSocket Control Loop ─────────────────────────────────

static void controlCommandLoop()
{
    while (!g_stopRequested)
    {
        // TODO Stage 5: Full WebSocket control channel
        // For now, use HTTP polling
        poll_commands();
        for (int i = 0; i < 30 && !g_stopRequested; i++)
            std::this_thread::sleep_for(std::chrono::seconds(1));
    }
}

// ─── Auto-Update ────────────────────────────────────────────

static void checkForUpdate(const std::string &uuid, const std::string &token)
{
    std::string url = serverURL + "/api/agent/check-update?uuid=" + uuid + "&token=" + token;
    std::string body = "{\"build\":\"" + buildSlug + "\"}";
    std::string responseBody;
    int statusCode;

    if (!postJSON(url, body, responseBody, statusCode) || statusCode != 200)
        return;
    if (responseBody.find("\"update\":true") == std::string::npos)
        return;

    std::string newBuild, downloadUrl, sha256;
    size_t buildPos = responseBody.find("\"build\":\"");
    size_t urlPos = responseBody.find("\"url\":\"");
    size_t shaPos = responseBody.find("\"sha256\":\"");
    if (buildPos != std::string::npos && urlPos != std::string::npos && shaPos != std::string::npos)
    {
        buildPos += 9; urlPos += 7; shaPos += 10;
        size_t buildEnd = responseBody.find("\"", buildPos);
        size_t urlEnd = responseBody.find("\"", urlPos);
        size_t shaEnd = responseBody.find("\"", shaPos);
        if (buildEnd != std::string::npos && urlEnd != std::string::npos && shaEnd != std::string::npos)
        {
            newBuild = responseBody.substr(buildPos, buildEnd - buildPos);
            downloadUrl = responseBody.substr(urlPos, urlEnd - urlPos);
            sha256 = responseBody.substr(shaPos, shaEnd - shaPos);
        }
    }
    if (newBuild.empty() || downloadUrl.empty()) return;
    if (g_stopRequested) return;

    std::string exePath = getExePath();
    std::string tmpPath = exePath + ".new";

    std::string host, path, query;
    int port;
    if (!parseUrl(downloadUrl, host, port, path, query)) return;
    std::string fullPath = path + query;

    HINTERNET hSession = WinHttpOpen(L"Agent/1.0", WINHTTP_ACCESS_TYPE_NO_PROXY, NULL, NULL, 0);
    if (!hSession) return;
    std::wstring whost(host.begin(), host.end());
    HINTERNET hConnect = WinHttpConnect(hSession, whost.c_str(), port, 0);
    if (!hConnect) { WinHttpCloseHandle(hSession); return; }

    std::wstring wpath(fullPath.begin(), fullPath.end());
    DWORD dwFlags = (downloadUrl.find("https://") == 0) ? WINHTTP_FLAG_SECURE : 0;
    HINTERNET hRequest = WinHttpOpenRequest(hConnect, L"GET", wpath.c_str(), NULL, NULL, NULL, dwFlags);
    if (!hRequest) { WinHttpCloseHandle(hConnect); WinHttpCloseHandle(hSession); return; }

    if (downloadUrl.find("https://") == 0) {
        DWORD certFlags = SECURITY_FLAG_IGNORE_UNKNOWN_CA |
                          SECURITY_FLAG_IGNORE_CERT_DATE_INVALID |
                          SECURITY_FLAG_IGNORE_CERT_CN_INVALID |
                          SECURITY_FLAG_IGNORE_CERT_WRONG_USAGE;
        WinHttpSetOption(hRequest, WINHTTP_OPTION_SECURITY_FLAGS, &certFlags, sizeof(certFlags));
    }

    if (!WinHttpSendRequest(hRequest, NULL, 0, NULL, 0, 0, 0) ||
        !WinHttpReceiveResponse(hRequest, NULL))
    { WinHttpCloseHandle(hRequest); WinHttpCloseHandle(hConnect); WinHttpCloseHandle(hSession); return; }

    HANDLE hFile = CreateFileA(tmpPath.c_str(), GENERIC_WRITE, 0, 0, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, 0);
    if (hFile == INVALID_HANDLE_VALUE)
    { WinHttpCloseHandle(hRequest); WinHttpCloseHandle(hConnect); WinHttpCloseHandle(hSession); return; }

    char buffer[4096];
    DWORD bytesRead = 0;
    while (WinHttpReadData(hRequest, buffer, sizeof(buffer), &bytesRead) && bytesRead > 0)
    { DWORD written = 0; WriteFile(hFile, buffer, bytesRead, &written, NULL); }
    CloseHandle(hFile);
    WinHttpCloseHandle(hRequest);
    WinHttpCloseHandle(hConnect);
    WinHttpCloseHandle(hSession);

    std::string hash = sha256File(tmpPath);
    if (hash.empty() || hash != sha256) { DeleteFileA(tmpPath.c_str()); return; }

    stopRDPWorker();

    std::string oldPath = exePath + ".old";
    DeleteFileA(oldPath.c_str());
    if (MoveFileA(exePath.c_str(), oldPath.c_str()))
    {
        if (MoveFileA(tmpPath.c_str(), exePath.c_str()))
        {
            log("[update] Agent updated: version=" + buildSlug + " -> " + newBuild);
            STARTUPINFOA si = {0};
            si.cb = sizeof(si);
            si.dwFlags = STARTF_USESHOWWINDOW;
            si.wShowWindow = SW_HIDE;
            PROCESS_INFORMATION pi = {0};
            char cmd[MAX_PATH * 2];
            sprintf_s(cmd, sizeof(cmd),
                      "cmd.exe /c \"timeout /t 2 /nobreak >nul && sc start RTSPDesktopAgent\"");
            if (CreateProcessA(NULL, cmd, NULL, NULL, FALSE, CREATE_NO_WINDOW, NULL, NULL, &si, &pi))
            { CloseHandle(pi.hProcess); CloseHandle(pi.hThread); }
            Sleep(3000);
            g_logger.stop();
            ExitProcess(0);
        }
        else { MoveFileA(oldPath.c_str(), exePath.c_str()); }
    }
    else { DeleteFileA(tmpPath.c_str()); }
}

// ─── Main Logic ─────────────────────────────────────────────

static void mainLogic()
{
    if (serverURL.empty()) return;

    std::string machineUID = loadOrCreateMachineUID();
    char hostnameA[256];
    DWORD size = sizeof(hostnameA);
    GetComputerNameA(hostnameA, &size);
    std::string hostname = ansiToUtf8(std::string(hostnameA));

    std::string uuid, token;

    // Registration loop
    for (;;)
    {
        log("Registering agent...");
        if (g_stopRequested) { log("Stop requested, aborting registration"); return; }

        std::string url = serverURL + "/api/agent/register";
        TelemetryData td = collectTelemetry();
        std::string body = "{\"name_pc\":\"" + RDPAgent::json_escape(hostname) + "\","
                           "\"machine_uid\":\"" + RDPAgent::json_escape(machineUID) + "\","
                           "\"system\":\"" + RDPAgent::json_escape(td.system) + "\","
                           "\"exe_version\":\"" + RDPAgent::json_escape(buildSlug) + "\","
                           "\"external_ip\":\"" + RDPAgent::json_escape(getExternalIP()) + "\"}";
        std::string responseBody;
        int statusCode = 0;
        postJSON(url, body, responseBody, statusCode);
        if (statusCode == 200)
        {
            log("Registration successful");
            size_t uuidPos = responseBody.find("\"agent_uuid\":\"");
            size_t tokenPos = responseBody.find("\"token\":\"");
            if (uuidPos != std::string::npos && tokenPos != std::string::npos)
            {
                uuidPos += 14; tokenPos += 9;
                size_t uuidEnd = responseBody.find("\"", uuidPos);
                size_t tokenEnd = responseBody.find("\"", tokenPos);
                if (uuidEnd != std::string::npos && tokenEnd != std::string::npos)
                {
                    uuid = responseBody.substr(uuidPos, uuidEnd - uuidPos);
                    token = responseBody.substr(tokenPos, tokenEnd - tokenPos);
                    break;
                }
            }
        }
        log("Registration failed, retrying in 10 seconds...");
        for (int i = 0; i < 10 && !g_stopRequested; i++)
            std::this_thread::sleep_for(std::chrono::seconds(1));
    }
    if (g_stopRequested) { log("Stop requested"); return; }

    // Initial telemetry
    {
        TelemetryData t = collectTelemetry();
        std::string rb; int rc;
        std::string tb = "{\"system\":\"" + RDPAgent::json_escape(t.system) + "\","
                         "\"external_ip\":\"" + t.externalIP + "\","
                         "\"memory\":{\"total\":" + std::to_string(t.totalMemory) + ","
                         "\"available\":" + std::to_string(t.availableMemory) + "},"
                         "\"exe_version\":\"" + buildSlug + "\","
                         "\"disks\":[";
        for (size_t i = 0; i < t.disks.size(); i++) { if (i > 0) tb += ","; tb += t.disks[i]; }
        tb += "],\"name_pc\":\"" + RDPAgent::json_escape(t.name_pc) + "\","
              "\"machine_uid\":\"" + RDPAgent::json_escape(t.machine_uid) + "\"}";
        postJSON(serverURL + "/api/agent/telemetry?uuid=" + uuid + "&token=" + token, tb, rb, rc);
    }

    // Init RDP config
    {
        std::string rdp_host, rdp_path, rdp_query;
        int rdp_port;
        parseUrl(serverURL, rdp_host, rdp_port, rdp_path, rdp_query);
        g_rdp_server_host = rdp_host;
        g_rdp_server_port = rdp_port;
        g_rdp_agent_id = uuid;
        g_rdp_verify_cert = false;
        g_agent_uuid = uuid;
        g_agent_token = token;
    }

    // Start control command thread
    std::thread cmdThread(controlCommandLoop);
    std::thread inactivityThread(inactivity_monitor_thread);

    log("Entering main loop...");
    while (!g_stopRequested)
    {
        for (int i = 0; i < 60 && !g_stopRequested; i++)
            std::this_thread::sleep_for(std::chrono::seconds(1));
        if (!g_stopRequested)
            checkForUpdate(uuid, token);
    }

    log("Cleaning up...");
    stopRDPWorker();
    if (cmdThread.joinable()) cmdThread.join();
    if (inactivityThread.joinable()) inactivityThread.join();
    log("Main logic finished");
}

// ─── Service ────────────────────────────────────────────────

static bool isServiceInstalled()
{
    SC_HANDLE scm = OpenSCManager(NULL, NULL, SC_MANAGER_CONNECT);
    if (!scm) return false;
    SC_HANDLE svc = OpenServiceA(scm, "RTSPDesktopAgent", SERVICE_QUERY_STATUS);
    bool installed = (svc != NULL);
    if (svc) CloseServiceHandle(svc);
    CloseServiceHandle(scm);
    return installed;
}

static bool installService()
{
    std::string exePath = getExePath();
    SC_HANDLE scm = OpenSCManager(NULL, NULL, SC_MANAGER_CREATE_SERVICE);
    if (!scm) return false;
    SC_HANDLE svc = CreateServiceA(scm, "RTSPDesktopAgent", "RTSP Desktop Agent",
        SERVICE_ALL_ACCESS, SERVICE_WIN32_OWN_PROCESS,
        SERVICE_AUTO_START, SERVICE_ERROR_NORMAL,
        exePath.c_str(), NULL, NULL, NULL, NULL, NULL);
    if (!svc) { CloseServiceHandle(scm); return false; }
    SERVICE_FAILURE_ACTIONS actions = {0};
    SC_ACTION action = {SC_ACTION_RESTART, 1000};
    actions.cActions = 1;
    actions.lpsaActions = &action;
    actions.dwResetPeriod = 86400;
    ChangeServiceConfig2A(svc, SERVICE_CONFIG_FAILURE_ACTIONS, &actions);
    CloseServiceHandle(svc);
    CloseServiceHandle(scm);
    return true;
}

static void uninstallService()
{
    SC_HANDLE scm = OpenSCManager(NULL, NULL, SC_MANAGER_ALL_ACCESS);
    if (!scm) return;
    SC_HANDLE svc = OpenServiceA(scm, "RTSPDesktopAgent",
        SERVICE_STOP | SERVICE_QUERY_STATUS | DELETE);
    if (svc) {
        SERVICE_STATUS ss = {0};
        ControlService(svc, SERVICE_CONTROL_STOP, &ss);
        for (int i = 0; i < 10; i++) {
            if (!QueryServiceStatus(svc, &ss)) break;
            if (ss.dwCurrentState == SERVICE_STOPPED) break;
            Sleep(500);
        }
        DeleteService(svc);
        CloseServiceHandle(svc);
    }
    CloseServiceHandle(scm);
}

static void self_delete()
{
    log("Self-delete command received");
    stopRDPWorker();
    close_activity_shm();
    std::string exeDir = getExeDir();
    std::string exePath = getExePath();
    g_logger.stop();

    DeleteFileA((exeDir + "\\agent.log").c_str());
    DeleteFileA((exeDir + "\\agent_rdp.log").c_str());
    DeleteFileA((exeDir + "\\machine_uid").c_str());
    DeleteFileA((exePath + ".new").c_str());
    DeleteFileA((exePath + ".old").c_str());

    uninstallService();

    std::string batPath = exeDir + "\\~sd.bat";
    {
        std::ofstream bat(batPath);
        if (bat.is_open()) {
            bat << "@echo off\r\n"
                << "timeout /t 3 /nobreak >nul\r\n"
                << "del /f /q \"" << exePath << "\"\r\n"
                << "rmdir /q \"" << exeDir << "\"\r\n"
                << "del /f /q \"" << batPath << "\"\r\n";
        }
    }
    STARTUPINFOA si = {0};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;
    PROCESS_INFORMATION pi = {0};
    std::string cmdLine = "cmd.exe /c \"" + batPath + "\"";
    if (CreateProcessA(NULL, (LPSTR)cmdLine.c_str(), NULL, NULL, FALSE,
                       CREATE_NO_WINDOW, NULL, NULL, &si, &pi))
    { CloseHandle(pi.hProcess); CloseHandle(pi.hThread); }
    Sleep(1000);
    g_logger.stop();
    ExitProcess(0);
}

// ─── Service Handlers ───────────────────────────────────────

VOID WINAPI serviceCtrlHandler(DWORD ctrlCode)
{
    if (ctrlCode == SERVICE_CONTROL_STOP) {
        log("Service stop requested");
        serviceStatus.dwCurrentState = SERVICE_STOP_PENDING;
        SetServiceStatus(serviceHandle, &serviceStatus);
        g_stopRequested = true;
        if (stopEvent) SetEvent(stopEvent);
    }
}

VOID WINAPI serviceMain(DWORD argc, LPWSTR *argv)
{
    serviceStatus.dwServiceType = SERVICE_WIN32;
    serviceStatus.dwCurrentState = SERVICE_START_PENDING;
    serviceStatus.dwControlsAccepted = SERVICE_ACCEPT_STOP;
    serviceStatus.dwWin32ExitCode = 0;
    serviceStatus.dwServiceSpecificExitCode = 0;
    serviceHandle = RegisterServiceCtrlHandlerW(L"RTSPDesktopAgent", serviceCtrlHandler);
    if (!serviceHandle) return;

    SetServiceStatus(serviceHandle, &serviceStatus);
    stopEvent = CreateEvent(NULL, TRUE, FALSE, NULL);
    if (!stopEvent) {
        serviceStatus.dwCurrentState = SERVICE_STOPPED;
        SetServiceStatus(serviceHandle, &serviceStatus);
        return;
    }
    serviceStatus.dwCurrentState = SERVICE_RUNNING;
    SetServiceStatus(serviceHandle, &serviceStatus);

    log("Service main started");
    mainLogic();

    CloseHandle(stopEvent);
    serviceStatus.dwCurrentState = SERVICE_STOPPED;
    SetServiceStatus(serviceHandle, &serviceStatus);
    log("Service stopped");
}

// ─── Main Entry Point ───────────────────────────────────────

int main(int argc, char *argv[])
{
    bool worker_mode = false;
    bool install_flag = false;
    bool uninstall_flag = false;
    bool delete_flag = false;
    std::string cli_server, cli_id, cli_token, cli_shm;
    std::string cli_codec, cli_encoder, cli_quality;
    int cli_port = 443;
    bool cli_insecure = false;
    int cli_timeout = 0;
    int cli_fps = 0;

    for (int i = 1; i < argc; ++i)
    {
        std::string a = argv[i];
        if (a == "--rdp-worker") worker_mode = true;
        else if (a == "--install") install_flag = true;
        else if (a == "--uninstall") uninstall_flag = true;
        else if (a == "--self-delete") delete_flag = true;
        else if (a == "--insecure") cli_insecure = true;
        else if (a.rfind("--server=", 0) == 0) cli_server = a.substr(9);
        else if (a.rfind("--port=", 0) == 0) { try { cli_port = std::stoi(a.substr(7)); } catch (...) {} }
        else if (a.rfind("--id=", 0) == 0) cli_id = a.substr(5);
        else if (a.rfind("--token=", 0) == 0) cli_token = a.substr(8);
        else if (a.rfind("--timeout=", 0) == 0) { try { cli_timeout = std::stoi(a.substr(10)); } catch (...) {} }
        else if (a.rfind("--shm=", 0) == 0) cli_shm = a.substr(6);
        else if (a.rfind("--codec=", 0) == 0) cli_codec = a.substr(8);
        else if (a.rfind("--encoder=", 0) == 0) cli_encoder = a.substr(10);
        else if (a.rfind("--quality=", 0) == 0) cli_quality = a.substr(10);
        else if (a.rfind("--fps=", 0) == 0) { try { cli_fps = std::stoi(a.substr(6)); } catch (...) {} }
    }

    // Handle explicit --install / --uninstall / --self-delete
    if (install_flag) {
        setupFileLogger("agent.log");
        if (installService()) {
            log("Service installed");
            SC_HANDLE scm = OpenSCManager(NULL, NULL, SC_MANAGER_CONNECT);
            if (scm) {
                SC_HANDLE svc = OpenServiceA(scm, "RTSPDesktopAgent", SERVICE_START);
                if (svc) { StartServiceA(svc, 0, NULL); CloseServiceHandle(svc); }
                CloseServiceHandle(scm);
            }
        } else {
            log("Service install failed (may already exist)");
        }
        return 0;
    }

    if (uninstall_flag) {
        setupFileLogger("agent.log");
        uninstallService();
        log("Service uninstalled");
        return 0;
    }

    if (delete_flag) {
        setupFileLogger("agent.log");
        self_delete();
        return 0;
    }

    // Worker mode
    if (worker_mode) {
        setupFileLogger("agent_rdp.log");
        log("=== Starting in RDP-WORKER mode ===");
        return run_rdp_worker(cli_server, cli_port, cli_id, cli_token, !cli_insecure,
                              cli_timeout, cli_shm, cli_codec, cli_encoder, cli_quality, cli_fps);
    }

    // Service / console mode
    setupFileLogger("agent.log");
    log("Agent started as console app");

    if (!isServiceInstalled()) {
        if (installService()) {
            SC_HANDLE scm = OpenSCManager(NULL, NULL, SC_MANAGER_CONNECT);
            if (scm) {
                SC_HANDLE svc = OpenServiceA(scm, "RTSPDesktopAgent", SERVICE_START);
                if (svc) { StartServiceA(svc, 0, NULL); CloseServiceHandle(svc); }
                CloseServiceHandle(scm);
            }
        }
        return 0;
    }

    SERVICE_TABLE_ENTRYW table[] = {
        {(LPWSTR)L"RTSPDesktopAgent", serviceMain},
        {NULL, NULL}
    };
    log("Starting service dispatcher...");
    StartServiceCtrlDispatcherW(table);
    log("Service dispatcher exited");
    return 0;
}