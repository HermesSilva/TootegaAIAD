// The Claude Code CLI as a child process in headless streaming mode (lesson of the Cockpit: the agent
// loop, tools, MCP, permissions and compaction live in the CLI; the app renders events and answers
// the interactive protocol).
//
//   claude -p --output-format stream-json --input-format stream-json --include-partial-messages
//          --permission-prompt-tool stdio --verbose --permission-mode <mode> [--model] [--effort]
//          [--resume <id>] [--mcp-config <file>] [--allowedTools ...] [--append-system-prompt-file <file>]
//
// CreateProcessW with redirected pipes inside a Job Object (the CLI and its children die with the
// app). One reader thread per stream; parsed events are queued and drained on the main thread.
#pragma once

#include "Json.h"

#include <atomic>
#include <deque>
#include <filesystem>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace aiad::ai
{

struct ProcessOptions
{
    std::filesystem::path executable;      // claude.exe (or .cmd)
    std::filesystem::path cwd;
    std::vector<std::string> arguments;
    std::map<std::string, std::string> environment;  // overrides on top of the app's environment
};

// One-shot run that captures the whole output (stdout + stderr), e.g. `claude plugin list --json`.
struct CapturedRun
{
    bool started = false;
    unsigned long exitCode = 0;
    std::string output;
};
CapturedRun RunCaptured(const std::filesystem::path& executable, const std::vector<std::string>& arguments,
                        const std::filesystem::path& cwd, int timeoutMs = 60000);
// Opens a visible console running the command (sign in, update: the CLI is interactive there).
bool RunInConsole(const std::filesystem::path& executable, const std::vector<std::string>& arguments, const std::filesystem::path& cwd);

// claude.exe from the configured path, PATH, or the native installer folder (~/.local/bin).
std::filesystem::path ResolveClaude(const std::string& configured);
// `claude --version` (empty when it does not run).
std::string ClaudeVersion(const std::filesystem::path& executable);

class ClaudeProcess
{
public:
    ~ClaudeProcess();
    bool Start(const ProcessOptions& options, std::string& error);
    bool Running() const { return m_process != nullptr; }
    void WriteLine(const Json& message);
    // Main thread: takes the events parsed since the last call. `exited` is set once when the
    // process ended (stdout closed); `exitCode` then holds its code.
    void Poll(std::vector<Json>& events, bool& exited, unsigned long& exitCode);
    std::string StderrTail() const;
    void Stop();
    unsigned long Pid() const { return m_pid; }

private:
    void ReadStdout();
    void ReadStderr();

    void* m_process = nullptr;
    void* m_job = nullptr;
    void* m_stdinWrite = nullptr;
    void* m_stdoutRead = nullptr;
    void* m_stderrRead = nullptr;
    unsigned long m_pid = 0;
    std::thread m_stdoutThread, m_stderrThread;
    mutable std::mutex m_mutex;
    std::vector<Json> m_events;
    std::deque<std::string> m_stderr;
    std::atomic<bool> m_eof = false;
    bool m_exitReported = false;
    std::mutex m_writeMutex;
};

}  // namespace aiad::ai
