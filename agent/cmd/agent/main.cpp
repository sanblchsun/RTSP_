// agent/cmd/agent/main.cpp
// RTSP Desktop Agent — Windows service + streaming worker
// Service (SYSTEM) manages worker via CreateProcessAsUserA in user session
// Worker captures WGC, encodes x264, pushes RTP over RTSP TCP interleaved

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <winhttp.h>
#include <processthreadsapi.h>
#include <winsock2.h>
#include <ws2tcpip.h>
#include <wtsapi32.h>
#include <userenv.h>
#include <psapi.h>
#include <sddl.h>
#include <tlhelp32.h>
#include <iphlpapi.h>
#include <mmsystem.h>

#include <iostream>
#include <fstream>
#include <string>
#include <vector>
#include <chrono>
#include <thread>
#include <sstream>
#include <iomanip>
#include <algorithm>
#include <cctype>
#include <ctime>
#include <random>
#include <mutex>
#include <cstdarg>
#include <atomic>
#include <cstring>
#include <cstdio>
#include <queue>
#include <condition_variable>
#include <csignal>

#pragma comment(lib, "winhttp.lib")
#pragma comment(lib, "advapi32.lib")
#pragma comment(lib, "ws2_32.lib")
#pragma comment(lib, "winmm.lib")
#pragma comment(lib, "wtsapi32.lib")
#pragma comment(lib, "userenv.lib")
#pragma comment(lib, "netapi32.lib")
#pragma comment(lib, "iphlpapi.lib")
#pragma comment(lib, "psapi.lib")

#include "WinRT-API/capture_wgc.h"
#include "WinRT-API/encoder_x264.h"
#include "rtp/h264_rtp_packetizer.h"
#include "rtp/rtp_header.h"

#ifndef SERVER_URL
#define SERVER_URL "http://localhost:8000"
#endif
#ifndef BUILD_SLUG
#define BUILD_SLUG "1.0.0"
#endif

#define SERVICE_NAME L"RTSPDesktopAgent"
#define SERVICE_DISPLAY_NAME L"RTSP Desktop Agent"

std::string g_server_url = SERVER_URL;
std::string g_build_slug = BUILD_SLUG;

// ======================== LOGGER ========================

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
                while (!m_queue.empty())
                {
                    batch.push_back(std::move(m_queue.front()));
                    m_queue.pop();
                }
            }
            if (batch.empty())
                continue;
            if (m_file.is_open())
            {
                for (auto &s : batch)
                    m_file << s;
                m_file.flush();
            }
            batch.clear();
        }
    }

public:
    AsyncLogger()
    {
        m_thread = std::thread([this] { run(); });
    }

    ~AsyncLogger() { stop(); }

    void open(const std::string &path)
    {
        m_file.open(path, std::ios::app | std::ios::out);
    }

    void log(const std::string &msg)
    {
        if (!m_file.is_open())
            return;
        std::lock_guard<std::mutex> lk(m_mutex);
        m_queue.push(msg);
        m_cv.notify_one();
    }

    bool is_open() const { return m_file.is_open(); }

    void stop()
    {
        m_stop = true;
        m_cv.notify_one();
        if (m_thread.joinable())
            m_thread.join();
        if (m_file.is_open())
            m_file.close();
    }
};

AsyncLogger g_logger;
std::atomic<bool> g_stopRequested(false);

// ======================== HELPERS ========================

std::string getExePath()
{
    char path[MAX_PATH] = {0};
    GetModuleFileNameA(NULL, path, MAX_PATH);
    return std::string(path);
}

std::string getExeDir()
{
    std::string exePath = getExePath();
    size_t pos = exePath.find_last_of("\\/");
    return (pos != std::string::npos) ? exePath.substr(0, pos) : exePath;
}

void setupFileLogger(const std::string &name = "agent.log")
{
    std::string logPath = getExeDir() + "\\" + name;
    g_logger.open(logPath);
}

void log(const char *msg)
{
    auto now = std::chrono::system_clock::now();
    auto time = std::chrono::system_clock::to_time_t(now);
    struct tm tmTemp;
    localtime_s(&tmTemp, &time);
    char timeStr[32];
    strftime(timeStr, sizeof(timeStr), "%Y-%m-%d %H:%M:%S", &tmTemp);
    std::string line = std::string(timeStr) + " [pid=" +
                       std::to_string(GetCurrentProcessId()) + "] " + msg + "\n";
    std::cout << line;
    g_logger.log(line);
}

void logf(const char *fmt, ...)
{
    char buf[2048];
    va_list args;
    va_start(args, fmt);
    vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);
    log(buf);
}

void log(const std::string &msg) { log(msg.c_str()); }

bool parseUrl(const std::string &url, std::string &host, int &port,
              std::string &path, std::string &query)
{
    host.clear();
    port = 80;
    path = "/";
    query.clear();
    std::string u = url;
    if (u.find("http://") == 0)
        u = u.substr(7);
    else if (u.find("https://") == 0)
    {
        u = u.substr(8);
        port = 443;
    }

    size_t pathPos = u.find('/');
    size_t queryPos = u.find('?');
    std::string hostPort;
    if (pathPos != std::string::npos)
    {
        hostPort = u.substr(0, pathPos);
        if (queryPos != std::string::npos && queryPos > pathPos)
        {
            path = u.substr(pathPos, queryPos - pathPos);
            query = u.substr(queryPos);
        }
        else
            path = u.substr(pathPos);
    }
    else
        hostPort = u;

    size_t colonPos = hostPort.find(':');
    if (colonPos != std::string::npos)
    {
        host = hostPort.substr(0, colonPos);
        port = std::stoi(hostPort.substr(colonPos + 1));
    }
    else
        host = hostPort;
    return !host.empty();
}

