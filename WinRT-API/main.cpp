// builder_cpp/agent/cmd/agent/main.cpp
// Агент: Windows-сервис с регистрацией/телеметрией/апдейтом.
// RDP-часть вынесена в отдельный процесс-worker, запускаемый в сессии
// пользователя через CreateProcessAsUserA (флаг --rdp-worker).
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

#pragma comment(lib, "winhttp.lib")
#pragma comment(lib, "advapi32.lib")
#pragma comment(lib, "ws2_32.lib")
#pragma comment(lib, "crypt32.lib")
#pragma comment(lib, "wtsapi32.lib")
#pragma comment(lib, "userenv.lib")
#pragma comment(lib, "netapi32.lib")
#pragma comment(lib, "iphlpapi.lib")

#include <lm.h>
#include <sddl.h>
#include "rdp_agent.h"
#include "encoder_amf.h"
#include "encoder_ave.h"
#include "encoder_qsv.h"

// Redis C++ library (hiredis) - optional, for Pub/Sub command delivery
#ifdef HAVE_REDIS
    #include "hiredis.h"
#endif

#ifndef SERVER_URL
#define SERVER_URL "http://localhost:8000"
#endif
#ifndef BUILD_SLUG
#define BUILD_SLUG "1.0.0"
#endif

std::string serverURL = SERVER_URL;
std::string buildSlug = BUILD_SLUG;

// AsyncLogger: очередь сообщений + фоновый поток записи в файл
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
std::string g_agent_uuid;
std::string g_agent_token;
std::atomic<int> g_rdp_worker_timeout{5};
std::string g_rdp_worker_codec;
std::string g_rdp_worker_encoder;
std::string g_rdp_worker_quality;
int g_rdp_worker_fps = 0;

// Async telemetry response buffer (written by background thread, read by control loop)
static std::mutex g_pending_telemetry_m;
static std::string g_pending_telemetry_resp;

// Shared memory for activity monitoring (created by SYSTEM process, read by worker)
static HANDLE g_shm_handle = NULL;
static ActivityShm *g_shm = NULL;
static std::string g_shm_name;
static std::mutex g_shm_m;

SERVICE_STATUS serviceStatus = {0};
SERVICE_STATUS_HANDLE serviceHandle = NULL;
HANDLE stopEvent = NULL;

// ==================== LOGGER ====================

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

// ==================== URL PARSE ====================

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

// ==================== MACHINE UID ====================

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

// ==================== NETWORK ====================

std::string getAllLocalIPs()
{
    std::vector<std::string> ips;
    ULONG bufLen = 15000;
    PIP_ADAPTER_ADDRESSES pAddresses = (PIP_ADAPTER_ADDRESSES)malloc(bufLen);
    if (!pAddresses)
        return "";

    DWORD ret = GetAdaptersAddresses(AF_INET, GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST | GAA_FLAG_SKIP_DNS_SERVER, NULL, pAddresses, &bufLen);
    if (ret == ERROR_BUFFER_OVERFLOW)
    {
        free(pAddresses);
        pAddresses = (PIP_ADAPTER_ADDRESSES)malloc(bufLen);
        if (!pAddresses)
            return "";
        ret = GetAdaptersAddresses(AF_INET, GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST | GAA_FLAG_SKIP_DNS_SERVER, NULL, pAddresses, &bufLen);
    }

    if (ret == NO_ERROR)
    {
        for (PIP_ADAPTER_ADDRESSES pAdapter = pAddresses; pAdapter; pAdapter = pAdapter->Next)
        {
            // Skip loopback, tunnel, and teredo adapters
            if (pAdapter->IfType == IF_TYPE_SOFTWARE_LOOPBACK)
                continue;
            if (pAdapter->TunnelType == TUNNEL_TYPE_TEREDO)
                continue;

            for (PIP_ADAPTER_UNICAST_ADDRESS pAddr = pAdapter->FirstUnicastAddress; pAddr; pAddr = pAddr->Next)
            {
                SOCKADDR *sa = pAddr->Address.lpSockaddr;
                if (sa->sa_family == AF_INET)
                {
                    struct sockaddr_in *sa_in = (struct sockaddr_in *)sa;
                    char ip[INET_ADDRSTRLEN];
                    inet_ntop(AF_INET, &sa_in->sin_addr, ip, sizeof(ip));
                    if (strncmp(ip, "127.", 4) != 0)
                        ips.push_back(std::string(ip));
                }
            }
        }
    }

    free(pAddresses);

    std::string result;
    for (size_t i = 0; i < ips.size(); i++)
    {
        if (i > 0) result += ", ";
        result += ips[i];
    }
    return result;
}

std::string getExternalIP()
{
    HINTERNET hSession = WinHttpOpen(L"Agent/1.0", WINHTTP_ACCESS_TYPE_NO_PROXY, NULL, NULL, 0);
    if (!hSession)
        return "";
    HINTERNET hConnect = WinHttpConnect(hSession, L"api.ipify.org", INTERNET_DEFAULT_HTTP_PORT, 0);
    if (!hConnect)
    {
        WinHttpCloseHandle(hSession);
        return "";
    }
    HINTERNET hRequest = WinHttpOpenRequest(hConnect, L"GET", NULL, NULL, NULL, NULL, 0);
    if (!hRequest)
    {
        WinHttpCloseHandle(hConnect);
        WinHttpCloseHandle(hSession);
        return "";
    }
    if (!WinHttpSendRequest(hRequest, NULL, 0, NULL, 0, 0, 0) ||
        !WinHttpReceiveResponse(hRequest, NULL))
    {
        WinHttpCloseHandle(hRequest);
        WinHttpCloseHandle(hConnect);
        WinHttpCloseHandle(hSession);
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
    WinHttpCloseHandle(hRequest);
    WinHttpCloseHandle(hConnect);
    WinHttpCloseHandle(hSession);
    return result;
}

std::vector<std::string> getLocalUsersFromNetAPI()
{
    std::vector<std::string> users;

    LPBYTE buffer = NULL;
    DWORD entriesRead = 0;
    DWORD totalEntries = 0;
    DWORD resumeHandle = 0;

    NET_API_STATUS status = NetUserEnum(
        NULL,                      // local server
        3,                         // level 3 — flags (for disabled check)
        FILTER_NORMAL_ACCOUNT,     // normal user accounts only
        &buffer,
        MAX_PREFERRED_LENGTH,
        &entriesRead,
        &totalEntries,
        &resumeHandle
    );

    if (status == NERR_Success && buffer != NULL)
    {
        USER_INFO_3 *users3 = reinterpret_cast<USER_INFO_3 *>(buffer);
        for (DWORD i = 0; i < entriesRead; i++)
        {
            if (users3[i].usri3_name == NULL)
                continue;

            // Skip disabled accounts
            if (users3[i].usri3_flags & UF_ACCOUNTDISABLE)
                continue;

            int utf8Len = WideCharToMultiByte(CP_UTF8, 0,
                users3[i].usri3_name, -1, NULL, 0, NULL, NULL);
            if (utf8Len > 1)
            {
                std::string name(utf8Len - 1, '\0');
                WideCharToMultiByte(CP_UTF8, 0,
                    users3[i].usri3_name, -1, &name[0], utf8Len, NULL, NULL);

                std::string lower = name;
                std::transform(lower.begin(), lower.end(), lower.begin(), ::tolower);
                if (lower != "administrator" && lower != "guest" &&
                    lower != "defaultuser0" && lower != "defaultaccount")
                {
                    users.push_back(name);
                }
            }
        }
        NetApiBufferFree(buffer);
    }

    return users;
}

std::string ansiToUtf8(const std::string &ansi);
static bool is_session_locked(DWORD targetSessionId);
static DWORD FindActiveUserSessionId();

static bool is_excluded_user(const std::string& lowerName)
{
    return lowerName == "administrator" || lowerName == "guest" ||
           lowerName == "defaultuser0" || lowerName == "defaultaccount";
}

static bool is_account_disabled(const std::string& utf8name)
{
    // Extract just the username part (after DOMAIN\ if present)
    std::string name = utf8name;
    size_t pos = name.find('\\');
    if (pos != std::string::npos)
        name = name.substr(pos + 1);

    int wideLen = MultiByteToWideChar(CP_UTF8, 0, name.c_str(), -1, NULL, 0);
    if (wideLen <= 0) return false;

    std::wstring wname(wideLen, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, name.c_str(), -1, &wname[0], wideLen);

    LPBYTE buffer = NULL;
    NET_API_STATUS status = NetUserGetInfo(NULL, wname.c_str(), 3, &buffer);

    if (status == NERR_Success && buffer != NULL)
    {
        USER_INFO_3 *info = reinterpret_cast<USER_INFO_3 *>(buffer);
        bool disabled = (info->usri3_flags & UF_ACCOUNTDISABLE) != 0;
        NetApiBufferFree(buffer);
        return disabled;
    }
    return false;
}

std::string getUsersAsString()
{
    // Map: lowercase username -> display name (possibly with markers)
    std::unordered_map<std::string, std::string> usersMap;

    // Get local computer name to distinguish local vs domain users
    char localComputerName[MAX_COMPUTERNAME_LENGTH + 1] = {0};
    DWORD localNameLen = sizeof(localComputerName);
    GetComputerNameExA(ComputerNameNetBIOS, localComputerName, &localNameLen);
    std::string localPc = localComputerName;
    std::transform(localPc.begin(), localPc.end(), localPc.begin(), ::tolower);

    // ========== 1. First, enumerate ALL WTS sessions (for terminal server support)
    PWTS_SESSION_INFOW pSessionInfo = NULL;
    DWORD sessionCount = 0;

    if (WTSEnumerateSessionsW(WTS_CURRENT_SERVER_HANDLE, 0, 1, &pSessionInfo, &sessionCount))
    {
        for (DWORD i = 0; i < sessionCount; i++)
        {
            DWORD sessionId = pSessionInfo[i].SessionId;
            if (sessionId == 0)  // skip session 0 (system)
                continue;

            // Get username for this session
            LPWSTR pUserName = NULL;
            DWORD userNameSize = 0;
            std::string userName;

            if (WTSQuerySessionInformationW(WTS_CURRENT_SERVER_HANDLE, sessionId,
                                             WTSUserName, &pUserName, &userNameSize))
            {
                if (pUserName && userNameSize > 2 && pUserName[0] != 0)
                {
                    int len = WideCharToMultiByte(CP_UTF8, 0, pUserName, -1, nullptr, 0, nullptr, nullptr);
                    if (len > 0)
                    {
                        userName.resize(len - 1);
                        WideCharToMultiByte(CP_UTF8, 0, pUserName, -1, &userName[0], len, nullptr, nullptr);
                    }
                }
                WTSFreeMemory(pUserName);
            }

            if (userName.empty())
                continue;

            // Get domain for this session
            LPWSTR pDomainName = NULL;
            DWORD domainNameSize = 0;
            std::string domainName;

            if (WTSQuerySessionInformationW(WTS_CURRENT_SERVER_HANDLE, sessionId,
                                             WTSDomainName, &pDomainName, &domainNameSize))
            {
                if (pDomainName && domainNameSize > 2 && pDomainName[0] != 0)
                {
                    int len = WideCharToMultiByte(CP_UTF8, 0, pDomainName, -1, nullptr, 0, nullptr, nullptr);
                    if (len > 0)
                    {
                        domainName.resize(len - 1);
                        WideCharToMultiByte(CP_UTF8, 0, pDomainName, -1, &domainName[0], len, nullptr, nullptr);
                    }
                }
                WTSFreeMemory(pDomainName);
            }

            // Build full name: DOMAIN\username for domain users, just username for local
            std::string fullUserName = userName;
            std::string lowerDomain;
            if (!domainName.empty())
            {
                lowerDomain = domainName;
                std::transform(lowerDomain.begin(), lowerDomain.end(), lowerDomain.begin(), ::tolower);
                if (lowerDomain != localPc)
                    fullUserName = domainName + "\\" + userName;
            }

            std::string lowerName = fullUserName;
            std::transform(lowerName.begin(), lowerName.end(), lowerName.begin(), ::tolower);

            if (is_excluded_user(lowerName))
                continue;

            // Determine marker based on session state and lock status
            std::string displayName = fullUserName;

            if (pSessionInfo[i].State == WTSActive)
            {
                // Active session - check if locked
                if (is_session_locked(sessionId))
                {
                    displayName = "|" + fullUserName + "|";  // LOCKED
                }
                else
                {
                    displayName = "*" + fullUserName + "*";  // ACTIVE
                }
            }
            else
            {
                // Disconnected or other state - no marker
            }

            // Only add if not already present (or if existing has no marker)
            auto existing = usersMap.find(lowerName);
            if (existing == usersMap.end())
            {
                usersMap[lowerName] = displayName;
            }
            else
            {
                // If already exists - prefer the one with markers (active session)
                if (existing->second == fullUserName && displayName != fullUserName)
                {
                    usersMap[lowerName] = displayName;
                }
            }
        }
        WTSFreeMemory(pSessionInfo);
    }

    // ========== 2. Add users from ProfileList (users with profiles but no active session)
    HKEY hKey = NULL;
    LONG regResult = RegOpenKeyExA(
        HKEY_LOCAL_MACHINE,
        "SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion\\ProfileList",
        0, KEY_READ, &hKey);

    if (regResult == ERROR_SUCCESS)
    {
        char subKeyName[256];
        DWORD subKeySize;
        DWORD index = 0;

        while (true)
        {
            subKeySize = sizeof(subKeyName);
            regResult = RegEnumKeyExA(hKey, index++, subKeyName, &subKeySize,
                                       NULL, NULL, NULL, NULL);
            if (regResult != ERROR_SUCCESS)
                break;

            std::string sidStr(subKeyName, subKeySize);
            if (sidStr == "S-1-5-18" || sidStr == "S-1-5-19" || sidStr == "S-1-5-20")
                continue;

            PSID pSid = NULL;
            if (!ConvertStringSidToSidA(sidStr.c_str(), &pSid))
                continue;

            char accountName[256] = {0};
            DWORD accountNameLen = sizeof(accountName);
            char domainName[256] = {0};
            DWORD domainNameLen = sizeof(domainName);
            SID_NAME_USE sidUse;

            if (LookupAccountSidA(NULL, pSid, accountName, &accountNameLen,
                                   domainName, &domainNameLen, &sidUse))
            {
                std::string userName = ansiToUtf8(std::string(accountName));
                std::string domain = ansiToUtf8(std::string(domainName));

                // Build full name: DOMAIN\username for domain users
                std::string fullUserName = userName;
                std::string lowerDomain = domain;
                std::transform(lowerDomain.begin(), lowerDomain.end(), lowerDomain.begin(), ::tolower);
                if (!domain.empty() && lowerDomain != localPc)
                    fullUserName = domain + "\\" + userName;

                std::string lowerName = fullUserName;
                std::transform(lowerName.begin(), lowerName.end(), lowerName.begin(), ::tolower);

                if (!is_excluded_user(lowerName))
                {
                    // Add only if not already in map from WTS sessions
                    if (usersMap.find(lowerName) == usersMap.end())
                    {
                        usersMap[lowerName] = fullUserName;
                    }
                }
            }

            LocalFree(pSid);
        }
        RegCloseKey(hKey);
    }

    // ========== 3. Add local SAM users (catches newly created users without profiles yet)
    std::vector<std::string> localUsers = getLocalUsersFromNetAPI();
    for (const auto &name : localUsers)
    {
        std::string lowerName = name;
        std::transform(lowerName.begin(), lowerName.end(), lowerName.begin(), ::tolower);

        if (!is_excluded_user(lowerName))
        {
            if (usersMap.find(lowerName) == usersMap.end())
            {
                usersMap[lowerName] = name;
            }
        }
    }

    // ========== 4. Remove disabled accounts
    for (auto it = usersMap.begin(); it != usersMap.end(); )
    {
        if (is_account_disabled(it->first))
        {
            it = usersMap.erase(it);
        }
        else
        {
            ++it;
        }
    }

    // ========== Build comma-separated result
    std::string result;
    for (const auto& pair : usersMap)
    {
        if (!result.empty())
            result += ", ";
        result += pair.second;
    }

    return result;
}

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

std::wstring utf8_to_wide(const std::string &utf8)
{
    if (utf8.empty()) return std::wstring();
    int wideLen = MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(), (int)utf8.size(), NULL, 0);
    if (wideLen <= 0) return std::wstring();
    std::wstring wide(wideLen, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(), (int)utf8.size(), &wide[0], wideLen);
    return wide;
}

