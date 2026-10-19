#include "AgentSession.h"

#include "Paths.h"
#include "Text.h"

#include <windows.h>

#include <algorithm>
#include <format>
#include <regex>

namespace aiad::ai
{

namespace
{

int64_t Int(const Json& j, const char* key)
{
    return j.is_object() && j.contains(key) && j[key].is_number() ? j[key].get<int64_t>() : 0;
}

std::string Str(const Json& j, const char* key)
{
    return j.is_object() && j.contains(key) && j[key].is_string() ? j[key].get<std::string>() : std::string();
}

const char* LanguageName(const std::string& code)
{
    if (code == "pt")
        return "Brazilian Portuguese (pt-BR)";
    if (code == "en")
        return "international English";
    if (code == "es")
        return "Spanish";
    return nullptr;
}

// Text of a tool_result content (string or blocks); images are collected apart.
std::string ResultText(const Json& content, std::vector<std::string>& images)
{
    if (content.is_string())
        return content.get<std::string>();
    std::string text;
    if (content.is_array())
        for (const Json& block : content)
        {
            const std::string type = Str(block, "type");
            if (type == "text")
                text += (text.empty() ? "" : "\n") + Str(block, "text");
            else if (type == "image" && block.contains("source"))
                images.push_back(Str(block["source"], "data"));
        }
    return text;
}

}  // namespace

AgentSession::AgentSession(SessionOptions options) : m_options(std::move(options))
{
    m_sessionId = m_options.resumeId;
}

AgentSession::~AgentSession()
{
    Stop();
}

void AgentSession::SetOptions(const SessionOptions& options)
{
    const std::string resume = m_sessionId;
    m_options = options;
    m_options.resumeId = resume;
    m_restartPending = true;
    Touch();
}

std::vector<std::string> AgentSession::Arguments()
{
    std::vector<std::string> args = {"-p",
                                     "--output-format", "stream-json",
                                     "--input-format", "stream-json",
                                     "--include-partial-messages",
                                     "--permission-prompt-tool", "stdio",
                                     "--verbose"};
    if (!m_options.model.empty())
        args.insert(args.end(), {"--model", m_options.model});
    if (!m_options.effort.empty())
        args.insert(args.end(), {"--effort", m_options.effort});
    // Always explicit, `default` included (CLI 2.1.285+ may otherwise start in auto).
    args.insert(args.end(), {"--permission-mode", m_options.permissionMode.empty() ? std::string("default") : m_options.permissionMode});
    if (!m_options.resumeId.empty())
        args.insert(args.end(), {"--resume", m_options.resumeId});
    if (!m_options.mcpConfig.empty())
        args.insert(args.end(), {"--mcp-config", m_options.mcpConfig.string()});
    auto join = [](const std::vector<std::string>& list) {
        std::string s;
        for (const std::string& t : list)
            s += (s.empty() ? "" : ",") + t;
        return s;
    };
    if (!m_options.allowedTools.empty())
        args.insert(args.end(), {"--allowedTools", join(m_options.allowedTools)});
    if (!m_options.disallowedTools.empty())
        args.insert(args.end(), {"--disallowedTools", join(m_options.disallowedTools)});
    // One --append-system-prompt only (repeating it does not add up: the last one wins), by file.
    std::string appended = m_options.appendSystemPrompt;
    if (const char* language = LanguageName(m_options.askLanguage))
        appended += std::format("\n\nWhen you use the AskUserQuestion tool, write every question, header text, and option label/description in {}. "
                                "This language rule applies ONLY to AskUserQuestion content, not to your other replies.",
                                language);
    if (!appended.empty())
    {
        const std::filesystem::path file = AppDataDir() / "tmp" / std::format("prompt-{}-{}.txt", GetCurrentProcessId(), m_tempFiles.size());
        if (WriteTextFile(file, appended))
        {
            m_tempFiles.push_back(file);
            args.insert(args.end(), {"--append-system-prompt-file", file.string()});
        }
    }
    return args;
}

void AgentSession::Write(const Json& message)
{
    if (m_options.offline)
        m_offlineWrites.push_back(message);
    else
        m_process.WriteLine(message);
}

bool AgentSession::EnsureProcess()
{
    if (m_options.offline)
        return true;
    if (m_restartPending && m_process.Running())
    {
        m_process.Stop();
        m_costBase = m_stats.costUsd;
    }
    m_restartPending = false;
    if (m_process.Running())
        return true;
    m_options.resumeId = m_sessionId;
    const std::filesystem::path claude = ResolveClaude(m_options.claudePath);
    if (claude.empty())
    {
        Notice("Claude Code CLI not found. Install it (claude.ai/code) or set its path in AI settings.", true);
        return false;
    }
    std::map<std::string, std::string> environment = m_options.environment;
    if (m_options.permissionMode == "auto")
        environment["CLAUDE_CODE_ENABLE_AUTO_MODE"] = "1";
    std::string error;
    if (!m_process.Start({claude, m_options.cwd, Arguments(), environment}, error))
    {
        Notice(error, true);
        return false;
    }
    // Control-protocol handshake: enables interactive routing and returns the slash commands.
    Write({{"type", "control_request"}, {"request_id", "init"}, {"request", {{"subtype", "initialize"}}}});
    return true;
}

void AgentSession::Send(const std::string& text, const std::vector<std::pair<std::string, std::string>>& images, size_t undoDepth)
{
    TimelineItem item;
    item.kind = ItemKind::User;
    item.text = text;
    item.undoDepth = undoDepth;
    item.images = int(images.size());
    m_timeline.push_back(item);
    Touch();
    if (!EnsureProcess())
        return;
    Json content = Json::array();
    if (!text.empty())
        content.push_back({{"type", "text"}, {"text", text}});
    for (const auto& [mime, data] : images)
        content.push_back({{"type", "image"}, {"source", {{"type", "base64"}, {"media_type", mime}, {"data", data}}}});
    if (content.empty())
        content.push_back({{"type", "text"}, {"text", ""}});
    Write({{"type", "user"}, {"message", {{"role", "user"}, {"content", content}}}});
    // A prompt sent while the agent works is queued by the CLI: it joins the running turn (one result)
    // or starts the next one (its stream marks the session busy again).
    if (!m_busy)
        m_turnStart = std::chrono::steady_clock::now();
    m_busy = true;
    m_interruptDeadline.reset();
}

void AgentSession::Interrupt()
{
    if (!m_process.Running())
    {
        m_busy = false;   // nothing runs: nothing to wait for
        return;
    }
    Write({{"type", "control_request"}, {"request_id", std::format("interrupt_{}", m_revision)}, {"request", {{"subtype", "interrupt"}}}});
    // A turn that already ended answers no interrupt with a result: the session is idle after a moment.
    m_interruptDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
}

void AgentSession::Stop()
{
    m_process.Stop();
    m_costBase = m_stats.costUsd;
    m_busy = false;
    m_interruptDeadline.reset();
    m_pending.clear();
    for (const auto& f : m_tempFiles)
    {
        std::error_code ec;
        std::filesystem::remove(f, ec);
    }
    m_tempFiles.clear();
    Touch();
}

double AgentSession::TurnSeconds() const
{
    return m_busy ? std::chrono::duration<double>(std::chrono::steady_clock::now() - m_turnStart).count() : 0.0;
}

void AgentSession::Pump()
{
    std::vector<Json> events;
    bool exited = false;
    unsigned long code = 0;
    m_process.Poll(events, exited, code);
    for (const Json& e : events)
        Handle(e);
    if (m_interruptDeadline && std::chrono::steady_clock::now() > *m_interruptDeadline)
    {
        m_interruptDeadline.reset();
        if (m_busy)
        {
            m_busy = false;
            m_compacting = false;
            for (TimelineItem& item : m_timeline)
                item.streaming = false;
            Notice("Stopped.");
        }
    }
    if (exited)
    {
        if (m_busy || code != 0)
        {
            std::string tail = m_process.StderrTail();
            if (tail.size() > 1500)
                tail = "..." + tail.substr(tail.size() - 1500);
            Notice(std::format("Claude Code exited (code {}).{}{}", code, tail.empty() ? "" : "\n", tail), true);
        }
        m_busy = false;
        m_interruptDeadline.reset();
        m_pending.clear();
        m_process.Stop();
        m_costBase = m_stats.costUsd;
        Touch();
    }
}

void AgentSession::Notice(std::string text, bool error)
{
    TimelineItem item;
    item.kind = ItemKind::Notice;
    item.text = std::move(text);
    item.error = error;
    m_timeline.push_back(std::move(item));
    Touch();
}

TimelineItem& AgentSession::StreamingItem(ItemKind kind, const std::string& messageId)
{
    if (!m_timeline.empty())
    {
        TimelineItem& last = m_timeline.back();
        if (last.kind == kind && last.messageId == messageId && last.streaming)
            return last;
    }
    TimelineItem item;
    item.kind = kind;
    item.messageId = messageId;
    item.streaming = true;
    m_timeline.push_back(std::move(item));
    return m_timeline.back();
}

void AgentSession::Handle(const Json& event)
{
    if (!event.is_object())
        return;
    const std::string type = Str(event, "type");
    // Subagent narration (forwarded with parent_tool_use_id) stays out of the main bubble; its
    // permission prompts still reach the user.
    if (!Str(event, "parent_tool_use_id").empty() && type != "control_request")
        return;
    Touch();
    if (type == "system")
        HandleSystem(event);
    else if (type == "stream_event")
        HandleStream(event.value("event", Json::object()));
    else if (type == "assistant")
        HandleAssistant(event);
    else if (type == "user")
        HandleUser(event);
    else if (type == "control_request")
        HandleControlRequest(event);
    else if (type == "control_response")
    {
        const Json outer = event.value("response", Json::object());
        if (auto waiter = m_controlWaiters.find(Str(outer, "request_id")); waiter != m_controlWaiters.end())
        {
            auto done = std::move(waiter->second);
            m_controlWaiters.erase(waiter);
            done(Str(outer, "subtype") == "success" ? outer.value("response", Json::object()) : Json());
            return;
        }
        // initialize handshake: slash commands before the first turn.
        const Json response = event.value("response", Json::object()).value("response", Json::object());
        if (response.contains("commands") && response["commands"].is_array() && m_slashCommands.empty())
            for (const Json& c : response["commands"])
                if (c.is_string())
                    m_slashCommands.push_back(c.get<std::string>());
                else if (c.is_object() && c.contains("name"))
                    m_slashCommands.push_back(Str(c, "name"));
    }
    else if (type == "result")
        HandleResult(event);
    else if (type == "rate_limit_event")
    {
        const Json info = event.value("rate_limit_info", Json::object());
        const std::string bucket = Str(info, "rateLimitType");
        RateWindow* window = bucket == "five_hour" ? &m_stats.fiveHour : bucket == "seven_day" ? &m_stats.sevenDay : nullptr;
        if (window)
        {
            window->status = Str(info, "status");
            if (info.contains("utilization") && info["utilization"].is_number())
            {
                double u = info["utilization"].get<double>();
                window->utilization = u > 1.5 ? u / 100.0 : u;
            }
            if (info.contains("resetsAt") && info["resetsAt"].is_number())
            {
                int64_t r = info["resetsAt"].get<int64_t>();
                window->resetsAt = r > 1000000000000LL ? r / 1000 : r;
            }
        }
    }
    // Unknown types are ignored: the stream contract is version-tolerant.
}

void AgentSession::HandleSystem(const Json& s)
{
    const std::string subtype = Str(s, "subtype");
    if (subtype == "init")
    {
        m_sessionId = Str(s, "session_id");
        m_options.resumeId = m_sessionId;
        m_model = Str(s, "model");
        m_tools.clear();
        if (s.contains("tools") && s["tools"].is_array())
            for (const Json& t : s["tools"])
                if (t.is_string())
                    m_tools.push_back(t);
        if (s.contains("mcp_servers") && s["mcp_servers"].is_array())
            m_mcpServers = s["mcp_servers"];
        if (s.contains("slash_commands") && s["slash_commands"].is_array())
        {
            m_slashCommands.clear();
            for (const Json& c : s["slash_commands"])
                if (c.is_string())
                    m_slashCommands.push_back(c);
        }
        if (s.contains("skills") && s["skills"].is_array())
        {
            m_skills.clear();
            for (const Json& c : s["skills"])
                if (c.is_string())
                    m_skills.push_back(c);
        }
        const int64_t window = Int(s, "context_window");
        m_stats.contextLimit = window > 0 ? window : m_model.find("[1m]") != std::string::npos ? 1000000 : 200000;
        return;
    }
    if (subtype == "status")
    {
        const std::string status = Str(s, "status");
        if (status == "compacting")
            m_compacting = true;
        else if (s.contains("compact_result") || s.contains("compact_error"))
            m_compacting = false;
        return;
    }
    if (subtype == "compact_boundary")
    {
        m_compacting = false;
        ++m_stats.compactions;
        const Json meta =s.contains("compact_metadata") ? s["compact_metadata"] : s.value("compactMetadata", Json::object());
        TimelineItem item;
        item.kind = ItemKind::Compaction;
        item.preTokens = Int(meta, "pre_tokens");
        item.postTokens = Int(meta, "post_tokens");
        item.text = Str(meta, "trigger");
        m_timeline.push_back(item);
        return;
    }
    static const std::set<std::string> known = {"init", "background_tasks_changed", "task_started", "task_updated", "task_notification",
                                                "compact_boundary", "status", "hook_response", "hook_started"};
    if (subtype.empty() || known.contains(subtype))
        return;
    // Mid-session warnings recognised by shape (credits, restricted models, fallbacks), once each.
    const std::string warning = Str(s, "warning");
    static const std::regex warnish("warn|notice|credit|limit|restrict|degrad|fallback", std::regex::icase);
    if (warning.empty() && !std::regex_search(subtype, warnish))
        return;
    std::string text = !warning.empty() ? warning : Str(s, "message");
    if (text.empty())
        text = Str(s, "text");
    if (text.empty())
        text = subtype;
    if (m_noticesSeen.insert(subtype + text).second)
        Notice("Engine: " + text);
}

void AgentSession::ApplyPromptUsage(const Json& usage, const std::string& messageId)
{
    // Live figures: each API message of the turn counts as soon as it reports its usage (a long turn
    // with tools takes minutes before the CLI's result). Fields only grow within a message.
    std::array<int64_t, 4>& m = m_turnUsage[messageId];
    const int64_t fields[4] = {Int(usage, "input_tokens"), Int(usage, "output_tokens"), Int(usage, "cache_read_input_tokens"),
                               Int(usage, "cache_creation_input_tokens")};
    for (int i = 0; i < 4; ++i)
        m[size_t(i)] = std::max(m[size_t(i)], fields[i]);
    RecomputeTokens();
    const int64_t prompt = fields[0] + fields[2] + fields[3];
    if (prompt <= 0)
        return;
    m_stats.contextUsed = prompt;
    m_stats.peakContext = std::max(m_stats.peakContext, prompt);
}

void AgentSession::RecomputeTokens()
{
    std::array<int64_t, 4> total = m_committedTokens;
    for (const auto& [id, m] : m_turnUsage)
        for (size_t i = 0; i < 4; ++i)
            total[i] += m[i];
    m_stats.inputTokens = total[0];
    m_stats.outputTokens = total[1];
    m_stats.cacheReadTokens = total[2];
    m_stats.cacheCreateTokens = total[3];
}

void AgentSession::HandleStream(const Json& raw)
{
    // A prompt queued during the last turn starts its own turn: the session works again.
    if (!m_busy && Str(raw, "type") == "message_start")
    {
        m_busy = true;
        m_turnStart = std::chrono::steady_clock::now();
    }
    const std::string type = Str(raw, "type");
    if (type == "message_start")
    {
        const Json message = raw.value("message", Json::object());
        m_currentMessage = Str(message, "id");
        m_toolBuffers.clear();
        if (message.contains("usage"))
            ApplyPromptUsage(message["usage"], m_currentMessage);
    }
    else if (type == "content_block_start")
    {
        const int index = int(Int(raw, "index"));
        const Json block = raw.value("content_block", Json::object());
        if (Str(block, "type") == "tool_use")
        {
            const std::string id = Str(block, "id");
            m_toolBuffers[index] = {id, ""};
            if (!m_toolItems.contains(id))
            {
                TimelineItem item;
                item.kind = ItemKind::Tool;
                item.messageId = m_currentMessage;
                item.tool.id = id;
                item.tool.name = Str(block, "name");
                m_toolItems[id] = m_timeline.size();
                m_toolStart[id] = std::chrono::steady_clock::now();
                m_timeline.push_back(std::move(item));
            }
        }
    }
    else if (type == "content_block_delta")
    {
        const Json delta = raw.value("delta", Json::object());
        const std::string kind = Str(delta, "type");
        if (kind == "text_delta")
        {
            m_streamedText.insert(m_currentMessage);
            StreamingItem(ItemKind::Assistant, m_currentMessage).text += Str(delta, "text");
        }
        else if (kind == "thinking_delta")
        {
            m_streamedText.insert(m_currentMessage + "/thinking");
            StreamingItem(ItemKind::Thinking, m_currentMessage).text += Str(delta, "thinking");
        }
        else if (kind == "input_json_delta")
        {
            auto it = m_toolBuffers.find(int(Int(raw, "index")));
            if (it != m_toolBuffers.end())
                it->second.second += Str(delta, "partial_json");
        }
    }
    else if (type == "content_block_stop")
    {
        auto it = m_toolBuffers.find(int(Int(raw, "index")));
        if (it != m_toolBuffers.end())
        {
            const Json input = ParseJson(it->second.second.empty() ? "{}" : it->second.second);
            if (auto item = m_toolItems.find(it->second.first); item != m_toolItems.end() && item->second < m_timeline.size())
                m_timeline[item->second].tool.input = input.is_discarded() ? Json::object() : input;
            m_toolBuffers.erase(it);
        }
        for (TimelineItem& item : m_timeline)
            if (item.streaming && item.messageId == m_currentMessage && item.kind != ItemKind::Tool)
                item.streaming = false;
    }
    else if (type == "message_delta")
    {
        if (raw.contains("usage"))
            ApplyPromptUsage(raw["usage"], m_currentMessage);
    }
    else if (type == "message_stop")
    {
        for (TimelineItem& item : m_timeline)
            if (item.messageId == m_currentMessage)
                item.streaming = false;
    }
}

void AgentSession::HandleAssistant(const Json& event)
{
    const Json message = event.value("message", Json::object());
    const std::string id = Str(message, "id");
    if (message.contains("usage"))
        ApplyPromptUsage(message["usage"], id);
    if (!message.contains("content") || !message["content"].is_array())
        return;
    for (const Json& block : message["content"])
    {
        const std::string type = Str(block, "type");
        if (type == "text" && !m_streamedText.contains(id))
        {
            TimelineItem item;
            item.kind = ItemKind::Assistant;
            item.messageId = id;
            item.text = Str(block, "text");
            if (!item.text.empty())
                m_timeline.push_back(std::move(item));
        }
        else if (type == "thinking" && !m_streamedText.contains(id + "/thinking"))
        {
            TimelineItem item;
            item.kind = ItemKind::Thinking;
            item.messageId = id;
            item.text = Str(block, "thinking");
            if (!item.text.empty())
                m_timeline.push_back(std::move(item));
        }
        else if (type == "tool_use")
        {
            const std::string toolId = Str(block, "id");
            auto it = m_toolItems.find(toolId);
            if (it == m_toolItems.end())
            {
                TimelineItem item;
                item.kind = ItemKind::Tool;
                item.messageId = id;
                item.tool.id = toolId;
                item.tool.name = Str(block, "name");
                item.tool.input = block.value("input", Json::object());
                m_toolItems[toolId] = m_timeline.size();
                m_toolStart[toolId] = std::chrono::steady_clock::now();
                m_timeline.push_back(std::move(item));
            }
            else if (it->second < m_timeline.size() && (m_timeline[it->second].tool.input.is_null() || m_timeline[it->second].tool.input.empty()))
                m_timeline[it->second].tool.input = block.value("input", Json::object());
        }
    }
}

void AgentSession::HandleUser(const Json& event)
{
    const Json content = event.value("message", Json::object()).value("content", Json());
    if (!content.is_array())
        return;
    for (const Json& block : content)
    {
        if (Str(block, "type") != "tool_result")
            continue;
        const std::string id = Str(block, "tool_use_id");
        auto it = m_toolItems.find(id);
        if (it == m_toolItems.end() || it->second >= m_timeline.size())
            continue;
        ToolCall& tool = m_timeline[it->second].tool;
        tool.imagesBase64.clear();
        tool.resultText = ResultText(block.value("content", Json()), tool.imagesBase64);
        tool.isError = block.value("is_error", false);
        tool.done = true;
        if (auto start = m_toolStart.find(id); start != m_toolStart.end())
            tool.seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - start->second).count();
        ToolUsage& usage = m_stats.tools[tool.name];
        ++usage.calls;
        usage.resultChars += int64_t(tool.resultText.size());
        if (OnToolResult)
            OnToolResult(tool.name);
    }
}