std::wstring utf8_to_wide(const std::string &utf8)
{
    if (utf8.empty()) return std::wstring();
    int wideLen = MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(), (int)utf8.size(), NULL, 0);
    if (wideLen <= 0) return std::wstring();
    std::wstring wide(wideLen, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(), (int)utf8.size(), &wide[0], wideLen);
    return wide;
}

// ======================== SERVICE ========================

SERVICE_STATUS g_serviceStatus = {0};
SERVICE_STATUS_HANDLE g_serviceHandle = NULL;
HANDLE g_serviceStopEvent = NULL;

VOID WINAPI serviceCtrlHandler(DWORD ctrlCode)
{
    if (ctrlCode == SERVICE_CONTROL_STOP)
    {
        log("Service stop requested");
        g_serviceStatus.dwCurrentState = SERVICE_STOP_PENDING;
        SetServiceStatus(g_serviceHandle, &g_serviceStatus);
        g_stopRequested = true;
        if (g_serviceStopEvent)
            SetEvent(g_serviceStopEvent);
    }
}

bool installService()
{
    std::string exePath = getExePath();
    SC_HANDLE scm = OpenSCManager(NULL, NULL, SC_MANAGER_CREATE_SERVICE);
    if (!scm)
    {
        log("installService: OpenSCManager failed");
        return false;
    }
    SC_HANDLE svc = CreateServiceA(
        scm, "RTSPDesktopAgent", "RTSP Desktop Agent",
        SERVICE_ALL_ACCESS, SERVICE_WIN32_OWN_PROCESS,
        SERVICE_AUTO_START, SERVICE_ERROR_NORMAL,
        exePath.c_str(), NULL, NULL, NULL, NULL, NULL);
    if (!svc)
    {
        DWORD err = GetLastError();
        if (err == ERROR_SERVICE_EXISTS || err == ERROR_DUP_NAME)
        {
            log("installService: service already exists");
            CloseServiceHandle(scm);
            return true;
        }
        logf("installService: CreateService failed (err=%lu)", err);
        CloseServiceHandle(scm);
        return false;
    }
    SERVICE_FAILURE_ACTIONS actions = {0};
    SC_ACTION action = {SC_ACTION_RESTART, 1000};
    actions.cActions = 1;
    actions.lpsaActions = &action;
    actions.dwResetPeriod = 86400;
    ChangeServiceConfig2A(svc, SERVICE_CONFIG_FAILURE_ACTIONS, &actions);
    CloseServiceHandle(svc);
    CloseServiceHandle(scm);
    log("installService: service created successfully");
    return true;
}

bool isServiceInstalled()
{
    SC_HANDLE scm = OpenSCManager(NULL, NULL, SC_MANAGER_ENUMERATE_SERVICE);
    if (!scm)
        return false;
    SC_HANDLE svc = OpenServiceA(scm, "RTSPDesktopAgent", SERVICE_QUERY_CONFIG);
    bool exists = (svc != NULL);
    if (svc)
        CloseServiceHandle(svc);
    CloseServiceHandle(scm);
    return exists;
}

// ======================== USER SESSION TOKEN ========================

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

static HANDLE GetSessionTokenFromExplorer(DWORD sessionId)
{
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE)
        return NULL;

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
        if (!hProcess)
            continue;

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
    if (sessionId == 0xFFFFFFFF)
        return NULL;

    return GetSessionToken(sessionId);
}

// ======================== WORKER MANAGEMENT ========================

PROCESS_INFORMATION g_worker_pi = {0};
std::mutex g_worker_m;
std::string g_worker_server;
std::string g_agent_uuid;
std::string g_agent_token;

bool spawnStreamingWorker()
{
    std::lock_guard<std::mutex> lk(g_worker_m);
    if (g_worker_pi.hProcess)
    {
        DWORD ec = 0;
        if (GetExitCodeProcess(g_worker_pi.hProcess, &ec) && ec != STILL_ACTIVE)
        {
            CloseHandle(g_worker_pi.hProcess);
            CloseHandle(g_worker_pi.hThread);
            g_worker_pi = {0};
        }
        else
        {
            log("[worker] already running");
            return true;
        }
    }

    std::string selfPath = getExePath();
    std::ostringstream args;
    args << "\"" << selfPath << "\""
         << " --streaming-worker"
         << " --server=" << g_worker_server
         << " --id=" << g_agent_uuid
         << " --token=" << g_agent_token;
    std::string cmdline = args.str();

    HANDLE hUserToken = GetActiveUserToken();
    if (!hUserToken)
    {
        log("[worker] no active user session, will retry later");
        return false;
    }

    LPVOID envBlock = NULL;
    if (!CreateEnvironmentBlock(&envBlock, hUserToken, FALSE))
        envBlock = NULL;

    STARTUPINFOA si = {0};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;
    si.lpDesktop = (LPSTR)"winsta0\\default";

    std::vector<char> buf(cmdline.begin(), cmdline.end());
    buf.push_back(0);

    PROCESS_INFORMATION pi = {0};
    BOOL ok = CreateProcessAsUserA(
        hUserToken, NULL, buf.data(),
        NULL, NULL, FALSE,
        CREATE_UNICODE_ENVIRONMENT | CREATE_NO_WINDOW,
        envBlock, NULL, &si, &pi);

    if (envBlock)
        DestroyEnvironmentBlock(envBlock);
    CloseHandle(hUserToken);

    if (!ok)
    {
        logf("[worker] CreateProcessAsUserA failed (err=%lu)", GetLastError());
        return false;
    }

    g_worker_pi = pi;
    logf("[worker] streaming worker started, pid=%lu", pi.dwProcessId);
    return true;
}

