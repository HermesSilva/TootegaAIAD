#include "Bridge.h"

#include "Paths.h"
#include "Text.h"

#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <winhttp.h>

#include <algorithm>
#include <chrono>
#include <format>
#include <future>

namespace aiad::bridge
{

namespace
{

std::filesystem::path DiscoveryDir()
{
    std::filesystem::path dir = AppDataDir() / "bridge";
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    return dir;
}

std::string Lower(std::string s)
{
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return char(std::tolower(c)); });
    return s;
}

bool SendAll(SOCKET s, const std::string& data)
{
    size_t sent = 0;
    while (sent < data.size())
    {
        const int n = send(s, data.data() + sent, int(data.size() - sent), 0);
        if (n <= 0)
            return false;
        sent += size_t(n);
    }
    return true;
}

void Reply(SOCKET s, int status, const char* reason, const Json& body)
{
    const std::string payload = body.dump(-1, ' ', false, Json::error_handler_t::replace);
    SendAll(s, std::format("HTTP/1.1 {} {}\r\nContent-Type: application/json; charset=utf-8\r\nContent-Length: {}\r\nConnection: close\r\n\r\n",
                           status, reason, payload.size()) +
                   payload);
}

}  // namespace

BridgeServer::~BridgeServer()
{
    Stop();
}

bool BridgeServer::Start(MainThreadQueue& queue, Handler handler, const std::filesystem::path& workspace, std::string& error)
{
    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0)
    {
        error = "WSAStartup failed";
        return false;
    }
    SOCKET s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = 0;  // ephemeral
    if (s == INVALID_SOCKET || bind(s, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0 || listen(s, 16) != 0)
    {
        error = std::format("bridge socket failed (WSA {})", WSAGetLastError());
        if (s != INVALID_SOCKET)
            closesocket(s);
        return false;
    }
    int length = sizeof(addr);
    getsockname(s, reinterpret_cast<sockaddr*>(&addr), &length);
    m_port = ntohs(addr.sin_port);
    m_listen = uintptr_t(s);
    m_queue = &queue;
    m_handler = std::move(handler);
    m_token = RandomHex(24);
    m_url = std::format("http://127.0.0.1:{}/bridge", m_port);
    m_stopping = false;
    m_thread = std::thread([this] { AcceptLoop(); });

    m_discoveryFile = DiscoveryDir() / std::format("aiad-{}.json", GetCurrentProcessId());
    const Json discovery = {{"protocol", kProtocol},
                            {"url", m_url},
                            {"token", m_token},
                            {"workspacePath", Narrow(workspace.wstring())},
                            {"pid", GetCurrentProcessId()},
                            {"startedAt", std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count()}};
    WriteTextFile(m_discoveryFile, discovery.dump(2));
    return true;
}

void BridgeServer::Stop()
{
    if (!Running())
        return;
    m_stopping = true;
    closesocket(SOCKET(m_listen));
    m_listen = ~uintptr_t(0);
    if (m_thread.joinable())
        m_thread.join();
    // Connection threads give up waiting for the main thread when stopping is set.
    for (int i = 0; i < 300 && m_active > 0; ++i)
        Sleep(10);
    std::error_code ec;
    std::filesystem::remove(m_discoveryFile, ec);
    WSACleanup();
}

void BridgeServer::AcceptLoop()
{
    while (!m_stopping)
    {
        const SOCKET client = accept(SOCKET(m_listen), nullptr, nullptr);
        if (client == INVALID_SOCKET)
            break;
        ++m_active;
        std::thread([this, client] {
            Serve(uintptr_t(client));
            closesocket(client);
            --m_active;
        }).detach();
    }
}

void BridgeServer::Serve(uintptr_t socketHandle)
{
    const SOCKET s = SOCKET(socketHandle);
    DWORD timeout = 30000;
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&timeout), sizeof(timeout));

    std::string data;
    char buffer[16384];
    size_t headerEnd = std::string::npos;
    while ((headerEnd = data.find("\r\n\r\n")) == std::string::npos)
    {
        const int n = recv(s, buffer, sizeof(buffer), 0);
        if (n <= 0 || data.size() > (1 << 20))
            return;
        data.append(buffer, size_t(n));
    }
    const std::string head = data.substr(0, headerEnd);
    std::string body = data.substr(headerEnd + 4);
    size_t contentLength = 0;
    std::string authorization, origin, host, requestLine = head.substr(0, head.find("\r\n"));
    for (size_t pos = head.find("\r\n"); pos != std::string::npos && pos < head.size();)
    {
        const size_t next = head.find("\r\n", pos + 2);
        const std::string line = head.substr(pos + 2, next == std::string::npos ? std::string::npos : next - pos - 2);
        const size_t colon = line.find(':');
        if (colon != std::string::npos)
        {
            const std::string key = Lower(line.substr(0, colon));
            std::string value = line.substr(colon + 1);
            value.erase(0, value.find_first_not_of(' '));
            if (key == "content-length")
                contentLength = size_t(std::stoull(value));
            else if (key == "authorization")
                authorization = value;
            else if (key == "origin")
                origin = value;
            else if (key == "host")
                host = value;
        }
        pos = next;
    }
    if (contentLength > (64u << 20))
        return Reply(s, 413, "Payload Too Large", {{"ok", false}, {"error", "request too large"}});
    while (body.size() < contentLength)
    {
        const int n = recv(s, buffer, sizeof(buffer), 0);
        if (n <= 0)
            return;
        body.append(buffer, size_t(n));
    }

    if (!requestLine.starts_with("POST /bridge "))
        return Reply(s, 404, "Not Found", {{"ok", false}, {"error", "use POST /bridge"}});
    if (!origin.empty())
        return Reply(s, 403, "Forbidden", {{"ok", false}, {"error", "browser origins are not accepted"}});
    if (host != std::format("127.0.0.1:{}", m_port) && host != std::format("localhost:{}", m_port))
        return Reply(s, 403, "Forbidden", {{"ok", false}, {"error", "bad host"}});
    if (authorization != "Bearer " + m_token)
        return Reply(s, 401, "Unauthorized", {{"ok", false}, {"error", "missing or wrong bridge token"}});
    const Json request = ParseJson(body);
    if (request.is_discarded() || !request.is_object())
        return Reply(s, 400, "Bad Request", {{"ok", false}, {"error", "body must be a JSON object"}});

    ++m_requests;
    auto promise = std::make_shared<std::promise<Json>>();
    std::future<Json> result = promise->get_future();
    m_queue->Post([this, promise, request] {
        try
        {
            promise->set_value(m_handler(request));
        }
        catch (const std::exception& e)
        {
            promise->set_value(Json{{"ok", false}, {"error", e.what()}});
        }
    });
    // Commands are short (long work runs as jobs); give up only when the app is closing.
    while (result.wait_for(std::chrono::milliseconds(100)) != std::future_status::ready)
        if (m_stopping)
            return Reply(s, 503, "Service Unavailable", {{"ok", false}, {"error", "the app is closing"}});
    Reply(s, 200, "OK", result.get());
}