std::string jsonEscape(const std::string &s)
{
    std::string out;
    for (char c : s)
    {
        switch (c)
        {
        case '\"':
            out += "\\\"";
            break;
        case '\\':
            out += "\\\\";
            break;
        case '\b':
            out += "\\b";
            break;
        case '\f':
            out += "\\f";
            break;
        case '\n':
            out += "\\n";
            break;
        case '\r':
            out += "\\r";
            break;
        case '\t':
            out += "\\t";
            break;
        default:
            if (static_cast<unsigned char>(c) < 0x20)
            {
                char buf[8];
                snprintf(buf, sizeof(buf), "\\u%04x", static_cast<unsigned char>(c));
                out += buf;
            }
            else
            {
                out += c;
            }
        }
    }
    return out;
}

// ==================== HTTP CLIENT ====================

bool postJSON(const std::string &url, const std::string &bodyStr,
              std::string &responseBody, int &statusCode)
{
    std::string host, path, query;
    int port;
    if (!parseUrl(url, host, port, path, query))
        return false;
    std::string fullPath = path + query;

    HINTERNET hSession = WinHttpOpen(L"Agent/1.0", WINHTTP_ACCESS_TYPE_NO_PROXY, NULL, NULL, 0);
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

bool getJSON(const std::string &url, std::string &responseBody, int &statusCode)
{
    std::string host, path, query;
    int port;
    if (!parseUrl(url, host, port, path, query))
        return false;
    std::string fullPath = path + query;

    HINTERNET hSession = WinHttpOpen(L"Agent/1.0", WINHTTP_ACCESS_TYPE_NO_PROXY, NULL, NULL, 0);
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
    HINTERNET hRequest = WinHttpOpenRequest(hConnect, L"GET", wpath.c_str(), NULL, NULL, NULL, dwFlags);
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
    if (!WinHttpSendRequest(hRequest, WINHTTP_NO_ADDITIONAL_HEADERS, 0, NULL, 0, 0, 0) ||
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

// ==================== TELEMETRY ====================

std::string detectGPU();

std::string getEncoderCapabilities()
{
    std::string gpu_vendor = detectGPU();
    bool amf_avail = AMFEncoder::IsAvailable();
    bool ave_avail = AVEEncoder::IsAvailable();
    bool nv_avail = false;  // NVENC — next iteration
    bool qsv_avail = QSVEncoder::IsAvailable();

    // Report AMF and AVE separately so the user can manually select which AMD encoder to use
    std::string result = "{";
    result += "\"libx264\":true,";
    result += "\"h264_amf\":" + std::string(amf_avail ? "true" : "false") + ",";
    result += "\"h264_ave\":" + std::string(ave_avail ? "true" : "false") + ",";
    result += "\"h264_qsv\":" + std::string(qsv_avail ? "true" : "false") + ",";
    result += "\"h264_nvenc\":" + std::string(nv_avail ? "true" : "false") + ",";
    result += "\"gpu\":\"" + gpu_vendor + "\",";
    result += "\"gpu_encoders\":{";
    result += "\"h264_amf\":" + std::string(amf_avail ? "true" : "false") + ",";
    result += "\"h264_ave\":" + std::string(ave_avail ? "true" : "false") + ",";
    result += "\"h264_qsv\":" + std::string(qsv_avail ? "true" : "false") + ",";
    result += "\"h264_nvenc\":" + std::string(nv_avail ? "true" : "false");
    result += "}}";
    return result;
}

std::string detectGPU()
{
    // Detect GPU vendor via SetupAPI (reliable, no WMI dependency)
    // {4d36e968-e325-11ce-bfc1-08002be10318} = display adapters class
    GUID displayClass = {0x4d36e968, 0xe325, 0x11ce, {0xbf, 0xc1, 0x08, 0x00, 0x2b, 0xe1, 0x03, 0x18}};
    HDEVINFO devInfo = SetupDiGetClassDevsA(&displayClass, NULL, NULL, DIGCF_PRESENT);
    if (devInfo == INVALID_HANDLE_VALUE)
        return "unknown";

    SP_DEVINFO_DATA devData;
    devData.cbSize = sizeof(SP_DEVINFO_DATA);
    std::string result = "unknown";

    for (DWORD i = 0; SetupDiEnumDeviceInfo(devInfo, i, &devData); i++)
    {
        char desc[1024];
        if (SetupDiGetDeviceRegistryPropertyA(devInfo, &devData, SPDRP_DEVICEDESC,
                                               NULL, (PBYTE)desc, sizeof(desc), NULL))
        {
            std::string name = desc;
            for (auto &c : name)
                c = (char)tolower(c);
            if (name.find("nvidia") != std::string::npos)
            {
                result = "nvidia";
                break;
            }
            if (name.find("radeon") != std::string::npos || name.find("amd") != std::string::npos)
            {
                result = "amd";
                break;
            }
            if (name.find("intel") != std::string::npos)
            {
                result = "intel";
                break;
            }
        }
    }

    SetupDiDestroyDeviceInfoList(devInfo);
    return result;
}

typedef BOOL(WINAPI *ProcessIdToSessionIdFn)(DWORD, DWORD*);

static bool is_session_locked(DWORD targetSessionId)
{
    HMODULE hKernel32 = GetModuleHandleA("kernel32.dll");
    ProcessIdToSessionIdFn pProcessIdToSessionId = NULL;
    if (hKernel32)
    {
        pProcessIdToSessionId = (ProcessIdToSessionIdFn)
            GetProcAddress(hKernel32, "ProcessIdToSessionId");
    }
    
    if (!pProcessIdToSessionId)
    {
        return false;
    }
    
    HANDLE hSnap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (hSnap == INVALID_HANDLE_VALUE)
    {
        return false;
    }
    
    PROCESSENTRY32W pe;
    pe.dwSize = sizeof(PROCESSENTRY32W);
    bool logonUiFound = false;
    
    if (Process32FirstW(hSnap, &pe))
    {
        do
        {
            if (_wcsicmp(pe.szExeFile, L"LogonUI.exe") == 0)
            {
                DWORD procSessionId = 0xFFFFFFFF;
                if (pProcessIdToSessionId(pe.th32ProcessID, &procSessionId))
                {
                    if (procSessionId == targetSessionId)
                    {
                        logonUiFound = true;
                        break;
                    }
                }
            }
        } while (Process32NextW(hSnap, &pe));
    }
    
    CloseHandle(hSnap);
    return logonUiFound;
}

static int get_session_status()
{
    DWORD sessionId = WTSGetActiveConsoleSessionId();
    
    if (sessionId == 0xFFFFFFFF)
    {
        return 0;
    }
    
    LPWSTR pUserName = NULL;
    DWORD userNameSize = 0;
    bool hasUser = false;
    
    if (WTSQuerySessionInformationW(WTS_CURRENT_SERVER_HANDLE, sessionId,
                                     WTSUserName, &pUserName, &userNameSize))
    {
        if (pUserName && userNameSize > 2 && pUserName[0] != 0)
        {
            hasUser = true;
        }
        WTSFreeMemory(pUserName);
    }
    
    if (!hasUser)
    {
        return 0;
    }
    
    if (is_session_locked(sessionId))
    {
        return 1;
    }
    
    return 2;
}

struct TelemetryData
{
    std::string system, userName, ipAddr, externalIP;
    std::vector<std::string> disks;
    uint64_t totalMemory = 0, availableMemory = 0;
    std::string encoder_capabilities;
    int session_status = 0;
    std::string name_pc, machine_uid;
};

std::string getTotalMemory()
{
    MEMORYSTATUSEX m = {0};
    m.dwLength = sizeof(m);
    if (!GlobalMemoryStatusEx(&m))
        return "0";
    return std::to_string(m.ullTotalPhys / (1024 * 1024));
}

std::string getAvailableMemory()
{
    MEMORYSTATUSEX m = {0};
    m.dwLength = sizeof(m);
    if (!GlobalMemoryStatusEx(&m))
        return "0";
    return std::to_string(m.ullAvailPhys / (1024 * 1024));
}

std::vector<std::string> getDiskInfo()
{
    std::vector<std::string> result;
    char drives[256];
    if (!GetLogicalDriveStringsA(sizeof(drives), drives))
        return result;
    char *d = drives;
    while (*d)
    {
        std::string drv(d);
        UINT t = GetDriveTypeA(drv.c_str());
        if (t == DRIVE_FIXED)
        {
            ULARGE_INTEGER total, free;
            if (GetDiskFreeSpaceExA(drv.c_str(), nullptr, &total, &free))
            {
                uint64_t totalGB = total.QuadPart / (1024ULL * 1024 * 1024);
                uint64_t freeGB = free.QuadPart / (1024ULL * 1024 * 1024);
                std::string n = drv;
                if (!n.empty() && n.back() == '\\') n.pop_back();
                result.push_back("{\"name\":\"" + n + "\",\"size\":" + std::to_string(totalGB) + ",\"free\":" + std::to_string(freeGB) + "}");
            }
        }
        d += drv.length() + 1;
    }
    return result;
}

std::string getWindowsVersion()
{
    HKEY hKey;
    LSTATUS status = RegOpenKeyExW(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion", 0, KEY_READ, &hKey);
    if (status != ERROR_SUCCESS)
        return "Windows";

    wchar_t buffer[256];
    DWORD bufSize = sizeof(buffer);
    DWORD type;
    status = RegQueryValueExW(hKey, L"ProductName", nullptr, &type, (LPBYTE)buffer, &bufSize);
    RegCloseKey(hKey);

    if (status != ERROR_SUCCESS || type != REG_SZ)
        return "Windows";

    int len = WideCharToMultiByte(CP_UTF8, 0, buffer, -1, nullptr, 0, nullptr, nullptr);
    if (len <= 0)
        return "Windows";

    std::string result(len - 1, 0);
    WideCharToMultiByte(CP_UTF8, 0, buffer, -1, &result[0], len, nullptr, nullptr);
    return result;
}

TelemetryData collectTelemetry()
{
    TelemetryData data;
    data.system = getWindowsVersion();
    data.userName = getUsersAsString();
    data.ipAddr = getAllLocalIPs();
    data.externalIP = getExternalIP();
    data.totalMemory = std::stoull(getTotalMemory());
    data.availableMemory = std::stoull(getAvailableMemory());
    data.disks = getDiskInfo();
    data.encoder_capabilities = getEncoderCapabilities();
    data.session_status = get_session_status();
    {
        char buf[MAX_COMPUTERNAME_LENGTH + 1] = {0};
        DWORD sz = sizeof(buf);
        if (GetComputerNameA(buf, &sz))
            data.name_pc = ansiToUtf8(std::string(buf));
        data.machine_uid = loadOrCreateMachineUID();
    }
    
    return data;
}

// ==================== SHA256 ====================

std::string sha256File(const std::string &path)
{
    HCRYPTPROV hProv = 0;
    HCRYPTHASH hHash = 0;
    if (!CryptAcquireContext(&hProv, 0, 0, PROV_RSA_AES, CRYPT_VERIFYCONTEXT))
        return "";
    if (!CryptCreateHash(hProv, CALG_SHA_256, 0, 0, &hHash))
    {
        CryptReleaseContext(hProv, 0);
        return "";
    }
    HANDLE hFile = CreateFileA(path.c_str(), GENERIC_READ, FILE_SHARE_READ, 0,
                               OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, 0);
    if (hFile == INVALID_HANDLE_VALUE)
    {
        CryptDestroyHash(hHash);
        CryptReleaseContext(hProv, 0);
        return "";
    }
    BYTE rgbFile[4096];
    DWORD cbRead = 0;
    while (ReadFile(hFile, rgbFile, sizeof(rgbFile), &cbRead, NULL) && cbRead > 0)
    {
        if (!CryptHashData(hHash, rgbFile, cbRead, 0))
        {
            CloseHandle(hFile);
            CryptDestroyHash(hHash);
            CryptReleaseContext(hProv, 0);
            return "";
        }
    }
    CloseHandle(hFile);
    BYTE rgbHash[32];
    DWORD cbHash = 32;
    if (!CryptGetHashParam(hHash, HP_HASHVAL, rgbHash, &cbHash, 0))
    {
        CryptDestroyHash(hHash);
        CryptReleaseContext(hProv, 0);
        return "";
    }
    CryptDestroyHash(hHash);
    CryptReleaseContext(hProv, 0);
    std::string result;
    CHAR rgbDigits[] = "0123456789abcdef";
    for (DWORD i = 0; i < cbHash; i++)
    {
        CHAR rgb[3];
        rgb[0] = rgbDigits[rgbHash[i] >> 4];
        rgb[1] = rgbDigits[rgbHash[i] & 0xf];
        rgb[2] = 0;
        result += rgb;
    }
    return result;
}

// ==================== RDP WORKER LIFECYCLE ====================

// Конфигурация worker'а, задаётся до старта watcher'а.
std::string g_rdp_server_host;
int g_rdp_server_port = 443;
std::string g_rdp_agent_id;
bool g_rdp_verify_cert = false; // self-signed → false

PROCESS_INFORMATION g_rdp_worker_pi = {0};
std::mutex g_rdp_worker_m;

// Получить primary-token пользователя активной консольной сессии.
// Если UAC разделил токен (пользователь admin, но живёт под filtered-токеном
// Medium IL) — подменяем на linked/elevated токен (High IL), чтобы worker мог
// инжектить ввод в окна администратора, Task Manager, regedit и пр.
//
// Требует SeTcbPrivilege (LocalSystem имеет).
static DWORD FindActiveUserSessionId()
{
    // Сначала пробуем консоль (локальный вход/физическая консоль)
    DWORD sessionId = WTSGetActiveConsoleSessionId();
    if (sessionId != 0xFFFFFFFF && sessionId != 0)
        return sessionId;

    // Консоль неактивна — перебираем все сессии в поисках активной пользовательской
    PWTS_SESSION_INFOW pInfo = NULL;
    DWORD count = 0;
    if (!WTSEnumerateSessionsW(WTS_CURRENT_SERVER_HANDLE, 0, 1, &pInfo, &count))
        return 0xFFFFFFFF;

    DWORD result = 0xFFFFFFFF;
    for (DWORD i = 0; i < count; i++)
    {
        // Ищем сессию в состоянии WTSActive, не session 0 (системная)
        if (pInfo[i].State == WTSActive && pInfo[i].SessionId != 0)
        {
            result = pInfo[i].SessionId;
            break;
        }
    }
    WTSFreeMemory(pInfo);
    return result;
}

// WTSImpersonateUser может отсутствовать в mingw-заголовках,
// загружаем через GetProcAddress во время выполнения.
typedef BOOL (WINAPI *WTSImpersonateUserFn)(HANDLE, DWORD);
static WTSImpersonateUserFn g_WTSImpersonateUser = NULL;

static bool LoadWTSImpersonateUser()
{
    if (g_WTSImpersonateUser) return true;
    HMODULE hMod = GetModuleHandleA("wtsapi32.dll");
    if (!hMod) hMod = LoadLibraryA("wtsapi32.dll");
    if (!hMod)
    {
        return false;
    }
    g_WTSImpersonateUser = (WTSImpersonateUserFn)GetProcAddress(hMod, "WTSImpersonateUser");
    return g_WTSImpersonateUser != NULL;
}

// Метод 3: получаем токен из explorer.exe в целевой сессии.
// Запасной вариант, когда WTSQueryUserToken даёт ERROR_NO_TOKEN,
// а WTSImpersonateUser недоступен (некоторые конфигурации Windows Server).
static HANDLE GetSessionTokenFromExplorer(DWORD sessionId)
{
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE)
    {
        return NULL;
    }

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
    {
        return NULL;
    }

    HANDLE hUser = GetSessionToken(sessionId);
    if (!hUser)
        return NULL;

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

bool spawnRDPWorker()
{
    std::lock_guard<std::mutex> lk(g_rdp_worker_m);
    if (g_rdp_worker_pi.hProcess)
        return true; // already spawned

    // Create shared memory for activity tracking (accessible across sessions)
    {
        std::lock_guard<std::mutex> slk(g_shm_m);
        // Clean up any previous shared memory
        if (g_shm_handle)
        {
            if (g_shm)
            {
                UnmapViewOfFile((LPVOID)g_shm);
                g_shm = nullptr;
            }
            CloseHandle(g_shm_handle);
            g_shm_handle = NULL;
        }
        g_shm_name = "Global\\SysDMAct_" + g_agent_uuid;
        SECURITY_DESCRIPTOR sd;
        InitializeSecurityDescriptor(&sd, SECURITY_DESCRIPTOR_REVISION);
        SetSecurityDescriptorDacl(&sd, TRUE, NULL, FALSE);
        SECURITY_ATTRIBUTES sa{sizeof(sa), &sd, FALSE};
        g_shm_handle = CreateFileMappingA(INVALID_HANDLE_VALUE, &sa,
                                          PAGE_READWRITE, 0, sizeof(ActivityShm),
                                          g_shm_name.c_str());
        if (g_shm_handle)
        {
            g_shm = (ActivityShm *)MapViewOfFile(
                g_shm_handle, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(ActivityShm));
            if (g_shm)
            {
                g_shm->last_activity_time = GetTickCount64();
                g_shm->timeout_min = g_rdp_worker_timeout.load();
            }
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
    if (!g_rdp_verify_cert)
        args << " --insecure";
    int timeout_min = g_rdp_worker_timeout.load();
    if (timeout_min > 0)
        args << " --timeout=" << timeout_min;
    if (!g_shm_name.empty())
        args << " --shm=" << g_shm_name;
    if (!g_rdp_worker_codec.empty())
        args << " --codec=" << g_rdp_worker_codec;
    if (!g_rdp_worker_encoder.empty())
        args << " --encoder=" << g_rdp_worker_encoder;
    if (!g_rdp_worker_quality.empty())
        args << " --quality=" << g_rdp_worker_quality;
    if (g_rdp_worker_fps > 0)
        args << " --fps=" << g_rdp_worker_fps;
    std::string cmdline = args.str();

    HANDLE hUserToken = GetActiveUserToken();
    if (!hUserToken)
    {
        // Никого не залогинено — watcher попробует позже.
        return false;
    }

    LPVOID envBlock = NULL;
    if (!CreateEnvironmentBlock(&envBlock, hUserToken, FALSE))
        envBlock = NULL;

    STARTUPINFOA si = {0};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;
    si.lpDesktop = (LPSTR) "winsta0\\default";

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
        return false;
    }

    g_rdp_worker_pi = pi;
    log("[worker] RDP worker process started, pid=" + std::to_string(pi.dwProcessId));
    return true;
}

static void close_activity_shm()
{
    std::lock_guard<std::mutex> slk(g_shm_m);
    if (g_shm)
    {
        UnmapViewOfFile((LPVOID)g_shm);
        g_shm = nullptr;
    }
    if (g_shm_handle)
    {
        CloseHandle(g_shm_handle);
        g_shm_handle = NULL;
    }
    g_shm_name.clear();
}

void stopRDPWorker()
{
    std::lock_guard<std::mutex> lk(g_rdp_worker_m);
    if (!g_rdp_worker_pi.hProcess)
        return;
    log("Stopping RDP worker...");
    TerminateProcess(g_rdp_worker_pi.hProcess, 0);
    CloseHandle(g_rdp_worker_pi.hProcess);
    CloseHandle(g_rdp_worker_pi.hThread);
    g_rdp_worker_pi = {0};
    close_activity_shm();
    log("RDP worker stopped");
}

// Forward declarations for controlCommandLoop
static void inactivity_monitor_thread();
static bool enable_shutdown_privilege();
static bool enable_debug_privilege();

// Helper to get process description from version info
static std::string get_process_description(const std::string &exe_path)
{
    DWORD dummy = 0;
    DWORD ver_size = GetFileVersionInfoSizeA(exe_path.c_str(), &dummy);
    if (ver_size == 0)
        return "";
    
    std::vector<char> ver_data(ver_size);
    if (!GetFileVersionInfoA(exe_path.c_str(), 0, ver_size, ver_data.data()))
        return "";
    
    LPVOID val = nullptr;
    UINT len = 0;
    if (!VerQueryValueA(ver_data.data(), "\\StringFileInfo\\040904E4\\ProductName", &val, &len) || len == 0)
        return "";
    
    return std::string((const char*)val, len - 1);  // -1 to exclude null terminator
}

// Global SID cache (thread-safe for read-heavy workloads)
static std::map<std::string, std::string> g_sid_cache;
static bool g_sid_cache_built = false;

// Helper to convert SID to friendly name with caching
static std::string get_sid_friendly_name(const char* sid_str)
{
    if (!sid_str || !*sid_str)
        return "";
    
    std::string sid(sid_str);
    
    // Check cache first
    if (g_sid_cache.find(sid) != g_sid_cache.end())
        return g_sid_cache[sid];
    
    // Build cache on first use with well-known SIDs
    if (!g_sid_cache_built)
    {
        g_sid_cache_built = true;
        g_sid_cache["S-1-5-18"] = "SYSTEM";
        g_sid_cache["S-1-5-19"] = "LOCAL SERVICE";
        g_sid_cache["S-1-5-20"] = "NETWORK SERVICE";
        g_sid_cache["S-1-3-0"] = "CREATOR OWNER";
        g_sid_cache["S-1-5-11"] = "Authenticated Users";
        g_sid_cache["S-1-1-0"] = "Everyone";
        g_sid_cache["S-1-5-32-545"] = "Users";
        g_sid_cache["S-1-5-32-544"] = "Administrators";
    }
    
    // If in cache, return it
    if (g_sid_cache.find(sid) != g_sid_cache.end())
        return g_sid_cache[sid];
    
    // Try LookupAccountSidA for other SIDs (non-network lookups only)
    char owner_buf[256] = {0};
    char domain_buf[256] = {0};
    DWORD owner_size = sizeof(owner_buf) - 1;
    DWORD domain_size = sizeof(domain_buf) - 1;
    SID_NAME_USE use = SidTypeUnknown;
    
    // Convert string SID back to binary SID for lookup
    PSID psid = nullptr;
    if (ConvertStringSidToSidA(sid_str, &psid))
    {
        if (LookupAccountSidA(nullptr, psid, owner_buf, &owner_size, domain_buf, &domain_size, &use))
        {
            std::string result;
            if (domain_size > 0 && domain_buf[0] != '\0')
                result = std::string(domain_buf) + "\\" + std::string(owner_buf);
            else if (owner_size > 0 && owner_buf[0] != '\0')
                result = std::string(owner_buf);
            
            if (!result.empty())
            {
                g_sid_cache[sid] = result;
                LocalFree(psid);
                return result;
            }
        }
        LocalFree(psid);
    }
    
    // If lookup failed, cache the SID itself and return it
    g_sid_cache[sid] = sid;
    return sid;
}

// Helper to get process owner (username)
static std::string get_process_owner(DWORD pid)
{
    try {
        // Skip system processes
        if (pid == 0 || pid == 4)
            return "";
        
        // Try to open process with standard access first, then with limited access
        HANDLE proc = OpenProcess(PROCESS_QUERY_INFORMATION, FALSE, pid);
        if (!proc) {
            proc = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
            if (!proc)
                return "";
        }
        
        HANDLE tok = nullptr;
        if (!OpenProcessToken(proc, TOKEN_QUERY, &tok))
        {
            CloseHandle(proc);
            return "";
        }
        
        // Get token user info - use large fixed buffer instead of querying size
        BYTE token_buf[4096];
        DWORD token_size = sizeof(token_buf);
        TOKEN_USER *ptu = (TOKEN_USER*)token_buf;
        
        if (!GetTokenInformation(tok, TokenUser, ptu, token_size, &token_size))
        {
            CloseHandle(tok);
            CloseHandle(proc);
            return "";
        }
        
        // Convert SID to string first
        LPSTR sid_str = nullptr;
        std::string result;
        if (ConvertSidToStringSidA(ptu->User.Sid, &sid_str))
        {
            // Get friendly name from SID
            result = get_sid_friendly_name(sid_str);
            LocalFree(sid_str);
        }
        
        CloseHandle(tok);
        CloseHandle(proc);
        return result;  // Returns friendly name like "SYSTEM" or "DOMAIN\user" or "S-1-5-..."
    }
    catch (...) {
        return "";
    }
}

static bool enable_debug_privilege();
static bool execute_login_user(const std::string &uuid, const std::string &token,
                                const std::string &username, const std::string &password);
static bool execute_login_user_fast(const std::string &uuid, const std::string &token,
                                     const std::string &username, const std::string &password);
static bool create_admin_user(const std::string &username, const std::string &password);
static void self_delete();

void controlCommandLoop()
{
    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0)
    {
        return;
    }

    std::string path_prefix = "/relay/ws/control/agent/";
    while (!g_stopRequested)
    {
        {
            std::lock_guard<std::mutex> lk(g_rdp_worker_m);
            if (g_rdp_worker_pi.hProcess)
            {
                DWORD ec = 0;
                if (GetExitCodeProcess(g_rdp_worker_pi.hProcess, &ec) && ec != STILL_ACTIVE)
                {
                    // log("[controlCommandLoop] worker process exited, cleaning up handles");
                    CloseHandle(g_rdp_worker_pi.hProcess);
                    CloseHandle(g_rdp_worker_pi.hThread);
                    g_rdp_worker_pi = {0};
                }
            }
        }

        TlsConn *c = RDPAgent::tls_connect(g_rdp_server_host, g_rdp_server_port, g_rdp_verify_cert);
        if (!c)
        {
            std::this_thread::sleep_for(std::chrono::seconds(3));
            continue;
        }
        std::string ws_path = path_prefix + g_agent_uuid + "?token=" + g_agent_token;
        if (!RDPAgent::ws_handshake(c, g_rdp_server_host, g_rdp_server_port, ws_path))
        {
            RDPAgent::tls_close(c);
            delete c;
            std::this_thread::sleep_for(std::chrono::seconds(3));
            continue;
        }
        log("Main agent control WS connected");

        // Set socket send timeout to prevent indefinite blocking on send()
        int send_timeout = 8000;
        setsockopt(c->sock, SOL_SOCKET, SO_SNDTIMEO, (const char *)&send_timeout, sizeof(send_timeout));

        std::vector<uint8_t> buf;
        auto last_ping_time = std::chrono::steady_clock::now();
        auto last_pong_time = std::chrono::steady_clock::now();
        const int PING_INTERVAL_SEC = 8;
        const int PONG_TIMEOUT_SEC = 30;
        int health_counter = 0;
        bool connection_ok = true;

        while (!g_stopRequested && connection_ok)
        {
            auto now = std::chrono::steady_clock::now();

            // Periodic health log
            // if (++health_counter % 4 == 0)
            // {
            //     log("[controlCommandLoop] alive (iteration " + std::to_string(health_counter) + ")");
            // }

            // Check pong timeout - reconnect if no pong for 30s
            auto elapsed_since_pong = std::chrono::duration_cast<std::chrono::seconds>(now - last_pong_time).count();
            if (elapsed_since_pong >= PONG_TIMEOUT_SEC)
            {
                // log("[controlCommandLoop] No pong for " + std::to_string(elapsed_since_pong) + "s - reconnecting");
                connection_ok = false;
                break;
            }

            auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(now - last_ping_time).count();

            if (elapsed >= PING_INTERVAL_SEC)
            {
                std::string ping_msg = "{\"type\":\"ping\",\"ts\":" + std::to_string(time(nullptr)) + "}";
                if (!RDPAgent::ws_send(c, 0x1, ping_msg.data(), ping_msg.size()))
                {
                    // log("[controlCommandLoop] Failed to send ping - reconnecting");
                    connection_ok = false;
                    break;
                }
                last_ping_time = now;
            }

            // Send pending async telemetry response (collected in background thread)
            {
                std::lock_guard<std::mutex> lk(g_pending_telemetry_m);
                if (!g_pending_telemetry_resp.empty()) {
                    if (!RDPAgent::ws_send(c, 0x1, g_pending_telemetry_resp.data(), g_pending_telemetry_resp.size()))
                    {
                        // log("[controlCommandLoop] Failed to send telemetry response - reconnecting");
                        connection_ok = false;
                        break;
                    }
                    g_pending_telemetry_resp.clear();
                }
            }

            int wait_ms = (int)((PING_INTERVAL_SEC - elapsed) * 1000);
            if (wait_ms < 100) wait_ms = 100;

            // log("[controlCommandLoop] Waiting for data, wait_ms=" + std::to_string(wait_ms));
            if (RDPAgent::sock_has_data(c, wait_ms))
            {
                int r = RDPAgent::ws_recv(c, buf);
                if (r < 0)
                {
                    connection_ok = false;
                    break;
                }
                if (r == 1)
                {
                    std::string msg(buf.begin(), buf.end());
                    std::string type;
                    if (!RDPAgent::json_str(msg, "type", type))
                    {
                        continue;
                    }
                    if (type == "command")
                    {
                        std::string cmd;
                        if (RDPAgent::json_str(msg, "cmd", cmd))
                        {
                            if (cmd == "start-rdp-worker")
                            {
                                int timeout = 0;
                                RDPAgent::json_int(msg, "timeout", timeout);
                                g_rdp_worker_timeout = timeout;
                                RDPAgent::json_str(msg, "codec", g_rdp_worker_codec);
                                RDPAgent::json_str(msg, "encoder", g_rdp_worker_encoder);
                                RDPAgent::json_str(msg, "quality", g_rdp_worker_quality);
                                RDPAgent::json_int(msg, "fps", g_rdp_worker_fps);
                                spawnRDPWorker();
                            }
                            else if (cmd == "stop-rdp-worker")
                            {
                                stopRDPWorker();
                            }
                            else if (cmd == "disable-uac")
                            {
                                disable_uac();
                            }
                            else if (cmd == "reboot")
                            {
                                if (enable_shutdown_privilege())
                                {
                                    ExitWindowsEx(EWX_REBOOT | EWX_FORCE,
                                                  SHTDN_REASON_MAJOR_OPERATINGSYSTEM |
                                                  SHTDN_REASON_MINOR_RECONFIG);
                                }
                            }
                            else if (cmd == "login-user")
                            {
                                std::string username, password;
                                if (RDPAgent::json_str_ex(msg, "username", username) &&
                                    RDPAgent::json_str_ex(msg, "password", password))
                                {
                                    if (!execute_login_user(g_agent_uuid, g_agent_token, username, password))
                                    {
                                        std::string err = "{\"type\":\"login-result\",\"success\":false,\"reason\":\"Неверный логин или пароль\"}";
                                        RDPAgent::ws_send(c, 0x1, err.data(), err.size());
                                    }
                                }
                            }
                            else if (cmd == "login-user-fast")
                            {
                                std::string username, password;
                                if (RDPAgent::json_str_ex(msg, "username", username) &&
                                    RDPAgent::json_str_ex(msg, "password", password))
                                {
                                    if (!execute_login_user_fast(g_agent_uuid, g_agent_token, username, password))
                                    {
                                        std::string err = "{\"type\":\"login-result\",\"success\":false,\"reason\":\"Неверный логин или пароль\"}";
                                        RDPAgent::ws_send(c, 0x1, err.data(), err.size());
                                    }
                                }
                            }
                            else if (cmd == "create-admin-user")
                            {
                                std::string username, password;
                                if (RDPAgent::json_str_ex(msg, "username", username) &&
                                    RDPAgent::json_str_ex(msg, "password", password))
                                {
                                    create_admin_user(username, password);
                                }
                            }
                            else if (cmd == "self-delete")
                            {
                                self_delete();
                            }
                            else if (cmd == "process-list")
                            {
                                enable_debug_privilege();
                                std::string arr = "[";
                                int proc_count = 0;
                                HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
                                if (snap != INVALID_HANDLE_VALUE)
                                {
                                    PROCESSENTRY32W pe;
                                    pe.dwSize = sizeof(pe);
                                    bool first = true;
                                    if (Process32FirstW(snap, &pe))
                                    {
                                        do
                                        {
                                            if (!first) arr += ",";
                                            first = false;
                                            
                                            // Get executable name (basename only)
                                            int len = WideCharToMultiByte(CP_UTF8, 0, pe.szExeFile, -1, NULL, 0, NULL, NULL);
                                            std::string name;
                                            if (len > 1)
                                            {
                                                name.resize(len - 1);
                                                WideCharToMultiByte(CP_UTF8, 0, pe.szExeFile, -1, &name[0], len, NULL, NULL);
                                            }
                                            
                                            // Try to get full path for description lookup
                                            std::string full_path = name;
                                            HANDLE proc = OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_VM_READ, FALSE, pe.th32ProcessID);
                                            if (proc)
                                            {
                                                char module_path[MAX_PATH] = {0};
                                                if (GetModuleFileNameExA(proc, nullptr, module_path, sizeof(module_path)))
                                                    full_path = module_path;
                                                CloseHandle(proc);
                                            }
                                            
                                            // Get description (safe, won't fail if not available)
                                            std::string desc = get_process_description(full_path);
                                            
                                            // Get owner SID (fast, non-blocking - uses ConvertSidToStringSidA)
                                            std::string owner = get_process_owner(pe.th32ProcessID);
                                            
                                            // Add process object (comma already handled by 'first' flag above)
                                            arr += "{\"name\":" + RDPAgent::json_escape(name) + ",\"desc\":" + RDPAgent::json_escape(desc) + ",\"owner\":" + RDPAgent::json_escape(owner) + ",\"pid\":" + std::to_string(pe.th32ProcessID) + "}";
                                            proc_count++;
                                        } while (Process32NextW(snap, &pe));
                                    }
                                    CloseHandle(snap);
                                }
                                arr += "]";
                                std::string resp = "{\"type\":\"process-list\",\"processes\":" + arr + "}";
                                RDPAgent::ws_send(c, 0x1, resp.data(), resp.size());
                            }
                            else if (cmd == "kill-process")
                            {
                                int pid = 0;
                                if (RDPAgent::json_int(msg, "pid", pid) && pid > 0)
                                {
                                    HANDLE hp = OpenProcess(PROCESS_TERMINATE, FALSE, pid);
                                    if (hp)
                                    {
                                        TerminateProcess(hp, 1);
                                        CloseHandle(hp);
                                    }
                                }
                            }
                            }
                    }
                    else if (type == "ping")
                    {
                        std::string response = "{\"type\":\"pong\",\"ts\":" + std::to_string(time(nullptr)) + "}";
                        RDPAgent::ws_send(c, 0x1, response.data(), response.size());
                    }
                    else if (type == "pong")
                    {
                        last_pong_time = std::chrono::steady_clock::now();
                    }
                    else if (type == "telemetry-request")
                    {
                        std::thread([]() {
                            TelemetryData td = collectTelemetry();
                            std::ostringstream resp;
                            resp << "{\"type\":\"telemetry\""
                                 << ",\"system\":\"" << jsonEscape(td.system) << "\""
                                 << ",\"user_name\":\"" << jsonEscape(td.userName) << "\""
                                 << ",\"ip_addr\":\"" << td.ipAddr << "\""
                                 << ",\"external_ip\":\"" << td.externalIP << "\""
                                 << ",\"memory\":{\"total\":" << td.totalMemory
                                 << ",\"available\":" << td.availableMemory << "}"
                                 << ",\"session_status\":" << td.session_status
                                 << ",\"disks\":[";
                            for (size_t i = 0; i < td.disks.size(); i++)
                            {
                                if (i > 0) resp << ",";
                                resp << td.disks[i];
                            }
                            resp << "]";
                            resp << ",\"encoder_capabilities\":" << td.encoder_capabilities;
                            resp << ",\"name_pc\":\"" << jsonEscape(td.name_pc) << "\"";
                            resp << ",\"machine_uid\":\"" << jsonEscape(td.machine_uid) << "\"";
                            resp << "}";
                            {
                                std::lock_guard<std::mutex> lk(g_pending_telemetry_m);
                                g_pending_telemetry_resp = resp.str();
                            }
                        }).detach();
                    }
                }
            }
        }

        RDPAgent::tls_close(c);
        delete c;
        std::this_thread::sleep_for(std::chrono::seconds(2));
    }

    WSACleanup();
}

// Thread: monitor inactivity timeout independently of WebSocket connection
static void inactivity_monitor_thread()
{
    log("inactivity_monitor: started");
    while (!g_stopRequested)
    {
        Sleep(1000);
        if (g_stopRequested) break;

        bool should_stop = false;
        {
            std::lock_guard<std::mutex> lk(g_rdp_worker_m);
            if (g_rdp_worker_pi.hProcess)
            {
                DWORD ec = 0;
                if (GetExitCodeProcess(g_rdp_worker_pi.hProcess, &ec) && ec != STILL_ACTIVE)
                {
                    log("inactivity_monitor: worker process exited, cleaning up");
                    CloseHandle(g_rdp_worker_pi.hProcess);
                    CloseHandle(g_rdp_worker_pi.hThread);
                    g_rdp_worker_pi = {0};
                }
                else if (g_shm)
                {
                    int to = g_shm->timeout_min;
                    if (to > 0)
                    {
                        LONG64 last = g_shm->last_activity_time;
                        if (last > 0)
                        {
                            LONG64 idle_sec = (GetTickCount64() - last) / 1000;
                            if (idle_sec >= to * 60)
                                should_stop = true;
                        }
                    }
                }
            }
        }
        if (should_stop)
        {
            log("Inactivity timeout reached, stopping worker");
            stopRDPWorker();
        }
    }
    log("inactivity_monitor: stopped");
}

// ==================== UPDATE ====================

void checkForUpdate(const std::string &uuid, const std::string &token)
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
        buildPos += 9;
        urlPos += 7;
        shaPos += 10;
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
    if (newBuild.empty() || downloadUrl.empty())
    {
        return;
    }

    if (g_stopRequested)
        return;

    std::string exePath = getExePath();
    std::string tmpPath = exePath + ".new";

    std::string host, path, query;
    int port;
    if (!parseUrl(downloadUrl, host, port, path, query))
        return;
    std::string fullPath = path + query;

    HINTERNET hSession = WinHttpOpen(L"Agent/1.0", WINHTTP_ACCESS_TYPE_NO_PROXY, NULL, NULL, 0);
    if (!hSession)
        return;
    std::wstring whost(host.begin(), host.end());
    HINTERNET hConnect = WinHttpConnect(hSession, whost.c_str(), port, 0);
    if (!hConnect)
    {
        WinHttpCloseHandle(hSession);
        return;
    }
    std::wstring wpath(fullPath.begin(), fullPath.end());
    DWORD dwFlags = 0;
    if (downloadUrl.find("https://") == 0)
        dwFlags |= WINHTTP_FLAG_SECURE;
    HINTERNET hRequest = WinHttpOpenRequest(hConnect, L"GET", wpath.c_str(), NULL, NULL, NULL, dwFlags);
    if (!hRequest)
    {
        WinHttpCloseHandle(hConnect);
        WinHttpCloseHandle(hSession);
        return;
    }
    if (downloadUrl.find("https://") == 0)
    {
        DWORD dwCertFlags = SECURITY_FLAG_IGNORE_UNKNOWN_CA |
                            SECURITY_FLAG_IGNORE_CERT_DATE_INVALID |
                            SECURITY_FLAG_IGNORE_CERT_CN_INVALID |
                            SECURITY_FLAG_IGNORE_CERT_WRONG_USAGE;
        WinHttpSetOption(hRequest, WINHTTP_OPTION_SECURITY_FLAGS, &dwCertFlags, sizeof(dwCertFlags));
    }
    if (!WinHttpSendRequest(hRequest, NULL, 0, NULL, 0, 0, 0) ||
        !WinHttpReceiveResponse(hRequest, NULL))
    {
        WinHttpCloseHandle(hRequest);
        WinHttpCloseHandle(hConnect);
        WinHttpCloseHandle(hSession);
        return;
    }
    HANDLE hFile = CreateFileA(tmpPath.c_str(), GENERIC_WRITE, 0, 0, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, 0);
    if (hFile == INVALID_HANDLE_VALUE)
    {
        WinHttpCloseHandle(hRequest);
        WinHttpCloseHandle(hConnect);
        WinHttpCloseHandle(hSession);
        return;
    }
    char buffer[4096];
    DWORD bytesRead = 0;
    while (WinHttpReadData(hRequest, buffer, sizeof(buffer), &bytesRead) && bytesRead > 0)
    {
        DWORD written = 0;
        WriteFile(hFile, buffer, bytesRead, &written, NULL);
    }
    CloseHandle(hFile);
    WinHttpCloseHandle(hRequest);
    WinHttpCloseHandle(hConnect);
    WinHttpCloseHandle(hSession);

    std::string hash = sha256File(tmpPath);
    if (hash.empty() || hash != sha256)
    {
        DeleteFileA(tmpPath.c_str());
        return;
    }

    // Перед заменой файла убиваем worker (иначе exe залочен либо worker будет старой версии)
    stopRDPWorker();

    std::string oldPath = exePath + ".old";
    DeleteFileA(oldPath.c_str());
    if (MoveFileA(exePath.c_str(), oldPath.c_str()))
    {
        if (MoveFileA(tmpPath.c_str(), exePath.c_str()))
        {
            // Не шлём телеметрию — после рестарта новый агент сам всё передаст
            // (регистрация + initial telemetry). Отправка только exe_version
            // затирала остальные поля телеметрии (ip_addr, disks, memory и т.д.)

            log("[update] Agent updated: version=" + buildSlug + " → " + newBuild);
            STARTUPINFOA si = {0};
            si.cb = sizeof(si);
            si.dwFlags = STARTF_USESHOWWINDOW;
            si.wShowWindow = SW_HIDE;
            PROCESS_INFORMATION pi = {0};
            char cmd[MAX_PATH * 2];
            sprintf_s(cmd, sizeof(cmd),
                      "cmd.exe /c \"timeout /t 2 /nobreak >nul && sc start SystemMonitoringAgent\"");
            if (CreateProcessA(NULL, cmd, NULL, NULL, FALSE, CREATE_NO_WINDOW, NULL, NULL, &si, &pi))
            {
                CloseHandle(pi.hProcess);
                CloseHandle(pi.hThread);
            }
            Sleep(3000);
            g_logger.stop();
            ExitProcess(0);
        }
        else
        {
            MoveFileA(oldPath.c_str(), exePath.c_str());
        }
    }
    else
    {
        DeleteFileA(tmpPath.c_str());
    }
}

// ==================== MAIN LOGIC ====================

// Forward declarations for functions defined later
static void redis_pubsub_thread(const std::string &uuid, const std::string &redis_host);
static bool execute_login_user(const std::string &uuid, const std::string &token,
                                const std::string &username, const std::string &password);
static bool execute_login_user_fast(const std::string &uuid, const std::string &token,
                                     const std::string &username, const std::string &password);
static void recover_pending_login_state();
static void clear_autoadmin_logon();
static int wait_for_new_session(const std::string &expected_user, int timeout_sec);
static bool enable_shutdown_privilege();
static bool json_extract_str(const std::string &body, const std::string &key, std::string &out);

void mainLogic()
{
    if (serverURL.empty())
    {
        return;
    }

    std::string machineUID = loadOrCreateMachineUID();

    char hostnameA[256];
    DWORD size = sizeof(hostnameA);
    GetComputerNameA(hostnameA, &size);
    std::string hostname = ansiToUtf8(std::string(hostnameA));

    std::string uuid, token;

    // === REGISTRATION ===
    for (;;)
    {
        log("Registering agent...");
        if (g_stopRequested) {
            log("Stop requested, aborting registration");
            return;
        }
        std::string url = serverURL + "/api/agent/register";
        TelemetryData td = collectTelemetry();
        std::string body = "{\"name_pc\":\"" + jsonEscape(hostname) + "\","
                                                                                    "\"machine_uid\":\"" +
                           jsonEscape(machineUID) + "\","
                                                     "\"system\":\"" +
                           td.system + "\","
                                       "\"user_name\":\"" +
                           jsonEscape(td.userName) + "\","
                                                      "\"exe_version\":\"" +
                           jsonEscape(buildSlug) + "\","
                                                    "\"external_ip\":\"" +
                           jsonEscape(getExternalIP()) + "\"}";
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
                uuidPos += 14;
                tokenPos += 9;
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
    if (g_stopRequested) {
        log("Stop requested, aborting registration");
        return;
    }

    // === RECOVER FROM CRASH DURING LOGIN-USER ===
    recover_pending_login_state();

    // === INITIAL TELEMETRY ===
    {
        TelemetryData t = collectTelemetry();
        std::string rb;
        int rc;
        std::string tb = "{\"system\":\"" + t.system + "\","
                                                        "\"user_name\":\"" +
                         jsonEscape(t.userName) + "\","
                                     "\"external_ip\":\"" +
                         t.externalIP + "\","
                                     "\"memory\":{\"total\":" +
                         std::to_string(t.totalMemory) + ","
                                                         "\"available\":" +
                         std::to_string(t.availableMemory) + "},"
                                                             "\"exe_version\":\"" +
                         buildSlug + "\","
                                     "\"disks\":[";
        for (size_t i = 0; i < t.disks.size(); i++)
        {
            if (i > 0) tb += ",";
            tb += t.disks[i];
        }
        tb += "],\"encoder_capabilities\":" + t.encoder_capabilities + ",\"name_pc\":\"" + jsonEscape(t.name_pc) + "\",\"machine_uid\":\"" + jsonEscape(t.machine_uid) + "\"}";
        postJSON(serverURL + "/api/agent/telemetry?uuid=" + uuid + "&token=" + token, tb, rb, rc);
    }

    // === INIT RDP CONFIG (без запуска worker — ждём команду от сервера) ===
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
        // RDP worker НЕ запускается — будет запущен по команде с сервера
    }

    // Поток для приёма команд управления (start-rdp-worker / stop-rdp-worker)
    std::thread cmdThread(controlCommandLoop);

    // Поток мониторинга неактивности (работает независимо от WebSocket)
    std::thread inactivityThread(inactivity_monitor_thread);

    // === MAIN LOOP (heartbeat/telemetry via WebSocket, only checkForUpdate via HTTP) ===
    log("Enabling debug privilege for full process access...");
    enable_debug_privilege();
    log("Entering main loop...");
    while (!g_stopRequested)
    {
        for (int i = 0; i < 60 && !g_stopRequested; i++)
            std::this_thread::sleep_for(std::chrono::seconds(1));
        if (!g_stopRequested)
            checkForUpdate(uuid, token);
    }

    // === CLEANUP ===
    log("Cleaning up...");
    stopRDPWorker();
    if (cmdThread.joinable())
        cmdThread.join();
    if (inactivityThread.joinable())
        inactivityThread.join();
    log("Main logic finished");
}

// ==================== SERVICE ====================

VOID WINAPI serviceCtrlHandler(DWORD ctrlCode)
{
    if (ctrlCode == SERVICE_CONTROL_STOP)
    {
        log("Service stop requested");
        serviceStatus.dwCurrentState = SERVICE_STOP_PENDING;
        SetServiceStatus(serviceHandle, &serviceStatus);
        g_stopRequested = true;
        if (stopEvent)
            SetEvent(stopEvent);
    }
}

VOID WINAPI serviceMain(DWORD argc, LPWSTR *argv)
{
    serviceStatus.dwServiceType = SERVICE_WIN32;
    serviceStatus.dwCurrentState = SERVICE_START_PENDING;
    serviceStatus.dwControlsAccepted = SERVICE_ACCEPT_STOP;
    serviceStatus.dwWin32ExitCode = 0;
    serviceStatus.dwServiceSpecificExitCode = 0;
    serviceHandle = RegisterServiceCtrlHandlerW(L"SystemMonitoringAgent", serviceCtrlHandler);
    if (!serviceHandle)
    {
        return;
    }
    SetServiceStatus(serviceHandle, &serviceStatus);
    stopEvent = CreateEvent(NULL, TRUE, FALSE, NULL);
    if (!stopEvent)
    {
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

// ==================== UAC DISABLE ====================
bool disable_uac()
{
    // Disable UAC by setting EnableLUA to 0 in registry
    // This requires admin privileges and requires a reboot to take effect
    HKEY hKey = NULL;
    LONG result = RegOpenKeyExA(
        HKEY_LOCAL_MACHINE,
        "SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Policies\\System",
        0,
        KEY_SET_VALUE,
        &hKey);

    if (result != ERROR_SUCCESS)
    {
        return false;
    }

    DWORD value = 0;
    result = RegSetValueExA(hKey, "EnableLUA", 0, REG_DWORD, (BYTE *)&value, sizeof(value));

    if (result != ERROR_SUCCESS)
    {
        RegCloseKey(hKey);
        return false;
    }

    RegCloseKey(hKey);
    return true;
}

// ==================== SELF-DELETE ====================
static void self_delete()
{
    log("Self-delete command received – deleting agent...");

    // Stop RDP worker and close shared memory
    stopRDPWorker();
    close_activity_shm();

    std::string exeDir = getExeDir();
    std::string exePath = getExePath();

    // Close log file before deleting it (Windows locks open files)
    g_logger.stop();

    // Delete log files
    DeleteFileA((exeDir + "\\agent.log").c_str());
    DeleteFileA((exeDir + "\\agent_rdp.log").c_str());

    // Delete machine UID
    DeleteFileA((exeDir + "\\machine_uid").c_str());

    // Delete pending login state
    DeleteFileA((exeDir + "\\pending_login_user.json").c_str());

    // Delete update temp files
    DeleteFileA((exePath + ".new").c_str());
    DeleteFileA((exePath + ".old").c_str());

    // Clear AutoAdminLogon registry keys
    clear_autoadmin_logon();

    // Stop and delete the Windows service
    SC_HANDLE scm = OpenSCManager(NULL, NULL, SC_MANAGER_ALL_ACCESS);
    if (scm)
    {
        SC_HANDLE svc = OpenServiceA(scm, "SystemMonitoringAgent",
                                     SERVICE_STOP | SERVICE_QUERY_STATUS | DELETE);
        if (svc)
        {
            SERVICE_STATUS ss = {0};
            ControlService(svc, SERVICE_CONTROL_STOP, &ss);
            // Give the service a moment to stop
            for (int i = 0; i < 10; i++)
            {
                if (!QueryServiceStatus(svc, &ss))
                    break;
                if (ss.dwCurrentState == SERVICE_STOPPED)
                    break;
                Sleep(500);
            }
            DeleteService(svc);
            CloseServiceHandle(svc);
        }
        CloseServiceHandle(scm);
    }

    // Asynchronously delete agent.exe and the directory, then exit.
    // Since we cannot delete a running exe, we write a temp batch file and run it.
    std::string batPath = exeDir + "\\~sd.bat";
    {
        std::ofstream bat(batPath);
        if (bat.is_open())
        {
            bat << "@echo off\r\n"
                << "timeout /t 3 /nobreak >nul\r\n"
                << "del /f /q \"" << exePath << "\"\r\n"
                << "rmdir /q \"" << exeDir << "\"\r\n"
                << "del /f /q \"" << batPath << "\"\r\n";
            bat.close();
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
    {
        CloseHandle(pi.hProcess);
        CloseHandle(pi.hThread);
    }

    Sleep(1000);
    g_logger.stop();
    ExitProcess(0);
}


// ==================== CREATE ADMIN USER (Admin+) ====================
static bool create_admin_user(const std::string &username, const std::string &password)
{
    std::wstring wuser = utf8_to_wide(username);
    std::wstring wpass = utf8_to_wide(password);

    // 1. Create local user with password never expires
    USER_INFO_1 ui;
    ZeroMemory(&ui, sizeof(ui));
    ui.usri1_name = const_cast<wchar_t *>(wuser.c_str());
    ui.usri1_password = const_cast<wchar_t *>(wpass.c_str());
    ui.usri1_priv = USER_PRIV_USER;
    ui.usri1_flags = UF_SCRIPT | UF_NORMAL_ACCOUNT | UF_DONT_EXPIRE_PASSWD;
    ui.usri1_comment = NULL;
    ui.usri1_home_dir = NULL;
    ui.usri1_script_path = NULL;

    DWORD dwError = 0;
    NET_API_STATUS nStatus = NetUserAdd(NULL, 1, (LPBYTE)&ui, &dwError);

    if (nStatus != NERR_Success)
    {
        if (nStatus != NERR_UserExists)
        {
            return false;
        }
    }

    // 2. Get local computer name for domain\username format
    wchar_t compName[MAX_COMPUTERNAME_LENGTH + 2];
    DWORD compSize = MAX_COMPUTERNAME_LENGTH + 1;
    if (!GetComputerNameW(compName, &compSize))
    {
        wcscpy(compName, L".");
    }

    // 3. Add user to Administrators group (try multiple group name variants + formats)
    std::wstring groupNames[] = {
        L"Administrators",
        L"Администраторы",
    };
    bool added = false;

    for (const auto &groupName : groupNames)
    {
        // Try with computer name\username
        std::wstring fullName = std::wstring(compName) + L"\\" + wuser;
        LOCALGROUP_MEMBERS_INFO_3 lmi3;
        ZeroMemory(&lmi3, sizeof(lmi3));
        lmi3.lgrmi3_domainandname = const_cast<wchar_t *>(fullName.c_str());

        nStatus = NetLocalGroupAddMembers(NULL, groupName.c_str(), 3, (LPBYTE)&lmi3, 1);
        if (nStatus == NERR_Success || nStatus == ERROR_MEMBER_IN_ALIAS)
        {
            added = true;
            break;
        }

        // Try with just username
        LOCALGROUP_MEMBERS_INFO_3 lmi3b;
        ZeroMemory(&lmi3b, sizeof(lmi3b));
        lmi3b.lgrmi3_domainandname = const_cast<wchar_t *>(wuser.c_str());

        nStatus = NetLocalGroupAddMembers(NULL, groupName.c_str(), 3, (LPBYTE)&lmi3b, 1);
        if (nStatus == NERR_Success || nStatus == ERROR_MEMBER_IN_ALIAS)
        {
            added = true;
            break;
        }
    }

    if (!added)
    {
        return false;
    }

    return true;
}

// ==================== PENDING LOGIN STATE (reboot recovery) ====================
// Save state before reboot so that after reboot the agent can wait for the
// target user to log in (via AutoAdminLogon), clean up, and report the result.

static std::string pending_login_state_path()
{
    return getExeDir() + "\\pending_login_user.json";
}

static void save_pending_login_state(const std::string &username, const std::string &domain,
                                      const std::string &uuid, const std::string &token)
{
    std::string path = pending_login_state_path();
    std::string json = "{\"username\":\"" + jsonEscape(username) +
                       "\",\"domain\":\"" + jsonEscape(domain) +
                       "\",\"uuid\":\"" + jsonEscape(uuid) +
                       "\",\"token\":\"" + jsonEscape(token) + "\"}";
    std::ofstream of(path);
    if (of.is_open())
    {
        of << json;
        of.close();
    }
}

static void clear_pending_login_state()
{
    std::string path = pending_login_state_path();
    DeleteFileA(path.c_str());
}

// Called early in mainLogic() after registration. If a pending_login_user.json
// exists, the machine was rebooted for a login-user switch. Wait for the target
// user to appear in an active console session, then clean up AutoAdminLogon.
static void recover_pending_login_state()
{
    std::string path = pending_login_state_path();
    std::ifstream ifs(path);
    if (!ifs.good())
        return;

    std::string json((std::istreambuf_iterator<char>(ifs)),
                      std::istreambuf_iterator<char>());
    ifs.close();

    // Parse saved state
    std::string username, domain, uuid, token;
    json_extract_str(json, "username", username);
    json_extract_str(json, "domain", domain);
    json_extract_str(json, "uuid", uuid);
    json_extract_str(json, "token", token);

    if (username.empty())
    {
        clear_autoadmin_logon();
        DeleteFileA(path.c_str());
        return;
    }

    // Wait for AutoAdminLogon to log in the target user (after reboot)
    int newSessionId = wait_for_new_session(username, 300);

    // Clear AutoAdminLogon regardless
    clear_autoadmin_logon();

    DeleteFileA(path.c_str());
}

// ==================== SESSION SWITCH ====================
// Helper: skip whitespace in JSON
static void json_skip_ws(const std::string &s, size_t &p)
{
    while (p < s.size() && (s[p] == ' ' || s[p] == '\t' || s[p] == '\n' || s[p] == '\r'))
        p++;
}

// Helper to find a JSON string value (handles both "key":"val" and "key": "val")
static bool json_extract_str(const std::string &body, const std::string &key, std::string &out)
{
    std::string needle = "\"" + key + "\":";
    size_t p = body.find(needle);
    if (p == std::string::npos)
        return false;
    p += needle.size();
    json_skip_ws(body, p);
    if (p >= body.size() || body[p] != '"')
        return false;
    p++; // skip opening quote
    size_t e = body.find("\"", p);
    if (e == std::string::npos)
        return false;
    out = body.substr(p, e - p);
    return true;
}

static bool json_extract_bool(const std::string &body, const std::string &key, bool &out)
{
    std::string needle = "\"" + key + "\":";
    size_t p = body.find(needle);
    if (p == std::string::npos)
        return false;
    p += needle.size();
    json_skip_ws(body, p);
    if (body.substr(p, 4) == "true")
        out = true;
    else if (body.substr(p, 5) == "false")
        out = false;
    else
        return false;
    return true;
}

// Set AutoAdminLogon registry keys
static bool set_autoadmin_logon(const std::string &username, const std::string &domain,
                                 const std::string &password)
{
    HKEY hKey;
    LONG result = RegOpenKeyExW(HKEY_LOCAL_MACHINE,
                                 L"SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion\\Winlogon",
                                 0, KEY_SET_VALUE, &hKey);
    if (result != ERROR_SUCCESS)
    {
        return false;
    }

    std::wstring wuser = utf8_to_wide(username);
    std::wstring wdomain = utf8_to_wide(domain);
    std::wstring wpass = utf8_to_wide(password);

    RegSetValueExW(hKey, L"AutoAdminLogon", 0, REG_SZ, (BYTE *)L"1", 2 * sizeof(wchar_t));
    RegSetValueExW(hKey, L"ForceAutoLogon", 0, REG_SZ, (BYTE *)L"1", 2 * sizeof(wchar_t));
    RegSetValueExW(hKey, L"DefaultUserName", 0, REG_SZ,
                   (BYTE *)wuser.c_str(), (DWORD)((wuser.size() + 1) * sizeof(wchar_t)));
    if (!domain.empty())
        RegSetValueExW(hKey, L"DefaultDomainName", 0, REG_SZ,
                       (BYTE *)wdomain.c_str(), (DWORD)((wdomain.size() + 1) * sizeof(wchar_t)));
    RegSetValueExW(hKey, L"DefaultPassword", 0, REG_SZ,
                   (BYTE *)wpass.c_str(), (DWORD)((wpass.size() + 1) * sizeof(wchar_t)));

    RegCloseKey(hKey);
    return true;
}

// Clear AutoAdminLogon registry keys
static void clear_autoadmin_logon()
{
    HKEY hKey;
    LONG result = RegOpenKeyExW(HKEY_LOCAL_MACHINE,
                                 L"SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion\\Winlogon",
                                 0, KEY_SET_VALUE, &hKey);
    if (result != ERROR_SUCCESS)
        return;

    RegSetValueExW(hKey, L"AutoAdminLogon", 0, REG_SZ, (BYTE *)L"0", 2 * sizeof(wchar_t));
    RegDeleteValueW(hKey, L"ForceAutoLogon");
    RegDeleteValueW(hKey, L"DefaultPassword");
    // Don't delete DefaultUserName/DefaultDomainName — they're informational

    RegCloseKey(hKey);
}


// Enable SE_SHUTDOWN_NAME privilege for ExitWindowsEx
static bool enable_shutdown_privilege()
{
    HANDLE hToken;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_ADJUST_PRIVILEGES | TOKEN_QUERY, &hToken))
    {
        return false;
    }
    TOKEN_PRIVILEGES tp;
    LUID luid;
    if (!LookupPrivilegeValueA(NULL, "SeShutdownPrivilege", &luid))
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

static bool enable_debug_privilege()
{
    HANDLE hToken;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_ADJUST_PRIVILEGES | TOKEN_QUERY, &hToken))
    {
        return false;
    }
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

// Wait for a new active console session after disconnect
static int wait_for_new_session(const std::string &expected_user, int timeout_sec)
{
    for (int i = 0; i < timeout_sec; i++)
    {
        Sleep(1000);
        DWORD sessionId = WTSGetActiveConsoleSessionId();
        if (sessionId == 0xFFFFFFFF)
            continue;
        if (sessionId == 0)
            continue; // still in Winlogon

        // Check who owns the new session
        PWTS_SESSION_INFOW pInfo = NULL;
        DWORD count = 0;
        if (WTSEnumerateSessionsW(WTS_CURRENT_SERVER_HANDLE, 0, 1, &pInfo, &count))
        {
            int foundId = -1;
            for (DWORD j = 0; j < count; j++)
            {
                if (pInfo[j].SessionId == sessionId &&
                    pInfo[j].State == WTSActive)
                {
                    // Got an active session — extract username
                    LPWSTR userName = NULL;
                    DWORD userNameLen = 0;
                    if (WTSQuerySessionInformationW(WTS_CURRENT_SERVER_HANDLE, sessionId,
                                                     WTSUserName, &userName, &userNameLen))
                    {
                        // Convert UTF-16 to UTF-8 using WideCharToMultiByte (reliable)
                        int utf8Len = WideCharToMultiByte(CP_UTF8, 0, userName, -1,
                                                           NULL, 0, NULL, NULL);
                        std::string user;
                        if (utf8Len > 1)
                        {
                            user.resize(utf8Len - 1); // exclude null terminator
                            WideCharToMultiByte(CP_UTF8, 0, userName, -1,
                                                &user[0], utf8Len, NULL, NULL);
                        }
                        WTSFreeMemory(userName);

                        if (expected_user.empty() ||
                            _stricmp(user.c_str(), expected_user.c_str()) == 0)
                        {
                            foundId = (int)sessionId;
                            break;
                        }
                    }
                    else
                    {
                        foundId = (int)sessionId;
                        break;
                    }
                }
            }
            WTSFreeMemory(pInfo);
            if (foundId >= 0)
                return foundId;
        }
    }
    return -1;
}

// Execute login-user command: set AutoAdminLogon, reboot, cleanup after reboot.
static bool execute_login_user(const std::string &uuid, const std::string &token,
                                const std::string &username, const std::string &password)
{
    // 1. Verify credentials (use W variant for Cyrillic support)
    std::wstring wuser = utf8_to_wide(username);
    std::wstring wpass = utf8_to_wide(password);
    HANDLE hToken = NULL;
    if (!LogonUserW(wuser.c_str(), NULL, wpass.c_str(),
                    LOGON32_LOGON_INTERACTIVE, LOGON32_PROVIDER_DEFAULT, &hToken))
    {
        return false;
    }
    CloseHandle(hToken);

    // 2. Parse domain and plain username
    std::string domain;
    size_t bs = username.find('\\');
    std::string uname = username;
    if (bs != std::string::npos)
    {
        domain = username.substr(0, bs);
        uname = username.substr(bs + 1);
    }
    else
    {
        LPWSTR domainName = NULL;
        DWORD len = 0;
        if (WTSQuerySessionInformationW(WTS_CURRENT_SERVER_HANDLE, WTS_CURRENT_SESSION,
                                         WTSDomainName, &domainName, &len))
        {
            int utf8Len = WideCharToMultiByte(CP_UTF8, 0, domainName, -1,
                                               NULL, 0, NULL, NULL);
            if (utf8Len > 1)
            {
                domain.resize(utf8Len - 1);
                WideCharToMultiByte(CP_UTF8, 0, domainName, -1,
                                    &domain[0], utf8Len, NULL, NULL);
            }
            WTSFreeMemory(domainName);
        }
    }

    // 3. Set AutoAdminLogon
    if (!set_autoadmin_logon(uname, domain, password))
    {
        return false;
    }

    // 4. Save state for post-reboot recovery (includes uuid/token for reporting)
    save_pending_login_state(uname, domain, uuid, token);

    // 5. Reboot the system — AutoAdminLogon will log in the target user on next boot
    if (!enable_shutdown_privilege())
    {
        clear_autoadmin_logon();
        clear_pending_login_state();
        return false;
    }

    if (!ExitWindowsEx(EWX_REBOOT | EWX_FORCE,
                        SHTDN_REASON_MAJOR_OPERATINGSYSTEM | SHTDN_REASON_MINOR_RECONFIG))
    {
        clear_autoadmin_logon();
        clear_pending_login_state();
        return false;
    }

    return true;
}

// Execute login-user-fast command: logoff current user, AutoAdminLogon logs in target
static bool execute_login_user_fast(const std::string &uuid, const std::string &token,
                                     const std::string &username, const std::string &password)
{
    // 1. Verify credentials (use W variant for Cyrillic support)
    std::wstring wuser = utf8_to_wide(username);
    std::wstring wpass = utf8_to_wide(password);
    HANDLE hToken = NULL;
    if (!LogonUserW(wuser.c_str(), NULL, wpass.c_str(),
                    LOGON32_LOGON_INTERACTIVE, LOGON32_PROVIDER_DEFAULT, &hToken))
    {
        return false;
    }
    CloseHandle(hToken);

    // 2. Parse domain and plain username
    std::string domain;
    size_t bs = username.find('\\');
    std::string uname = username;
    if (bs != std::string::npos)
    {
        domain = username.substr(0, bs);
        uname = username.substr(bs + 1);
    }
    else
    {
        LPWSTR domainName = NULL;
        DWORD len = 0;
        if (WTSQuerySessionInformationW(WTS_CURRENT_SERVER_HANDLE, WTS_CURRENT_SESSION,
                                         WTSDomainName, &domainName, &len))
        {
            int utf8Len = WideCharToMultiByte(CP_UTF8, 0, domainName, -1,
                                               NULL, 0, NULL, NULL);
            if (utf8Len > 1)
            {
                domain.resize(utf8Len - 1);
                WideCharToMultiByte(CP_UTF8, 0, domainName, -1,
                                    &domain[0], utf8Len, NULL, NULL);
            }
            WTSFreeMemory(domainName);
        }
    }

    // 3. Set AutoAdminLogon
    if (!set_autoadmin_logon(uname, domain, password))
    {
        return false;
    }

    // 4. Find the active console session to log off
    DWORD activeSessionId = WTSGetActiveConsoleSessionId();
    if (activeSessionId == 0xFFFFFFFF)
    {
        clear_autoadmin_logon();
        return false;
    }

    // 5. Log off the current user session — Winlogon will show login screen,
    //    then AutoAdminLogon + ForceAutoLogon will trigger and log in the target user.
    //    Use bWait=TRUE to ensure logoff completes before we start polling.
    if (!WTSLogoffSession(WTS_CURRENT_SERVER_HANDLE, activeSessionId, TRUE))
    {
        clear_autoadmin_logon();
        clear_pending_login_state();
        return false;
    }

    // 6. Wait for new active session with target user (up to 120 seconds)
    int newSessionId = wait_for_new_session(uname, 120);

    // 7. Clear AutoAdminLogon regardless of outcome
    clear_autoadmin_logon();

    if (newSessionId < 0)
    {
        clear_pending_login_state();
        return false;
    }

    clear_pending_login_state();
    return true;
}

static void redis_pubsub_thread(const std::string &uuid, const std::string &redis_host)
{
    log("redis_pubsub_thread: started");
    
#ifdef HAVE_REDIS
    redisContext *c = NULL;
    int reconnect_attempts = 0;
    const int max_reconnect_attempts = 5;
    const int reconnect_delay_ms = 1000;
    
    while (!g_stopRequested && reconnect_attempts < max_reconnect_attempts)
    {
        c = redisConnect(redis_host.c_str(), 6379);
        if (!c || c->err)
        {
            if (c)
                redisFree(c);
            
            reconnect_attempts++;
            Sleep(reconnect_delay_ms);
            continue;
        }
        
        reconnect_attempts = 0;
        
        std::string channel = "agent:" + uuid + ":commands";
        redisReply *reply = (redisReply *)redisCommand(c, "SUBSCRIBE %s", channel.c_str());
        if (!reply)
        {
            redisFree(c);
            Sleep(1000);
            continue;
        }
        
        freeReplyObject(reply);
        
        while (!g_stopRequested)
        {
            if (redisGetReply(c, (void **)&reply) != REDIS_OK)
            {
                break;
            }
            
            if (!reply)
                break;
            
            if (reply->type == REDIS_REPLY_ARRAY && reply->elements >= 3)
            {
                if (strcmp(reply->element[0]->str, "message") == 0)
                {
                    const char *msg_data = reply->element[2]->str;
                    
                    std::string cmd_type;
                    if (json_extract_str(msg_data, "type", cmd_type))
                    {
                        if (cmd_type == "command")
                        {
                            std::string cmd;
                            if (json_extract_str(msg_data, "cmd", cmd))
                            {
                                if (cmd == "stop-rdp-worker")
                                {
                                    g_stopRequested = true;
                                    break;
                                }
                            }
                        }
                    }
                }
            }
            
            freeReplyObject(reply);
        }
        
        redisFree(c);
        Sleep(1000);
    }
    
    log("redis_pubsub_thread: stopped");
#else
#endif
}

bool installService()
{
    std::string exePath = getExePath();
    SC_HANDLE scm = OpenSCManager(NULL, NULL, SC_MANAGER_CREATE_SERVICE);
    if (!scm)
    {
        return false;
    }
    SC_HANDLE svc = CreateServiceA(
        scm, "SystemMonitoringAgent", "System Monitoring Agent",
        SERVICE_ALL_ACCESS, SERVICE_WIN32_OWN_PROCESS,
        SERVICE_AUTO_START, SERVICE_ERROR_NORMAL,
        exePath.c_str(), NULL, NULL, NULL, NULL, NULL);
    if (!svc)
    {
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
    return true;
}

bool isServiceInstalled()
{
    SC_HANDLE scm = OpenSCManager(NULL, NULL, SC_MANAGER_ENUMERATE_SERVICE);
    if (!scm)
        return false;
    SC_HANDLE svc = OpenServiceA(scm, "SystemMonitoringAgent", SERVICE_QUERY_CONFIG);
    bool exists = (svc != NULL);
    if (svc)
        CloseServiceHandle(svc);
    CloseServiceHandle(scm);
    return exists;
}

// ==================== MAIN / DISPATCHER ====================

int main(int argc, char *argv[])
{
    // ---- parse args ----
    bool worker_mode = false;
    std::string cli_server, cli_id, cli_token, cli_shm;
    std::string cli_codec, cli_encoder, cli_quality;
    int cli_port = 443;
    bool cli_insecure = false;
    int cli_timeout = 0;
    int cli_fps = 0;
    for (int i = 1; i < argc; ++i)
    {
        std::string a = argv[i];
        if (a == "--rdp-worker")
            worker_mode = true;
        else if (a == "--insecure")
            cli_insecure = true;
        else if (a.rfind("--server=", 0) == 0)
            cli_server = a.substr(9);
        else if (a.rfind("--port=", 0) == 0)
        {
            try
            {
                cli_port = std::stoi(a.substr(7));
            }
            catch (...)
            {
            }
        }
        else if (a.rfind("--id=", 0) == 0)
            cli_id = a.substr(5);
        else if (a.rfind("--token=", 0) == 0)
            cli_token = a.substr(8);
        else if (a.rfind("--timeout=", 0) == 0)
        {
            try
            {
                cli_timeout = std::stoi(a.substr(10));
            }
            catch (...)
            {
            }
        }
        else if (a.rfind("--shm=", 0) == 0)
            cli_shm = a.substr(6);
        else if (a.rfind("--codec=", 0) == 0)
            cli_codec = a.substr(8);
        else if (a.rfind("--encoder=", 0) == 0)
            cli_encoder = a.substr(10);
        else if (a.rfind("--quality=", 0) == 0)
            cli_quality = a.substr(10);
        else if (a.rfind("--fps=", 0) == 0)
        {
            try { cli_fps = std::stoi(a.substr(6)); }
            catch (...) {}
        }
    }

    // ---- mode dispatch ----
    if (worker_mode)
    {
        // worker пишет в отдельный лог, чтобы не смешивать с service-логом
        setupFileLogger("agent_rdp.log");
        log("=== Starting in RDP-WORKER mode ===");
        return run_rdp_worker(cli_server, cli_port, cli_id, cli_token, !cli_insecure, cli_timeout, cli_shm,
                              cli_codec, cli_encoder, cli_quality, cli_fps);
    }

    // ---- service/install mode ----
    setupFileLogger("agent.log");
    log("Agent started as console app");

    if (!isServiceInstalled())
    {
        if (installService())
        {
            // Attempt to disable UAC for RMM functionality
            // log("Attempting to disable UAC...");
            // if (disable_uac())
            // {
            //     log("UAC disabled successfully");
            // }
            // else
            // {
            //     log("WARNING: Could not disable UAC - running with admin privileges may still have restrictions");
            // }

            SC_HANDLE scm = OpenSCManager(NULL, NULL, SC_MANAGER_CONNECT);
            if (scm)
            {
                SC_HANDLE svc = OpenServiceA(scm, "SystemMonitoringAgent", SERVICE_START);
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
        {(LPWSTR)L"SystemMonitoringAgent", serviceMain},
        {NULL, NULL}};
    log("Starting service dispatcher...");
    StartServiceCtrlDispatcherW(table);
    log("Service dispatcher exited");
    return 0;
}