void stopStreamingWorker()
{
    std::lock_guard<std::mutex> lk(g_worker_m);
    if (!g_worker_pi.hProcess)
        return;
    log("[worker] stopping streaming worker...");
    TerminateProcess(g_worker_pi.hProcess, 0);
    CloseHandle(g_worker_pi.hProcess);
    CloseHandle(g_worker_pi.hThread);
    g_worker_pi = {0};
    log("[worker] streaming worker stopped");
}

// ======================== HTTP CLIENT (WinHTTP) ========================

std::string ansiToUtf8(const std::string &ansi)
{
    if (ansi.empty()) return ansi;
    int wideLen = MultiByteToWideChar(CP_ACP, 0, ansi.c_str(), (int)ansi.size(), NULL, 0);
    if (wideLen <= 0) return ansi;
    std::wstring wide(wideLen, L'\0');
    MultiByteToWideChar(CP_ACP, 0, ansi.c_str(), (int)ansi.size(), &wide[0], wideLen);
    int utf8Len = WideCharToMultiByte(CP_UTF8, 0, wide.c_str(), wideLen, NULL, 0, NULL, NULL);
    if (utf8Len <= 0) return ansi;
    std::string utf8(utf8Len, '\0');
    WideCharToMultiByte(CP_UTF8, 0, wide.c_str(), wideLen, &utf8[0], utf8Len, NULL, NULL);
    return utf8;
}

bool postJSON(const std::string &url, const std::string &bodyStr,
              std::string &responseBody, int &statusCode)
{
    std::string host, path, query;
    int port;
    if (!parseUrl(url, host, port, path, query))
        return false;
    std::string fullPath = path + query;

    HINTERNET hSession = WinHttpOpen(L"RTSP_Agent/1.0", WINHTTP_ACCESS_TYPE_NO_PROXY, NULL, NULL, 0);
    if (!hSession)
        return false;
    std::wstring whost(host.begin(), host.end());
    HINTERNET hConnect = WinHttpConnect(hSession, whost.c_str(), port, 0);
    if (!hConnect)
    {
        WinHttpCloseHandle(hSession);
        return false;
    }
    std::wstring wpath(fullPath.begin(), fullPath.end());
    DWORD dwFlags = 0;
    if (url.find("https://") == 0)
        dwFlags |= WINHTTP_FLAG_SECURE;
    HINTERNET hRequest = WinHttpOpenRequest(hConnect, L"POST", wpath.c_str(), NULL, NULL, NULL, dwFlags);
    if (!hRequest)
    {
        WinHttpCloseHandle(hConnect);
        WinHttpCloseHandle(hSession);
        return false;
    }
    if (url.find("https://") == 0)
    {
        DWORD dwCertFlags = SECURITY_FLAG_IGNORE_UNKNOWN_CA |
                            SECURITY_FLAG_IGNORE_CERT_DATE_INVALID |
                            SECURITY_FLAG_IGNORE_CERT_CN_INVALID |
                            SECURITY_FLAG_IGNORE_CERT_WRONG_USAGE;
        WinHttpSetOption(hRequest, WINHTTP_OPTION_SECURITY_FLAGS, &dwCertFlags, sizeof(dwCertFlags));
    }
    std::wstring header = L"Content-Type: application/json\r\n";
    WinHttpAddRequestHeaders(hRequest, header.c_str(), (DWORD)header.size(), WINHTTP_ADDREQ_FLAG_ADD);
    if (!WinHttpSendRequest(hRequest, WINHTTP_NO_ADDITIONAL_HEADERS, 0,
                            (LPVOID)bodyStr.c_str(), (DWORD)bodyStr.size(),
                            (DWORD)bodyStr.size(), 0) ||
        !WinHttpReceiveResponse(hRequest, NULL))
    {
        WinHttpCloseHandle(hRequest);
        WinHttpCloseHandle(hConnect);
        WinHttpCloseHandle(hSession);
        return false;
    }
    DWORD dwStatusCode = 0;
    DWORD dwSize = sizeof(dwStatusCode);
    if (WinHttpQueryHeaders(hRequest, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                            NULL, &dwStatusCode, &dwSize, NULL))
        statusCode = (int)dwStatusCode;
    else
        statusCode = 0;
    char buffer[4096] = {0};
    DWORD bytesRead = 0;
    std::string response;
    while (WinHttpReadData(hRequest, buffer, sizeof(buffer) - 1, &bytesRead) && bytesRead > 0)
    {
        buffer[bytesRead] = 0;
        response += buffer;
    }
    responseBody = response;
    WinHttpCloseHandle(hRequest);
    WinHttpCloseHandle(hConnect);
    WinHttpCloseHandle(hSession);
    return true;
}