void AgentSession::HandleControlRequest(const Json& event)
{
    const Json request = event.value("request", Json::object());
    if (Str(request, "subtype") != "can_use_tool")
        return;
    PendingRequest pending;
    pending.requestId = Str(event, "request_id");
    pending.tool = Str(request, "tool_name");
    pending.displayName = Str(request, "display_name");
    pending.description = Str(request, "description");
    pending.input = request.value("input", Json::object());
    pending.suggestions = request.value("permission_suggestions", Json::array());
    if (pending.tool == "AskUserQuestion")
    {
        pending.kind = RequestKind::Question;
        for (const Json& q : pending.input.value("questions", Json::array()))
        {
            Question question;
            question.question = Str(q, "question");
            question.header = Str(q, "header");
            question.multiSelect = q.value("multiSelect", false);
            for (const Json& o : q.value("options", Json::array()))
                question.options.push_back({Str(o, "label"), Str(o, "description")});
            pending.questions.push_back(std::move(question));
        }
    }
    else if (pending.tool == "ExitPlanMode")
    {
        pending.kind = RequestKind::Plan;
        pending.plan = Str(pending.input, "plan");
    }
    // Bypass means "stop asking", but some CLI checks still reach the host; answer them here.
    // AskUserQuestion is a question to the user, never auto-answered.
    if (m_options.permissionMode == "bypassPermissions" && pending.kind != RequestKind::Question)
    {
        m_pending.push_back(pending);
        Allow(pending.requestId, false);
        return;
    }
    m_pending.push_back(std::move(pending));
}

