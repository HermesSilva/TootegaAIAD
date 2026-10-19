// End to end without the app: Command Bus <- bridge (loopback, token) <- aiad-mcp.exe (stdio MCP).
// What failure would this catch?
// - the MCP tool list drifting from the Command Bus catalog (names, schemas, read-only hints);
// - a tool call not reaching the bus on the main thread, or errors/images lost on the way back;
// - the bridge accepting a wrong token or a browser Origin; discovery not finding the instance.
#include "Bridge.h"
#include "ClaudeProcess.h"
#include "CommandBus.h"
#include "MainThreadQueue.h"

#include <windows.h>

#include <chrono>
#include <cstdio>

using namespace aiad;

static int g_failures = 0;
#define CHECK(c)                                                         \
    do                                                                   \
    {                                                                    \
        if (!(c))                                                        \
        {                                                                \
            std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #c);     \
            ++g_failures;                                                \
        }                                                                \
    } while (0)

int main()
{
    MainThreadQueue queue;
    cmd::CommandBus bus;
    double bore = 80.0;
    const DWORD mainThread = GetCurrentThreadId();
    bool ranOnMainThread = true;
    bus.Register({"aiad_test_set_bore", "Set bore", "Sets the bore [mm].",
                  Json{{"type", "object"}, {"properties", {{"bore", {{"type", "number"}, {"minimum", 40}, {"maximum", 120}}}}}, {"required", {"bore"}}},
                  {}, [&](const Json& args) {
                      ranOnMainThread &= GetCurrentThreadId() == mainThread;
                      const double before = bore;
                      bore = args["bore"];
                      cmd::CommandOutput o;
                      o.text = "Bore set.";
                      o.result = {{"bore", bore}};
                      o.images.push_back({"image/png", {0x89, 'P', 'N', 'G'}});
                      o.undo = [&, before] { bore = before; };
                      o.redo = [&, b = bore] { bore = b; };
                      return o;
                  }});

    bridge::BridgeServer server;
    std::string error;
    auto handler = [&](const Json& request) -> Json {
        const std::string method = request.value("method", "");
        if (method == "catalog")
            return {{"ok", true}, {"result", bus.CatalogJson()}};
        const cmd::CommandOutput out = bus.Execute(request.value("name", ""), request.value("args", Json::object()), "ai");
        Json images = Json::array();
        for (const auto& i : out.images)
            images.push_back({{"mimeType", i.mimeType}, {"data", "iVBORw=="}});
        return {{"ok", out.ok}, {"text", out.text}, {"result", out.result}, {"error", out.error}, {"images", images}};
    };
    const std::filesystem::path workspace = std::filesystem::temp_directory_path();
    CHECK(server.Start(queue, handler, workspace, error));

    // Security: wrong token and a browser Origin are refused; discovery finds this instance.
    // (refused before reaching the main thread, so a synchronous call is fine here)
    const Json refused = bridge::Call({server.Url(), "wrong", "", 0}, {{"method", "catalog"}}, 5000);
    CHECK(refused.value("ok", true) == false && refused.value("error", "").find("token") != std::string::npos);
    const auto found = bridge::Discover(workspace);
    CHECK(!found.empty() && found.front().pid == GetCurrentProcessId() && found.front().token == server.Token());

    ai::ClaudeProcess mcp;
    CHECK(mcp.Start({AIAD_MCP_EXE, workspace, {}, {{"AIAD_BRIDGE_URL", server.Url()}, {"AIAD_BRIDGE_TOKEN", server.Token()}}}, error));
    auto rpc = [&](int id, const char* method, const Json& params) -> Json {
        mcp.WriteLine({{"jsonrpc", "2.0"}, {"id", id}, {"method", method}, {"params", params}});
        const auto start = std::chrono::steady_clock::now();
        while (std::chrono::steady_clock::now() - start < std::chrono::seconds(20))
        {
            queue.Drain();  // the app's main loop
            std::vector<Json> events;
            bool exited = false;
            unsigned long code = 0;
            mcp.Poll(events, exited, code);
            for (const Json& e : events)
                if (e.value("id", -1) == id)
                    return e;
            Sleep(2);
        }
        return Json();
    };
    const Json init = rpc(1, "initialize", {{"protocolVersion", "2025-06-18"}, {"capabilities", Json::object()}, {"clientInfo", {{"name", "test"}}}});
    CHECK(init["result"]["serverInfo"]["name"] == "aiad" && init["result"]["protocolVersion"] == "2025-06-18");
    mcp.WriteLine({{"jsonrpc", "2.0"}, {"method", "notifications/initialized"}});

    const Json list = rpc(2, "tools/list", Json::object());
    const Json& tools = list["result"]["tools"];
    CHECK(tools.is_array() && tools.size() == bus.Catalog().size());
    bool sawSet = false, sawHistory = false;
    for (const Json& t : tools)
    {
        sawSet |= t["name"] == "aiad_test_set_bore" && t["inputSchema"]["required"][0] == "bore";
        sawHistory |= t["name"] == "aiad_history" && t["annotations"]["readOnlyHint"] == true;
    }
    CHECK(sawSet && sawHistory);

    const Json ok = rpc(3, "tools/call", {{"name", "aiad_test_set_bore"}, {"arguments", {{"bore", 84}}}});
    CHECK(ok["result"]["isError"] == false && ok["result"]["content"][0]["text"] == "Bore set." && ok["result"]["content"][1]["type"] == "image");
    CHECK(ok["result"]["structuredContent"]["bore"] == 84);
    CHECK(bore == 84.0 && ranOnMainThread);

    const Json bad = rpc(4, "tools/call", {{"name", "aiad_test_set_bore"}, {"arguments", {{"bore", 999}}}});
    CHECK(bad["result"]["isError"] == true && bad["result"]["content"][0]["text"].get<std::string>().find("maximum") != std::string::npos);
    CHECK(bore == 84.0);

    const Json undo = rpc(5, "tools/call", {{"name", "aiad_undo"}, {"arguments", Json::object()}});
    CHECK(undo["result"]["isError"] == false && bore == 80.0);

    const Json unknown = rpc(6, "no/such", Json::object());
    CHECK(unknown["error"]["code"] == -32601);

    mcp.Stop();
    server.Stop();
    CHECK(bridge::Discover(workspace).empty() || bridge::Discover(workspace).front().pid != GetCurrentProcessId());
    std::printf(g_failures ? "mcp_bridge_test: %d failure(s)\n" : "mcp_bridge_test: ok\n", g_failures);
    return g_failures ? 1 : 0;
}