// ======================== WEB SOCKET CONTROL (WinHTTP) ========================

// Minimal JSON field extraction (no external dependency)
static bool json_extract_str(const std::string &body, const std::string &key, std::string &out)
{
    std::string needle = "\"" + key + "\":\"";
    size_t p = body.find(needle);
    if (p == std::string::npos)
        return false;
    p += needle.size();
    size_t e = body.find("\"", p);
    if (e == std::string::npos)
        return false;
    out = body.substr(p, e - p);
    return true;
}

// WinHTTP WebSocket wrapper
class WsConnection
{
    HINTERNET hSession = NULL;
    HINTERNET hConnect = NULL;
    HINTERNET hRequest = NULL;
    HINTERNET hWebSocket = NULL;

public:
    ~WsConnection() { Close(); }

    bool Connect(const std::string &host, int port, const std::string &path)
    {
        Close();

        hSession = WinHttpOpen(L"RTSP_Agent/1.0", WINHTTP_ACCESS_TYPE_NO_PROXY, NULL, NULL, 0);
        if (!hSession) return false;

        std::wstring whost(host.begin(), host.end());
        std::wstring wpath(path.begin(), path.end());

        hConnect = WinHttpConnect(hSession, whost.c_str(), port, 0);
        if (!hConnect) { Close(); return false; }

        DWORD flags = (port == 443) ? WINHTTP_FLAG_SECURE : 0;
        hRequest = WinHttpOpenRequest(hConnect, L"GET", wpath.c_str(), NULL, NULL, NULL, flags);
        if (!hRequest) { Close(); return false; }

        if (port == 443)
        {
            DWORD certFlags = SECURITY_FLAG_IGNORE_UNKNOWN_CA |
                              SECURITY_FLAG_IGNORE_CERT_DATE_INVALID |
                              SECURITY_FLAG_IGNORE_CERT_CN_INVALID |
                              SECURITY_FLAG_IGNORE_CERT_WRONG_USAGE;
            WinHttpSetOption(hRequest, WINHTTP_OPTION_SECURITY_FLAGS, &certFlags, sizeof(certFlags));
        }

        DWORD wsUpgrade = WINHTTP_OPTION_UPGRADE_TO_WEB_SOCKET;
        if (!WinHttpSetOption(hRequest, WINHTTP_OPTION_UPGRADE_TO_WEB_SOCKET, NULL, 0))
        {
            log("[ws] WebSocket upgrade option not supported");
            Close();
            return false;
        }

        std::wstring headers = L"Upgrade: websocket\r\nConnection: Upgrade\r\n";
        WinHttpAddRequestHeaders(hRequest, headers.c_str(), (DWORD)headers.size(), WINHTTP_ADDREQ_FLAG_ADD);

        if (!WinHttpSendRequest(hRequest, WINHTTP_NO_ADDITIONAL_HEADERS, 0, NULL, 0, 0, 0))
        {
            Close();
            return false;
        }

        if (!WinHttpReceiveResponse(hRequest, NULL))
        {
            Close();
            return false;
        }

        hWebSocket = WinHttpWebSocketCompleteUpgrade(hRequest, NULL);
        if (!hWebSocket)
        {
            logf("[ws] WebSocket upgrade failed (err=%lu)", GetLastError());
            Close();
            return false;
        }

        return true;
    }

    bool Send(const std::string &data)
    {
        if (!hWebSocket) return false;
        DWORD err = WinHttpWebSocketSend(hWebSocket, WINHTTP_WEB_SOCKET_UTF8_MESSAGE_BUFFER_TYPE,
                                         (PVOID)data.data(), (DWORD)data.size());
        if (err != NO_ERROR)
        {
            logf("[ws] send error (err=%lu)", err);
            return false;
        }
        return true;
    }

    bool Recv(std::string &out)
    {
        if (!hWebSocket) return false;
        out.clear();
        char buf[4096];
        WINHTTP_WEB_SOCKET_BUFFER_TYPE type;
        DWORD bytesRead = 0;

        DWORD timeout = 1000;
        WinHttpSetOption(hWebSocket, WINHTTP_OPTION_RECEIVE_TIMEOUT, &timeout, sizeof(timeout));

        DWORD err = WinHttpWebSocketReceive(hWebSocket, buf, sizeof(buf) - 1, &bytesRead, &type);

        timeout = 30000;
        WinHttpSetOption(hWebSocket, WINHTTP_OPTION_RECEIVE_TIMEOUT, &timeout, sizeof(timeout));

        if (err == ERROR_WINHTTP_TIMEOUT || err == ERROR_WINHTTP_OPERATION_CANCELLED)
            return false;
        if (err != NO_ERROR)
        {
            logf("[ws] recv error (err=%lu)", err);
            return false;
        }
        if (bytesRead > 0)
        {
            buf[bytesRead] = 0;
            out.assign(buf, bytesRead);
        }
        return true;
    }

    bool IsConnected() const { return hWebSocket != NULL; }

    void Close()
    {
        if (hWebSocket)
        {
            WinHttpWebSocketClose(hWebSocket, WINHTTP_WEB_SOCKET_SUCCESS_CLOSE_STATUS, NULL, 0);
            WinHttpCloseHandle(hWebSocket);
            hWebSocket = NULL;
        }
        if (hRequest) { WinHttpCloseHandle(hRequest); hRequest = NULL; }
        if (hConnect) { WinHttpCloseHandle(hConnect); hConnect = NULL; }
        if (hSession) { WinHttpCloseHandle(hSession); hSession = NULL; }
    }
};