void AgentSession::RequestControl(const std::string& subtype, std::function<void(const Json&)> done)
{
    if (!EnsureProcess())
        return done(Json());
    const std::string id = std::format("{}_{}", subtype, ++m_controlSequence);
    m_controlWaiters[id] = std::move(done);
    Write({{"type", "control_request"}, {"request_id", id}, {"request", {{"subtype", subtype}}}});
}

void AgentSession::Respond(const std::string& requestId, const Json& response)
{
    Write({{"type", "control_response"}, {"response", {{"subtype", "success"}, {"request_id", requestId}, {"response", response}}}});
    m_pending.erase(std::remove_if(m_pending.begin(), m_pending.end(), [&](const PendingRequest& p) { return p.requestId == requestId; }),
                    m_pending.end());
    Touch();
}

void AgentSession::Allow(const std::string& requestId, bool always)
{
    for (const PendingRequest& p : m_pending)
        if (p.requestId == requestId)
        {
            // `allow` requires updatedInput (validated by the CLI).
            Json response = {{"behavior", "allow"}, {"updatedInput", p.input.is_null() ? Json::object() : p.input}};
            if (always && p.suggestions.is_array() && !p.suggestions.empty())
                response["updatedPermissions"] = p.suggestions;
            return Respond(requestId, response);
        }
}

