// aiad-mcp.exe - standalone MCP server (stdio, newline-delimited JSON-RPC 2.0) that forwards every
// tool call to the running Tootega AIAD app through the loopback bridge (aiad-bridge/1).
//
// The tool list is the app's Command Bus catalog, so it is never written twice. The app is found by
// AIAD_BRIDGE_URL + AIAD_BRIDGE_TOKEN (set when the app itself spawns the agent) or else by the
// discovery files (instance whose workspace matches the current directory, then the newest). A
// failed call re-discovers once. With no app running, the last catalog the app wrote is listed and
// calls answer with an actionable error.
#include "Bridge.h"
#include "Json.h"
#include "Paths.h"
#include "Text.h"

#include <windows.h>

#include <cstdio>
#include <fcntl.h>
#include <io.h>
#include <iostream>
#include <optional>
#include <string>

using namespace aiad;

namespace
{

constexpr const char* kServerVersion = "0.1.0";

std::optional<bridge::Endpoint> g_endpoint;

std::string EnvVar(const char* name)
{
    char buffer[2048];
    const DWORD n = GetEnvironmentVariableA(name, buffer, sizeof(buffer));
    return n > 0 && n < sizeof(buffer) ? std::string(buffer, n) : std::string();
}

bool Resolve(bool rediscover)
{
    if (g_endpoint && !rediscover)
        return true;
    const std::string url = EnvVar("AIAD_BRIDGE_URL"), token = EnvVar("AIAD_BRIDGE_TOKEN");
    if (!url.empty() && !token.empty() && !rediscover)
    {
        g_endpoint = bridge::Endpoint{url, token, "", 0};
        return true;
    }
    const auto found = bridge::Discover(std::filesystem::current_path());
    g_endpoint.reset();
    if (!found.empty())
        g_endpoint = found.front();
    return g_endpoint.has_value();
}

Json BridgeCall(const Json& request)
{
    if (!Resolve(false))
        return {{"ok", false}, {"error", "Tootega AIAD is not running. Start aiad.exe (Dist folder) and try again."}};
    Json response = bridge::Call(*g_endpoint, request);
    if (response.value("transport", false) && Resolve(true))
        response = bridge::Call(*g_endpoint, request);
    if (response.value("transport", false))
        response["error"] = "Cannot reach Tootega AIAD (" + response.value("error", std::string()) + "). Is aiad.exe still running?";
    return response;
}

void Send(const Json& message)
{
    const std::string line = message.dump(-1, ' ', false, Json::error_handler_t::replace) + "\n";
    std::fwrite(line.data(), 1, line.size(), stdout);
    std::fflush(stdout);
}

Json ToolList()
{
    const Json response = BridgeCall({{"method", "catalog"}});
    Json catalog = response.value("ok", false) ? response["result"] : ParseJson(ReadTextFile(AppDataDir() / "catalog.json"));
    if (catalog.is_discarded() || !catalog.is_array())
        catalog = Json::array();
    return {{"tools", catalog}};
}

Json ToolCall(const Json& params)
{
    const Json response = BridgeCall({{"method", "execute"}, {"name", params.value("name", "")}, {"args", params.value("arguments", Json::object())}});
    Json content = Json::array();
    const bool ok = response.value("ok", false);
    if (ok)
    {
        std::string text = response.value("text", std::string());
        if (text.empty() && response.contains("result"))
            text = response["result"].dump(2);
        content.push_back({{"type", "text"}, {"text", text.empty() ? "Done." : text}});
        if (response.contains("images"))
            for (const Json& image : response["images"])
                content.push_back({{"type", "image"}, {"data", image.value("data", "")}, {"mimeType", image.value("mimeType", "image/png")}});
    }
    else
        content.push_back({{"type", "text"}, {"text", response.value("error", std::string("failed"))}});
    Json result = {{"content", content}, {"isError", !ok}};
    if (ok && response.contains("result") && response["result"].is_object())
        result["structuredContent"] = response["result"];
    return result;
}

}  // namespace

int main()
{
    _setmode(_fileno(stdout), _O_BINARY);
    _setmode(_fileno(stdin), _O_BINARY);
    std::string line;
    while (std::getline(std::cin, line))
    {
        if (!line.empty() && line.back() == '\r')
            line.pop_back();
        if (line.empty())
            continue;
        const Json message = ParseJson(line);
        if (message.is_discarded() || !message.is_object())
        {
            Send({{"jsonrpc", "2.0"}, {"id", nullptr}, {"error", {{"code", -32700}, {"message", "parse error"}}}});
            continue;
        }
        if (!message.contains("id"))
            continue;  // notification (initialized, cancelled, ...)
        const Json id = message["id"];
        const std::string method = message.value("method", "");
        const Json params = message.value("params", Json::object());
        Json result;
        if (method == "initialize")
            result = {{"protocolVersion", params.value("protocolVersion", std::string("2025-06-18"))},
                      {"capabilities", {{"tools", {{"listChanged", false}}}}},
                      {"serverInfo", {{"name", "aiad"}, {"title", "Tootega AIAD"}, {"version", kServerVersion}}},
                      {"instructions",
                       "Tools of the Tootega AIAD CAD/CAE app. They operate the open model through the app's Command Bus "
                       "(the same commands, undo and audit trail as the user). Read before you change; after a change, check "
                       "with aiad_viewport_snapshot. Lengths are in millimetres and angles in degrees unless a tool says otherwise."}};
        else if (method == "ping")
            result = Json::object();
        else if (method == "tools/list")
            result = ToolList();
        else if (method == "tools/call")
            result = ToolCall(params);
        else
        {
            Send({{"jsonrpc", "2.0"}, {"id", id}, {"error", {{"code", -32601}, {"message", "method not found: " + method}}}});
            continue;
        }
        Send({{"jsonrpc", "2.0"}, {"id", id}, {"result", result}});
    }
    return 0;
}