// ======================== CONTROL CONNECTION LOOP ========================

static std::string g_machine_uid;

std::string loadOrCreateMachineUID()
{
    std::string uidPath = getExeDir() + "\\machine_uid";
    std::ifstream ifs(uidPath);
    if (ifs.good())
    {
        std::string uid;
        std::getline(ifs, uid);
        if (!uid.empty())
            return uid;
    }
    std::random_device rd;
    std::mt19937 gen(rd());
    std::uniform_int_distribution<> dist(0, 999999);
    std::ostringstream oss;
    oss << time(nullptr) << "-" << GetCurrentProcessId() << "-" << dist(gen);
    std::string uid = oss.str();
    std::ofstream of(uidPath);
    of << uid;
    of.close();
    return uid;
}

static bool enable_debug_privilege()
{
    HANDLE hToken;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_ADJUST_PRIVILEGES | TOKEN_QUERY, &hToken))
        return false;
    TOKEN_PRIVILEGES tp;
    LUID luid;
    if (!LookupPrivilegeValueA(NULL, "SeDebugPrivilege", &luid))
    {
        CloseHandle(hToken);
        return false;
    }
    tp.PrivilegeCount = 1;
    tp.Privileges[0].Luid = luid;
    tp.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED;
    if (!AdjustTokenPrivileges(hToken, FALSE, &tp, sizeof(tp), NULL, NULL))
    {
        CloseHandle(hToken);
        return false;
    }
    CloseHandle(hToken);
    return true;
}

void controlCommandLoop()
{
    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0)
        return;

    std::string path_prefix = "/relay/ws/control/agent/";

    while (!g_stopRequested)
    {
        {
            std::lock_guard<std::mutex> lk(g_worker_m);
            if (g_worker_pi.hProcess)
            {
                DWORD ec = 0;
                if (GetExitCodeProcess(g_worker_pi.hProcess, &ec) && ec != STILL_ACTIVE)
                {
                    log("[control] worker process exited, cleaning up");
                    CloseHandle(g_worker_pi.hProcess);
                    CloseHandle(g_worker_pi.hThread);
                    g_worker_pi = {0};
                }
            }
        }

        WsConnection ws;
        std::string ws_host;
        int ws_port;
        std::string ws_path_prefix, ws_query;
        parseUrl(g_server_url, ws_host, ws_port, ws_path_prefix, ws_query);

        std::string ws_path = path_prefix + g_agent_uuid + "?token=" + g_agent_token;

        if (!ws.Connect(ws_host, ws_port, ws_path))
        {
            log("[control] WebSocket connect failed, retry in 3s");
            for (int i = 0; i < 3 && !g_stopRequested; i++)
                std::this_thread::sleep_for(std::chrono::seconds(1));
            continue;
        }
        log("[control] control WebSocket connected");

        auto lastPing = std::chrono::steady_clock::now();
        auto lastPong = std::chrono::steady_clock::now();

        while (!g_stopRequested && ws.IsConnected())
        {
            auto now = std::chrono::steady_clock::now();

            auto sincePong = std::chrono::duration_cast<std::chrono::seconds>(now - lastPong).count();
            if (sincePong >= 30)
            {
                log("[control] no pong for 30s, reconnecting");
                break;
            }

            auto sincePing = std::chrono::duration_cast<std::chrono::seconds>(now - lastPing).count();
            if (sincePing >= 10)
            {
                std::string ping = "{\"type\":\"ping\",\"ts\":" + std::to_string(time(nullptr)) + "}";
                if (!ws.Send(ping))
                {
                    log("[control] ping send failed");
                    break;
                }
                lastPing = now;
            }

            std::string msg;
            if (ws.Recv(msg))
            {
                if (msg.empty())
                    continue;

                std::string type;
                json_extract_str(msg, "type", type);

                if (type == "pong")
                {
                    lastPong = std::chrono::steady_clock::now();
                }
                else if (type == "command")
                {
                    std::string cmd;
                    json_extract_str(msg, "cmd", cmd);

                    if (cmd == "start-streaming")
                    {
                        json_extract_str(msg, "server", g_worker_server);
                        logf("[control] command: start-streaming (server=%s)", g_worker_server.c_str());
                        if (!spawnStreamingWorker())
                        {
                            log("[control] spawnStreamingWorker failed (no user session?)");
                        }
                    }
                    else if (cmd == "stop-streaming")
                    {
                        log("[control] command: stop-streaming");
                        stopStreamingWorker();
                    }
                }
            }
        }

        ws.Close();
        log("[control] WebSocket disconnected, reconnecting in 2s...");
        for (int i = 0; i < 2 && !g_stopRequested; i++)
            std::this_thread::sleep_for(std::chrono::seconds(1));
    }

    WSACleanup();
}

// ======================== RTSP CLIENT (PUSH) ========================

class RtspClient
{
public:
    ~RtspClient() { Disconnect(); }

    bool HandshakeDone() const { return handshake_done_; }