void AgentSession::Deny(const std::string& requestId, const std::string& message)
{
    Respond(requestId, {{"behavior", "deny"}, {"message", message.empty() ? std::string("Denied by user") : message}});
}

void AgentSession::Answer(const std::string& requestId, const std::map<std::string, std::string>& answers)
{
    for (const PendingRequest& p : m_pending)
        if (p.requestId == requestId)
        {
            Json input = p.input.is_object() ? p.input : Json::object();
            input["answers"] = answers;
            return Respond(requestId, {{"behavior", "allow"}, {"updatedInput", input}});
        }
}

void AgentSession::ApprovePlan(const std::string& requestId, const std::string& editedPlan)
{
    for (const PendingRequest& p : m_pending)
        if (p.requestId == requestId)
        {
            Json input = p.input.is_object() ? p.input : Json::object();
            input["plan"] = editedPlan;
            return Respond(requestId, {{"behavior", "allow"}, {"updatedInput", input}});
        }
}

void AgentSession::HandleResult(const Json& r)
{
    // Only turns we started count (a resumed CLI may replay old results).
    if (!m_busy)
        return;
    const double seconds = TurnSeconds();
    // The turn is over (prompts queued meanwhile joined it, or start the next turn, which streams again).
    m_busy = false;
    m_interruptDeadline.reset();
    m_compacting = false;
    m_pending.clear();
    ++m_stats.turns;
    m_stats.activeSeconds += seconds;
    double turnCost = 0.0;
    if (r.contains("total_cost_usd") && r["total_cost_usd"].is_number())
    {
        // Cumulative for the CLI process; earlier processes of this session are in m_costBase.
        const double total = m_costBase + r["total_cost_usd"].get<double>();
        turnCost = std::max(0.0, total - m_stats.costUsd);
        m_stats.costUsd = total;
        m_stats.lastTurnCostUsd = turnCost;
    }
    if (r.contains("usage"))
    {
        const Json& u = r["usage"];
        // The turn's official totals replace the live per-message figures (no double count).
        m_committedTokens[0] += Int(u, "input_tokens");
        m_committedTokens[1] += Int(u, "output_tokens");
        m_committedTokens[2] += Int(u, "cache_read_input_tokens");
        m_committedTokens[3] += Int(u, "cache_creation_input_tokens");
        m_turnUsage.clear();
        RecomputeTokens();
        const int64_t previousRead = m_stats.lastTurnCacheRead, write = Int(u, "cache_creation_input_tokens");
        m_stats.lastTurnCacheRead = Int(u, "cache_read_input_tokens");
        m_stats.lastTurnPrompt = Int(u, "input_tokens") + Int(u, "cache_read_input_tokens") + write;
        // Cache reset: a context that was being read now had to be written again (TTL expired,
        // model switch). Priced at the cache-write premium over a read (~1.25x - 0.1x of input).
        if (previousRead > 20000 && write > previousRead / 2)
        {
            ++m_stats.cacheResets;
            m_stats.cacheResetCostUsd += turnCost * double(write) / double(std::max<int64_t>(1, m_stats.lastTurnPrompt));
        }
    }
    m_stats.lastTurnEnd = std::chrono::system_clock::now();
    if (r.value("is_error", false))
    {
        std::string text = Str(r, "result");
        if (text.empty())
            text = Str(r, "subtype");
        Notice("Turn ended with an error: " + text, true);
    }
    for (const Json& d : r.value("permission_denials", Json::array()))
        Notice("The engine denied " + Str(d, "tool_name") + " (see the tool card for the reason).");
    TimelineItem end;
    end.kind = ItemKind::TurnEnd;
    end.costUsd = turnCost;
    end.seconds = seconds;
    m_timeline.push_back(end);
    for (TimelineItem& item : m_timeline)
        item.streaming = false;
    m_currentMessage.clear();
    m_streamedText.clear();
    m_toolBuffers.clear();
    if (OnTurnEnd)
        OnTurnEnd();
}

void AgentSession::TruncateTimeline(size_t item)
{
    if (item < m_timeline.size())
        m_timeline.resize(item);
    m_toolItems.clear();
    for (size_t i = 0; i < m_timeline.size(); ++i)
        if (m_timeline[i].kind == ItemKind::Tool)
            m_toolItems[m_timeline[i].tool.id] = i;
    Touch();
}

void AgentSession::LoadTimeline(std::vector<TimelineItem> items)
{
    m_timeline = std::move(items);
    TruncateTimeline(m_timeline.size());
}

}  // namespace aiad::ai
