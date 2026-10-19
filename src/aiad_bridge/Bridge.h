// Agent bridge `aiad-bridge/1` (pattern of DASE50): the app does not host MCP. It exposes a loopback
// HTTP endpoint, and the standalone `aiad-mcp.exe` (stdio) translates MCP into bridge calls.
//
//   POST /bridge   Authorization: Bearer <token>
//   {"method":"catalog"}                          -> {"ok":true,"result":[...tools...]}
//   {"method":"execute","name":..,"args":{..}}    -> {"ok":..,"text":..,"result":..,"images":[{mimeType,data}],"error":..}
//
// Security: bound to 127.0.0.1 only; per-instance random token; requests carrying an Origin header
// are refused (browsers, DNS rebinding); Host must be the loopback address.
// Discovery: %APPDATA%\Tootega\AIAD\bridge\aiad-<pid>.json = {protocol,url,token,workspacePath,pid,startedAt}.
#pragma once

#include "Json.h"
#include "MainThreadQueue.h"

#include <atomic>
#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <thread>
#include <vector>

namespace aiad::bridge
{

inline constexpr const char* kProtocol = "aiad-bridge/1";

class BridgeServer
{
public:
    // The handler's answer: the response object, or `later`, which the connection thread runs after the
    // handler (off the main thread) to make the response.
    struct Reply
    {
        Reply(Json response) : json(std::move(response)) {}
        Json json;
        std::function<Json()> later;
    };
    // Runs on the main thread (posted through the queue).
    using Handler = std::function<Reply(const Json& request)>;

    ~BridgeServer();
    bool Start(MainThreadQueue& queue, Handler handler, const std::filesystem::path& workspace, std::string& error);
    void Stop();
    // The open document's folder changed: aiad-mcp instances started there find this app first.
    void SetWorkspace(const std::filesystem::path& workspace);
    bool Running() const { return m_listen != ~uintptr_t(0); }
    const std::string& Url() const { return m_url; }
    const std::string& Token() const { return m_token; }
    int Port() const { return m_port; }
    uint64_t Requests() const { return m_requests; }

private:
    void AcceptLoop();
    void Serve(uintptr_t socket);

    uintptr_t m_listen = ~uintptr_t(0);
    std::thread m_thread;
    std::atomic<bool> m_stopping = false;
    std::atomic<int> m_active = 0;
    std::atomic<uint64_t> m_requests = 0;
    MainThreadQueue* m_queue = nullptr;
    Handler m_handler;
    std::string m_url, m_token;
    int m_port = 0;
    std::filesystem::path m_discoveryFile;
    int64_t m_startedAt = 0;
};

struct Endpoint
{
    std::string url, token, workspace;
    unsigned long pid = 0;
};

// Live app instances, best match first: workspace equal to (or containing) `cwd`, then the newest.
// Stale discovery files (dead process) are deleted.
std::vector<Endpoint> Discover(const std::filesystem::path& cwd);

// One bridge call (WinHTTP). On transport failure returns {"ok":false,"error":..,"transport":true}.
Json Call(const Endpoint& endpoint, const Json& request, int timeoutMs = 120000);

}  // namespace aiad::bridge