    bool Connect(const std::string &host, int port)
    {
        Disconnect();

        SOCKET s = socket(AF_INET, SOCK_STREAM, 0);
        if (s == INVALID_SOCKET)
            return false;

        struct sockaddr_in addr;
        addr.sin_family = AF_INET;
        addr.sin_port = htons((short)port);
        if (inet_pton(AF_INET, host.c_str(), &addr.sin_addr) != 1)
        {
            closesocket(s);
            return false;
        }

        if (connect(s, (struct sockaddr*)&addr, sizeof(addr)) != 0)
        {
            closesocket(s);
            return false;
        }

        sock_ = s;
        int one = 1;
        setsockopt(sock_, IPPROTO_TCP, TCP_NODELAY, (const char*)&one, sizeof(one));
        logf("[rtsp] TCP connected to %s:%d", host.c_str(), port);
        return true;
    }

    void Disconnect()
    {
        handshake_done_ = false;
        if (sock_ != INVALID_SOCKET)
        {
            closesocket(sock_);
            sock_ = INVALID_SOCKET;
        }
    }

    bool Handshake()
    {
        send_req("OPTIONS * RTSP/1.0\r\nCSeq: 1\r\n\r\n");
        if (!recv_resp()) return false;

        send_req("DESCRIBE rtsp://relay/stream RTSP/1.0\r\nCSeq: 2\r\nAccept: application/sdp\r\n\r\n");
        if (!recv_resp()) return false;

        send_req("SETUP rtsp://relay/stream/trackID=0 RTSP/1.0\r\n"
                 "CSeq: 3\r\nTransport: RTP/AVP/TCP;interleaved=0-1\r\n\r\n");
        if (!recv_resp()) return false;

        send_req("PLAY rtsp://relay/stream RTSP/1.0\r\nCSeq: 4\r\nSession: 12345678\r\n\r\n");
        if (!recv_resp()) return false;

        handshake_done_ = true;
        log("[rtsp] RTSP handshake OK | RTP TCP interleaved");
        return true;
    }

    bool SendRtp(const uint8_t *data, size_t size)
    {
        if (sock_ == INVALID_SOCKET) return false;
        uint8_t header[4];
        header[0] = '$';
        header[1] = 0;
        header[2] = (uint8_t)((size >> 8) & 0xFF);
        header[3] = (uint8_t)(size & 0xFF);
        if (!send_all(header, 4)) return false;
        return send_all(data, size);
    }

    bool KeyframeRequested() const { return keyframe_requested_; }
    void ClearKeyframeFlag() { keyframe_requested_ = false; }

    void CheckForIncoming()
    {
        if (sock_ == INVALID_SOCKET) return;
        fd_set read_fds;
        FD_ZERO(&read_fds);
        FD_SET(sock_, &read_fds);
        struct timeval tv = {0, 0};
        if (select((int)sock_ + 1, &read_fds, nullptr, nullptr, &tv) > 0)
        {
            char buf[64];
            int n = (int)recv(sock_, buf, sizeof(buf) - 1, 0);
            if (n > 0)
            {
                buf[n] = 0;
                if (strstr(buf, "!K"))
                    keyframe_requested_ = true;
            }
        }
    }

private:
    SOCKET sock_ = INVALID_SOCKET;
    bool handshake_done_ = false;
    bool keyframe_requested_ = false;
    char recv_buf_[4096] = {};

    bool send_all(const uint8_t *buf, size_t len)
    {
        while (len > 0)
        {
            int r = (int)send(sock_, (const char*)buf, (int)len, 0);
            if (r <= 0) return false;
            buf += r;
            len -= (size_t)r;
        }
        return true;
    }

    void send_req(const std::string &req)
    {
        send(sock_, req.c_str(), (int)req.size(), 0);
    }

    bool recv_resp()
    {
        int n = (int)recv(sock_, recv_buf_, sizeof(recv_buf_) - 1, 0);
        if (n <= 0) return false;
        recv_buf_[n] = 0;
        return strstr(recv_buf_, "200 OK") != nullptr;
    }
};

// ======================== STREAMING WORKER ========================

static bool g_running = true;
void signal_handler(int)
{
    log("\nStopping...");
    g_running = false;
}

static bool extract_sps_pps(const std::vector<uint8_t> &nal_data,
                            std::vector<uint8_t> &sps,
                            std::vector<uint8_t> &pps)
{
    size_t i = 0;
    while (i < nal_data.size())
    {
        size_t sc_len = 0;
        if (i + 4 <= nal_data.size() && nal_data[i] == 0 && nal_data[i+1] == 0 && nal_data[i+2] == 0 && nal_data[i+3] == 1)
            sc_len = 4;
        else if (i + 3 <= nal_data.size() && nal_data[i] == 0 && nal_data[i+1] == 0 && nal_data[i+2] == 1)
            sc_len = 3;
        if (sc_len == 0) { i++; continue; }

        size_t nal_start = i + sc_len;
        if (nal_start >= nal_data.size()) break;

        uint8_t nal_type = nal_data[nal_start] & 0x1F;
        size_t nal_end = nal_start;

        for (size_t j = nal_start + 1; j < nal_data.size(); j++)
        {
            if ((j + 4 <= nal_data.size() && nal_data[j] == 0 && nal_data[j+1] == 0 && nal_data[j+2] == 0 && nal_data[j+3] == 1) ||
                (j + 3 <= nal_data.size() && nal_data[j] == 0 && nal_data[j+1] == 0 && nal_data[j+2] == 1))
            {
                nal_end = j;
                break;
            }
            nal_end = nal_data.size();
        }

        if (nal_type == 7)
            sps.assign(nal_data.begin() + nal_start, nal_data.begin() + nal_end);
        else if (nal_type == 8)
            pps.assign(nal_data.begin() + nal_start, nal_data.begin() + nal_end);

        if (!sps.empty() && !pps.empty())
            return true;
        i = nal_end;
    }
    return !sps.empty() && !pps.empty();
}

