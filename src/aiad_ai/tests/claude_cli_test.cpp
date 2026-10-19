// Live check against the installed Claude Code CLI, without sending a prompt (no tokens spent):
// spawn it in headless stream-json mode the way AgentSession does, do the control-protocol
// `initialize` handshake and expect its answer. Catches a CLI upgrade that breaks our arguments or
// the handshake, and process plumbing errors (pipes, quoting, job object). Skips without a CLI.
#include "AgentSession.h"
#include "ClaudeProcess.h"

#include <windows.h>

#include <chrono>
#include <cstdio>

using namespace aiad;
using namespace aiad::ai;

int main()
{
    const std::filesystem::path claude = ResolveClaude("");
    if (claude.empty())
    {
        std::printf("claude_cli_test: SKIPPED (Claude Code CLI not found)\n");
        return 0;
    }
    const std::string version = ClaudeVersion(claude);
    std::printf("claude: %s  version: %s\n", claude.string().c_str(), version.c_str());
    if (version.find('.') == std::string::npos)
    {
        std::printf("claude_cli_test: FAIL (--version printed nothing)\n");
        return 1;
    }
    ClaudeProcess process;
    std::string error;
    const std::vector<std::string> args = {"-p", "--output-format", "stream-json", "--input-format", "stream-json", "--include-partial-messages",
                                           "--permission-prompt-tool", "stdio", "--verbose", "--permission-mode", "default"};
    if (!process.Start({claude, std::filesystem::temp_directory_path(), args, {}}, error))
    {
        std::printf("claude_cli_test: FAIL (%s)\n", error.c_str());
        return 1;
    }
    process.WriteLine({{"type", "control_request"}, {"request_id", "init"}, {"request", {{"subtype", "initialize"}}}});
    const auto start = std::chrono::steady_clock::now();
    bool answered = false;
    size_t commands = 0;
    while (!answered && std::chrono::steady_clock::now() - start < std::chrono::seconds(60))
    {
        std::vector<Json> events;
        bool exited = false;
        unsigned long code = 0;
        process.Poll(events, exited, code);
        for (const Json& e : events)
            if (e.value("type", "") == "control_response" && e["response"].value("request_id", "") == "init")
            {
                answered = true;
                const Json r = e["response"].value("response", Json::object());
                if (r.contains("commands") && r["commands"].is_array())
                    commands = r["commands"].size();
            }
        if (exited)
        {
            std::printf("claude_cli_test: FAIL (CLI exited with %lu)\n%s\n", code, process.StderrTail().c_str());
            return 1;
        }
        Sleep(50);
    }
    process.Stop();
    std::printf(answered ? "claude_cli_test: ok (initialize answered, %zu slash commands, %.1f s)\n" : "claude_cli_test: FAIL (no initialize answer in 60 s)\n",
                commands, std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count());
    return answered ? 0 : 1;
}
