// One conversation with the agent: owns the CLI process, turns its tolerant event stream into a
// timeline, the pending interactions (permission, question, plan) and the consumption figures.
// Knows nothing about geometry: the model is reached only through MCP -> bridge -> Command Bus.
// Main thread only (Pump() once per frame).
#pragma once

#include "ClaudeProcess.h"
#include "Json.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <deque>
#include <filesystem>
#include <functional>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <vector>

namespace aiad::ai
{

struct SessionOptions
{
    std::string claudePath;                 // empty = PATH / ~/.local/bin
    std::filesystem::path cwd;              // the workspace
    std::string model;                      // empty = CLI default
    std::string effort;                     // low | medium | high | xhigh | max | empty
    std::string permissionMode = "default"; // default | plan | acceptEdits | auto | dontAsk | bypassPermissions
    std::string resumeId;
    std::filesystem::path mcpConfig;        // --mcp-config
    std::vector<std::string> allowedTools;  // auto-approved (read-only Command Bus tools)
    std::vector<std::string> disallowedTools;
    std::string appendSystemPrompt;         // domain prompt + quiet directive + user text
    std::string askLanguage;                // language of AskUserQuestion content ("pt", "en", ...)
    std::map<std::string, std::string> environment;
    bool offline = false;                   // fixture replay (tests): no CLI process, events come through Feed()
};

enum class ItemKind
{
    User,
    Assistant,
    Thinking,
    Tool,
    Notice,       // engine warning, process exit, error
    Compaction,
    TurnEnd
};

struct ToolCall
{
    std::string id, name;
    Json input;
    std::string resultText;
    std::vector<std::string> imagesBase64;  // png from tool results (viewport snapshots)
    bool done = false, isError = false;
    double seconds = 0.0;
};

struct TimelineItem
{
    ItemKind kind = ItemKind::Notice;
    std::string text;
    std::string messageId;
    ToolCall tool;
    bool streaming = false;
    bool error = false;
    size_t undoDepth = 0;                   // User: Command Bus depth when the prompt was sent
    int images = 0;                         // User: attached images
    double costUsd = 0.0, seconds = 0.0;    // TurnEnd
    int64_t preTokens = 0, postTokens = 0;  // Compaction
    std::chrono::system_clock::time_point time = std::chrono::system_clock::now();
};

enum class RequestKind
{
    Permission,
    Question,
    Plan
};

struct QuestionOption
{
    std::string label, description;
};
struct Question
{
    std::string question, header;
    std::vector<QuestionOption> options;
    bool multiSelect = false;
};

struct PendingRequest
{
    RequestKind kind = RequestKind::Permission;
    std::string requestId, tool, displayName, description;
    Json input, suggestions;
    std::vector<Question> questions;        // Question
    std::string plan;                       // Plan (editable)
};

struct ToolUsage
{
    int calls = 0;
    int64_t resultChars = 0;                // context pushed in by its results (~4 chars/token)
};

struct RateWindow
{
    std::string status;                     // allowed | allowed_warning | rejected
    double utilization = -1.0;              // 0..1, -1 = unknown
    int64_t resetsAt = 0;                   // epoch seconds
};

struct SessionStats
{
    int64_t inputTokens = 0, outputTokens = 0, cacheReadTokens = 0, cacheCreateTokens = 0;
    int64_t contextUsed = 0, contextLimit = 200000, peakContext = 0;
    int64_t lastTurnCacheRead = 0, lastTurnPrompt = 0;
    double costUsd = 0.0, lastTurnCostUsd = 0.0;
    int turns = 0;
    int compactions = 0;
    int cacheResets = 0;                    // turns whose prompt was re-written instead of read from cache
    double cacheResetCostUsd = 0.0;         // what those re-writes cost (estimate)
    double activeSeconds = 0.0;             // summed turn durations
    std::chrono::system_clock::time_point lastTurnEnd{};  // cache life (1 h TTL) counts from here
    std::chrono::steady_clock::time_point sessionStart = std::chrono::steady_clock::now();
    std::map<std::string, ToolUsage> tools;
    RateWindow fiveHour, sevenDay;

    double CacheHitRate() const
    {
        const double prompt = double(inputTokens + cacheReadTokens + cacheCreateTokens);
        return prompt > 0 ? double(cacheReadTokens) / prompt : 0.0;
    }
    double LastTurnHitRate() const { return lastTurnPrompt > 0 ? double(lastTurnCacheRead) / double(lastTurnPrompt) : 0.0; }
};

class AgentSession
{
public:
    explicit AgentSession(SessionOptions options);
    ~AgentSession();

    const SessionOptions& Options() const { return m_options; }
    // Changing model/effort/mode restarts the CLI on the next send, resuming the same session.
    void SetOptions(const SessionOptions& options);