int run_streaming_worker(const std::string &server_host, int server_port)
{
    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);

    timeBeginPeriod(1);

    // Init capture
    auto capture = std::make_unique<WGCCapture>();
    if (!capture->Initialize(15))
    {
        log("[worker] Capture init failed");
        timeEndPeriod(1);
        return 1;
    }

    int w = 0, h = 0, mx = 0, my = 0;
    capture->GetMonitorInfo(0, w, h, mx, my);
    logf("[worker] Monitor: %dx%d", w, h);

    // Init encoder
    auto encoder = std::make_unique<X264Encoder>();
    if (!encoder->Initialize(w, h, 15, 28))
    {
        log("[worker] Encoder init failed");
        capture->Shutdown();
        timeEndPeriod(1);
        return 1;
    }

    // Get SPS/PPS
    std::vector<uint8_t> sps = encoder->GetSps();
    std::vector<uint8_t> pps = encoder->GetPps();
    if (sps.empty() || pps.empty())
    {
        std::vector<uint8_t> bgra, first_nals;
        int fw = 0, fh = 0;
        for (int i = 0; i < 60 && g_running; i++)
        {
            if (capture->CaptureFrame(0, bgra, fw, fh))
            {
                first_nals.clear();
                if (encoder->EncodeFrame(bgra, first_nals) && !first_nals.empty())
                {
                    extract_sps_pps(first_nals, sps, pps);
                    break;
                }
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(16));
        }
    }
    logf("[worker] SPS: %zu PPS: %zu", sps.size(), pps.size());

    // Init packetizer
    H264RtpPacketizer packetizer;
    packetizer.SetMaxPayloadSize(8000);
    packetizer.SetSsrc(0xDEADBEEF);

    // Connect to relay server
    RtspClient rtsp_client;
    logf("[worker] Connecting to relay %s:%d", server_host.c_str(), server_port);
    if (!rtsp_client.Connect(server_host, server_port))
    {
        log("[worker] Failed to connect to relay");
        encoder->Shutdown();
        capture->Shutdown();
        timeEndPeriod(1);
        return 1;
    }

    // Main streaming loop
    std::vector<uint8_t> bgra, nals;
    std::vector<std::vector<uint8_t>> rtp_packets;
    int fw = 0, fh = 0;
    uint32_t rtp_ts = 0;
    const uint32_t rtp_ts_step = 90000 / 15;
    const auto frame_duration = std::chrono::nanoseconds(1000000000LL / 15);
    auto next_frame = std::chrono::steady_clock::now();
    int64_t frame_count = 0;
    bool handshake_ok = false;

    while (g_running)
    {
        if (!capture->CaptureFrame(0, bgra, fw, fh))
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
            continue;
        }

        nals.clear();
        if (!encoder->EncodeFrame(bgra, nals))
        {
            log("[worker] Encode error");
            break;
        }
        if (nals.empty())
            continue;

        packetizer.Packetize(nals.data(), nals.size(), rtp_ts, rtp_packets);
        rtp_ts += rtp_ts_step;

        // Connect and handshake on first frame
        if (!handshake_ok)
        {
            if (rtsp_client.Handshake())
            {
                handshake_ok = true;
            }
            else
            {
                log("[worker] RTSP handshake failed, retrying...");
                rtsp_client.Disconnect();
                std::this_thread::sleep_for(std::chrono::seconds(1));
                if (!rtsp_client.Connect(server_host, server_port))
                {
                    log("[worker] Reconnect failed");
                    break;
                }
                continue;
            }
        }

        rtsp_client.CheckForIncoming();
        if (rtsp_client.KeyframeRequested())
        {
            encoder->RequestKeyframe();
            rtsp_client.ClearKeyframeFlag();
        }

        for (const auto &pkt : rtp_packets)
        {
            if (!rtsp_client.SendRtp(pkt.data(), pkt.size()))
            {
                log("[worker] Relay disconnected, reconnecting...");
                rtsp_client.Disconnect();
                handshake_ok = false;
                std::this_thread::sleep_for(std::chrono::seconds(1));
                break;
            }
        }

        frame_count++;

        next_frame += frame_duration;
        {
            auto now = std::chrono::steady_clock::now();
            if (next_frame < now)
            {
                auto lag = std::chrono::duration_cast<std::chrono::milliseconds>(now - next_frame);
                if (lag.count() > 500)
                    next_frame = now;
            }
        }
        std::this_thread::sleep_until(next_frame);

        if (frame_count % 30 == 0)
        {
            logf("[worker] Frames: %lld (%zu bytes)", (long long)frame_count, nals.size());
        }
    }

    rtsp_client.Disconnect();
    encoder->Shutdown();
    capture->Shutdown();
    timeEndPeriod(1);
    logf("[worker] Done. %lld frames", (long long)frame_count);
    return 0;
}

// ======================== SERVICE MAIN ========================