std::vector<Endpoint> Discover(const std::filesystem::path& cwd)
{
    struct Candidate
    {
        Endpoint endpoint;
        int score;
        int64_t startedAt;
    };
    std::vector<Candidate> found;
    std::error_code ec;
    const std::wstring here = std::filesystem::weakly_canonical(cwd, ec).wstring();
    for (const auto& entry : std::filesystem::directory_iterator(DiscoveryDir(), ec))
    {
        if (entry.path().extension() != ".json")
            continue;
        const Json j = ParseJson(ReadTextFile(entry.path()));
        if (j.is_discarded() || j.value("protocol", "") != kProtocol)
            continue;
        const unsigned long pid = j.value("pid", 0ul);
        HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
        DWORD code = 0;
        const bool alive = process && GetExitCodeProcess(process, &code) && code == STILL_ACTIVE;
        if (process)
            CloseHandle(process);
        if (!alive)
        {
            std::filesystem::remove(entry.path(), ec);
            continue;
        }
        Candidate c{{j.value("url", ""), j.value("token", ""), j.value("workspacePath", ""), pid}, 0, j.value("startedAt", int64_t(0))};
        const std::wstring workspace = std::filesystem::weakly_canonical(Widen(c.endpoint.workspace), ec).wstring();
        if (!workspace.empty() && _wcsicmp(workspace.c_str(), here.c_str()) == 0)
            c.score = 2;
        else if (!workspace.empty() && here.size() > workspace.size() && _wcsnicmp(here.c_str(), workspace.c_str(), workspace.size()) == 0)
            c.score = 1;
        found.push_back(c);
    }
    std::sort(found.begin(), found.end(), [](const Candidate& a, const Candidate& b) {
        return a.score != b.score ? a.score > b.score : a.startedAt > b.startedAt;
    });
    std::vector<Endpoint> list;
    for (const Candidate& c : found)
        list.push_back(c.endpoint);
    return list;
}

Json Call(const Endpoint& endpoint, const Json& request, int timeoutMs)
{
    auto fail = [](std::string message) { return Json{{"ok", false}, {"error", std::move(message)}, {"transport", true}}; };
    URL_COMPONENTS url{sizeof(url)};
    wchar_t hostName[256], path[1024];
    url.lpszHostName = hostName;
    url.dwHostNameLength = 256;
    url.lpszUrlPath = path;
    url.dwUrlPathLength = 1024;
    const std::wstring wideUrl = Widen(endpoint.url);
    if (!WinHttpCrackUrl(wideUrl.c_str(), 0, 0, &url))
        return fail("bad bridge url " + endpoint.url);
    HINTERNET session = WinHttpOpen(L"aiad-mcp/1", WINHTTP_ACCESS_TYPE_NO_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!session)
        return fail("WinHttpOpen: " + LastErrorText());
    WinHttpSetTimeouts(session, 5000, 5000, timeoutMs, timeoutMs);
    HINTERNET connection = WinHttpConnect(session, hostName, url.nPort, 0);
    HINTERNET handle = connection ? WinHttpOpenRequest(connection, L"POST", path, nullptr, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, 0) : nullptr;
    Json response;
    if (!handle)
        response = fail("bridge connection: " + LastErrorText());
    else
    {
        const std::string body = request.dump(-1, ' ', false, Json::error_handler_t::replace);
        const std::wstring headers = Widen("Content-Type: application/json\r\nAuthorization: Bearer " + endpoint.token + "\r\n");
        if (!WinHttpSendRequest(handle, headers.c_str(), DWORD(-1), const_cast<char*>(body.data()), DWORD(body.size()), DWORD(body.size()), 0) ||
            !WinHttpReceiveResponse(handle, nullptr))
            response = fail("bridge request: " + LastErrorText());
        else
        {
            std::string text;
            for (DWORD available = 0; WinHttpQueryDataAvailable(handle, &available) && available > 0;)
            {
                std::string chunk(available, '\0');
                DWORD read = 0;
                if (!WinHttpReadData(handle, chunk.data(), available, &read))
                    break;
                text.append(chunk.data(), read);
            }
            response = ParseJson(text);
            if (response.is_discarded())
                response = fail("bridge answered with something that is not JSON");
        }
    }
    if (handle)
        WinHttpCloseHandle(handle);
    if (connection)
        WinHttpCloseHandle(connection);
    WinHttpCloseHandle(session);
    return response;
}

}  // namespace aiad::bridge