    // `undoDepth`: Command Bus depth at this prompt (for rewinding the model with the conversation).
    void Send(const std::string& text, const std::vector<std::pair<std::string, std::string>>& images /*mime, base64*/, size_t undoDepth);
    void Interrupt();
    void Stop();
    void Pump();
    // One stream-json event, as the CLI would emit it (Pump uses it; tests replay recorded fixtures).
    void Feed(const Json& event) { Handle(event); }
    // Control messages written to the CLI while offline (tests check the protocol answers).
    const std::vector<Json>& OfflineWrites() const { return m_offlineWrites; }

    // Our own control request (e.g. "get_context_usage": computed locally by the CLI, no turn, no
    // tokens). The callback gets the response payload, or null when the CLI answered with an error.
    void RequestControl(const std::string& subtype, std::function<void(const Json&)> done);

    // Interactive protocol (answers the oldest pending request).
    void Allow(const std::string& requestId, bool always);
    void Deny(const std::string& requestId, const std::string& message);
    void Answer(const std::string& requestId, const std::map<std::string, std::string>& answers);
    void ApprovePlan(const std::string& requestId, const std::string& editedPlan);

    // Rewind: drops the timeline from the prompt at `item` on (the transcript is truncated by the
    // caller through the store); the next send resumes the truncated session.
    void TruncateTimeline(size_t item);
    // Replaces the timeline with a transcript loaded from disk (resume).
    void LoadTimeline(std::vector<TimelineItem> items);

    const std::vector<TimelineItem>& Timeline() const { return m_timeline; }
    const std::deque<PendingRequest>& Pending() const { return m_pending; }
    const SessionStats& Stats() const { return m_stats; }
    bool Busy() const { return m_busy; }
    int QueuedPrompts() const { return std::max(0, m_pendingTurns - 1); }
    bool Compacting() const { return m_compacting; }
    double TurnSeconds() const;
    const std::string& SessionId() const { return m_sessionId; }
    const std::string& ActiveModel() const { return m_model; }
    const std::vector<std::string>& SlashCommands() const { return m_slashCommands; }
    const std::vector<std::string>& Skills() const { return m_skills; }
    const Json& McpServers() const { return m_mcpServers; }
    const std::vector<std::string>& Tools() const { return m_tools; }
    bool ProcessRunning() const { return m_process.Running(); }
    std::string StderrTail() const { return m_process.StderrTail(); }
    uint64_t Revision() const { return m_revision; }

    // Called when a turn ends (auto-save of the timeline, notifications).
    std::function<void()> OnTurnEnd;
    // Called for every tool_use of the agent (UI follow-ups such as refreshing after a mutation).
    std::function<void(const std::string& tool)> OnToolResult;

private:
    bool EnsureProcess();
    std::vector<std::string> Arguments();
    void Handle(const Json& event);
    void HandleStream(const Json& raw);
    void HandleAssistant(const Json& event);
    void HandleUser(const Json& event);
    void HandleControlRequest(const Json& event);
    void HandleSystem(const Json& event);
    void HandleResult(const Json& event);
    void ApplyPromptUsage(const Json& usage, const std::string& messageId);
    void RecomputeTokens();
    void Respond(const std::string& requestId, const Json& response);
    TimelineItem& StreamingItem(ItemKind kind, const std::string& messageId);
    void Notice(std::string text, bool error = false);
    void Touch() { ++m_revision; }

    SessionOptions m_options;
    bool m_restartPending = false;
    ClaudeProcess m_process;
    std::vector<std::filesystem::path> m_tempFiles;
    std::vector<TimelineItem> m_timeline;
    std::deque<PendingRequest> m_pending;
    SessionStats m_stats;
    double m_costBase = 0.0;                // cost of earlier CLI processes of this session
    bool m_busy = false, m_compacting = false;
    int m_pendingTurns = 0;                 // prompts sent and not answered yet (queued while busy)
    std::chrono::steady_clock::time_point m_turnStart;
    std::string m_sessionId, m_model;
    std::vector<std::string> m_slashCommands, m_skills, m_tools;
    Json m_mcpServers = Json::array();
    std::set<std::string> m_noticesSeen;
    // streaming state
    std::string m_currentMessage;
    std::set<std::string> m_streamedText;
    std::map<int, std::pair<std::string, std::string>> m_toolBuffers;  // block index -> (tool id, partial json)
    std::map<std::string, size_t> m_toolItems;                         // tool id -> timeline index
    std::map<std::string, std::chrono::steady_clock::time_point> m_toolStart;
    uint64_t m_revision = 0;
    std::vector<Json> m_offlineWrites;
    std::map<std::string, std::function<void(const Json&)>> m_controlWaiters;
    int m_controlSequence = 0;
    std::array<int64_t, 4> m_committedTokens{};                       // input, output, cache read, cache write of finished turns
    std::map<std::string, std::array<int64_t, 4>> m_turnUsage;        // this turn, per API message (live)
    void Write(const Json& message);
};

}  // namespace aiad::ai