VOID WINAPI serviceMain(DWORD argc, LPWSTR *argv)
{
    g_serviceStatus.dwServiceType = SERVICE_WIN32;
    g_serviceStatus.dwCurrentState = SERVICE_START_PENDING;
    g_serviceStatus.dwControlsAccepted = SERVICE_ACCEPT_STOP;
    g_serviceStatus.dwWin32ExitCode = 0;
    g_serviceStatus.dwServiceSpecificExitCode = 0;
    g_serviceHandle = RegisterServiceCtrlHandlerW(SERVICE_NAME, serviceCtrlHandler);
    if (!g_serviceHandle)
    {
        log("serviceMain: RegisterServiceCtrlHandlerW failed");
        return;
    }
    SetServiceStatus(g_serviceHandle, &g_serviceStatus);

    g_serviceStopEvent = CreateEvent(NULL, TRUE, FALSE, NULL);
    if (!g_serviceStopEvent)
    {
        g_serviceStatus.dwCurrentState = SERVICE_STOPPED;
        SetServiceStatus(g_serviceHandle, &g_serviceStatus);
        return;
    }

    g_serviceStatus.dwCurrentState = SERVICE_RUNNING;
    SetServiceStatus(g_serviceHandle, &g_serviceStatus);
    log("Service started");

    // ---- Init machine UID ----
    g_machine_uid = loadOrCreateMachineUID();

    // ---- Registration ----
    {
        char hostnameA[256];
        DWORD sz = sizeof(hostnameA);
        GetComputerNameA(hostnameA, &sz);
        std::string hostname = ansiToUtf8(std::string(hostnameA));

        std::string url = g_server_url + "/api/agent/register";
        std::string body = "{\"name_pc\":\"" + hostname +
                           "\",\"machine_uid\":\"" + g_machine_uid +
                           "\",\"exe_version\":\"" + g_build_slug + "\"}";
        std::string responseBody;
        int statusCode = 0;

        for (int retry = 0; retry < 60 && !g_stopRequested; retry++)
        {
            if (postJSON(url, body, responseBody, statusCode) && statusCode == 200)
            {
                json_extract_str(responseBody, "agent_uuid", g_agent_uuid);
                json_extract_str(responseBody, "token", g_agent_token);
                if (!g_agent_uuid.empty() && !g_agent_token.empty())
                {
                    logf("Registration OK: uuid=%s", g_agent_uuid.c_str());
                    break;
                }
            }
            log("Registration failed, retry in 2s...");
            for (int i = 0; i < 2 && !g_stopRequested; i++)
                Sleep(1000);
        }
    }

    if (g_stopRequested)
    {
        CloseHandle(g_serviceStopEvent);
        g_serviceStatus.dwCurrentState = SERVICE_STOPPED;
        SetServiceStatus(g_serviceHandle, &g_serviceStatus);
        return;
    }

    // ---- Enable debug privilege for session token access ----
    enable_debug_privilege();

    // ---- Control command loop (WebSocket) ----
    std::thread cmdThread(controlCommandLoop);

    // ---- Wait for stop ----
    WaitForSingleObject(g_serviceStopEvent, INFINITE);

    stopStreamingWorker();
    if (cmdThread.joinable())
        cmdThread.join();

    CloseHandle(g_serviceStopEvent);
    g_serviceStatus.dwCurrentState = SERVICE_STOPPED;
    SetServiceStatus(g_serviceHandle, &g_serviceStatus);
    log("Service stopped");
}

// ======================== MAIN ========================

int main(int argc, char *argv[])
{
    // Check for streaming worker mode
    for (int i = 1; i < argc; ++i)
    {
        if (strcmp(argv[i], "--streaming-worker") == 0)
        {
            std::string server, id, token;
            for (int j = i + 1; j < argc; ++j)
            {
                if (strncmp(argv[j], "--server=", 9) == 0)
                    server = argv[j] + 9;
                else if (strncmp(argv[j], "--id=", 5) == 0)
                    id = argv[j] + 5;
                else if (strncmp(argv[j], "--token=", 8) == 0)
                    token = argv[j] + 8;
            }
            setupFileLogger("agent_worker.log");
            log("=== Streaming worker started ===");

            int port = 8554;
            std::string host = server;
            size_t colon = server.find(':');
            if (colon != std::string::npos)
            {
                host = server.substr(0, colon);
                port = std::stoi(server.substr(colon + 1));
            }

            return run_streaming_worker(host, port);
        }
    }

    // Service mode
    setupFileLogger("agent.log");
    log("Agent started as console app");

    if (!isServiceInstalled())
    {
        if (installService())
        {
            SC_HANDLE scm = OpenSCManager(NULL, NULL, SC_MANAGER_CONNECT);
            if (scm)
            {
                SC_HANDLE svc = OpenServiceA(scm, "RTSPDesktopAgent", SERVICE_START);
                if (svc)
                {
                    StartServiceA(svc, 0, NULL);
                    CloseServiceHandle(svc);
                }
                CloseServiceHandle(scm);
            }
        }
        return 0;
    }

    SERVICE_TABLE_ENTRYW table[] = {
        {(LPWSTR)SERVICE_NAME, serviceMain},
        {NULL, NULL}};
    log("Starting service dispatcher...");
    StartServiceCtrlDispatcherW(table);
    log("Service dispatcher exited");
    return 0;
}
