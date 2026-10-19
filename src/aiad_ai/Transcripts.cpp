#include "Transcripts.h"

#include "Paths.h"
#include "Text.h"

#include <algorithm>
#include <fstream>
#include <map>
#include <mutex>

namespace aiad::ai
{

namespace
{

std::string Str(const Json& j, const char* key)
{
    return j.is_object() && j.contains(key) && j[key].is_string() ? j[key].get<std::string>() : std::string();
}

std::string TextOf(const Json& content)
{
    if (content.is_string())
        return content.get<std::string>();
    std::string text;
    if (content.is_array())
        for (const Json& b : content)
            if (Str(b, "type") == "text")
                text += Str(b, "text");
    return text;
}

std::string Collapse(const std::string& s)
{
    std::string out;
    bool space = false;
    for (const char c : s)
    {
        if (c == ' ' || c == '\n' || c == '\r' || c == '\t')
            space = !out.empty();
        else
        {
            if (space)
                out += ' ';
            space = false;
            out += c;
        }
    }
    return out;
}

template <typename F>
void ForEachLine(const std::filesystem::path& file, F&& f)
{
    std::ifstream in(file, std::ios::binary);
    std::string line;
    while (std::getline(in, line))
    {
        if (line.empty())
            continue;
        const Json o = ParseJson(line);
        if (o.is_object())
            f(o);
    }
}

// Is this transcript line a real user prompt (the unit of rewind)?
bool IsPrompt(const Json& o, std::string* text = nullptr, int* images = nullptr)
{
    if (Str(o, "type") != "user" || o.value("isMeta", false) || o.value("isSidechain", false) || !o.contains("message"))
        return false;
    const Json content = o["message"].value("content", Json());
    const std::string t = TextOf(content);
    int count = 0;
    if (content.is_array())
        for (const Json& b : content)
            count += Str(b, "type") == "image";
    const bool prompt = (!t.empty() && !IsMetaUserText(t)) || count > 0;
    if (prompt && text)
        *text = IsMetaUserText(t) ? std::string() : t;
    if (prompt && images)
        *images = count;
    return prompt;
}

std::filesystem::path TitlesFile()
{
    return AppDataDir() / "titles.json";
}

}  // namespace

bool IsMetaUserText(const std::string& text)
{
    const std::string t = Collapse(text);
    return t.empty() || t.starts_with("<command-") || t.starts_with("<local-command") || t.starts_with("<system-reminder") ||
           t.starts_with("<task-notification") || t.starts_with("[Request interrupted");
}

std::string ShortTitle(const std::string& text, size_t max)
{
    std::string t = Collapse(text);
    for (size_t i = 0; i + 1 < t.size(); ++i)
        if ((t[i] == '.' || t[i] == '!' || t[i] == '?') && t[i + 1] == ' ' && i + 1 <= max)
        {
            t.resize(i + 1);
            break;
        }
    if (t.size() <= max)
        return t;
    size_t cut = max;
    while (cut > 0 && (uint8_t(t[cut]) & 0xC0) == 0x80)  // never split a UTF-8 sequence
        --cut;
    const size_t space = t.rfind(' ', cut);
    return t.substr(0, space != std::string::npos && space > max / 2 ? space : cut) + "\xE2\x80\xA6";
}

std::string EncodeCwd(const std::wstring& cwd)
{
    std::string out;
    for (const wchar_t c : cwd)
        out += (c < 128 && std::isalnum(int(c))) ? char(c) : '-';
    return out;
}

std::filesystem::path ProjectDir(const std::filesystem::path& cwd)
{
    const std::filesystem::path base = ClaudeConfigDir() / "projects";
    const std::string encoded = EncodeCwd(cwd.wstring());
    const std::filesystem::path direct = base / encoded;
    std::error_code ec;
    if (std::filesystem::exists(direct, ec))
        return direct;
    // The drive letter's case is ambiguous; the CLI keeps whatever it saw first.
    for (const auto& entry : std::filesystem::directory_iterator(base, ec))
        if (_stricmp(entry.path().filename().string().c_str(), encoded.c_str()) == 0)
            return entry.path();
    return direct;
}

std::vector<TranscriptInfo> ListTranscripts(const std::filesystem::path& cwd)
{
    struct Cached
    {
        uint64_t size;
        std::filesystem::file_time_type time;
        TranscriptInfo info;
    };
    static std::mutex mutex;
    static std::map<std::filesystem::path, Cached> cache;

    std::vector<TranscriptInfo> list;
    std::error_code ec;
    for (const auto& entry : std::filesystem::directory_iterator(ProjectDir(cwd), ec))
    {
        if (entry.path().extension() != ".jsonl")
            continue;
        const uint64_t size = entry.file_size(ec);
        const auto time = entry.last_write_time(ec);
        {
            std::lock_guard lock(mutex);
            if (auto it = cache.find(entry.path()); it != cache.end() && it->second.size == size && it->second.time == time)
            {
                TranscriptInfo info = it->second.info;
                if (const std::string custom = CustomTitle(info.id); !custom.empty())
                    info.title = custom;
                list.push_back(info);
                continue;
            }
        }
        TranscriptInfo info;
        info.id = entry.path().stem().string();
        info.sizeBytes = size;
        info.updated = std::chrono::clock_cast<std::chrono::system_clock>(time);
        std::string aiTitle, firstUser;
        ForEachLine(entry.path(), [&](const Json& o) {
            const std::string type = Str(o, "type");
            if (type == "ai-title" && !Str(o, "aiTitle").empty())
                aiTitle = Collapse(Str(o, "aiTitle"));
            else if (type == "user" || type == "assistant")
            {
                if (!o.value("isMeta", false))
                    ++info.messages;
                std::string text;
                if (IsPrompt(o, &text))
                {
                    ++info.userPrompts;
                    if (firstUser.empty())
                        firstUser = text;
                }
                if (type == "assistant")
                {
                    ++info.assistantMessages;
                    const Json message = o.value("message", Json::object());
                    if (!Str(message, "model").empty())
                        info.model = Str(message, "model");
                    for (const Json& b : message.value("content", Json::array()))
                        info.toolCalls += Str(b, "type") == "tool_use";
                }
            }
        });
        info.title = !aiTitle.empty() ? aiTitle : ShortTitle(firstUser);
        {
            std::lock_guard lock(mutex);
            cache[entry.path()] = {size, time, info};
        }
        if (const std::string custom = CustomTitle(info.id); !custom.empty())
            info.title = custom;
        list.push_back(info);
    }
    std::sort(list.begin(), list.end(), [](const TranscriptInfo& a, const TranscriptInfo& b) { return a.updated > b.updated; });
    return list;
}

std::vector<TimelineItem> LoadTranscript(const std::filesystem::path& cwd, const std::string& id)
{
    std::vector<TimelineItem> items;
    std::map<std::string, size_t> tools, assistants;
    ForEachLine(ProjectDir(cwd) / (id + ".jsonl"), [&](const Json& o) {
        if (o.value("isMeta", false) || o.value("isSidechain", false))
            return;
        const std::string type = Str(o, "type");
        const Json message = o.value("message", Json::object());
        if (type == "user")
        {
            const Json content = message.value("content", Json());
            if (content.is_array())
                for (const Json& b : content)
                    if (Str(b, "type") == "tool_result")
                        if (auto it = tools.find(Str(b, "tool_use_id")); it != tools.end())
                        {
                            ToolCall& tool = items[it->second].tool;
                            const Json c = b.value("content", Json());
                            if (c.is_string())
                                tool.resultText = c.get<std::string>();
                            else if (c.is_array())
                                for (const Json& part : c)
                                    if (Str(part, "type") == "text")
                                        tool.resultText += Str(part, "text");
                                    else if (Str(part, "type") == "image" && part.contains("source"))
                                        tool.imagesBase64.push_back(Str(part["source"], "data"));
                            tool.isError = b.value("is_error", false);
                            tool.done = true;
                        }
            std::string text;
            int images = 0;
            if (IsPrompt(o, &text, &images))
            {
                TimelineItem item;
                item.kind = ItemKind::User;
                item.text = text;
                item.images = images;
                item.undoDepth = size_t(-1);  // sent in an earlier run: the model changes are not on this undo stack
                items.push_back(std::move(item));
            }
        }
        else if (type == "assistant")
        {
            const std::string messageId = Str(message, "id");
            for (const Json& b : message.value("content", Json::array()))
            {
                const std::string blockType = Str(b, "type");
                if (blockType == "text" || blockType == "thinking")
                {
                    const ItemKind kind = blockType == "text" ? ItemKind::Assistant : ItemKind::Thinking;
                    const std::string key = messageId + blockType;
                    auto it = assistants.find(key);
                    if (it == assistants.end() || it->second != items.size() - 1)
                    {
                        TimelineItem item;
                        item.kind = kind;
                        item.messageId = messageId;
                        assistants[key] = items.size();
                        items.push_back(std::move(item));
                        it = assistants.find(key);
                    }
                    items[it->second].text += Str(b, blockType == "text" ? "text" : "thinking");
                }
                else if (blockType == "tool_use" && !tools.contains(Str(b, "id")))
                {
                    TimelineItem item;
                    item.kind = ItemKind::Tool;
                    item.messageId = messageId;
                    item.tool.id = Str(b, "id");
                    item.tool.name = Str(b, "name");
                    item.tool.input = b.value("input", Json::object());
                    tools[item.tool.id] = items.size();
                    items.push_back(std::move(item));
                }
            }
        }
        else if (type == "system" && Str(o, "subtype") == "compact_boundary")
        {
            TimelineItem item;
            item.kind = ItemKind::Compaction;
            const Json meta = o.value("compactMetadata", o.value("compact_metadata", Json::object()));
            item.preTokens = meta.value("preTokens", meta.value("pre_tokens", int64_t(0)));
            items.push_back(std::move(item));
        }
    });
    // Empty thinking/text items (signatures only) carry nothing to show.
    items.erase(std::remove_if(items.begin(), items.end(),
                               [](const TimelineItem& i) { return (i.kind == ItemKind::Assistant || i.kind == ItemKind::Thinking) && i.text.empty(); }),
                items.end());
    return items;
}

bool DeleteTranscript(const std::filesystem::path& cwd, const std::string& id)
{
    std::error_code ec;
    return std::filesystem::remove(ProjectDir(cwd) / (id + ".jsonl"), ec);
}

int DeleteAllTranscripts(const std::filesystem::path& cwd)
{
    int removed = 0;
    std::error_code ec;
    std::vector<std::filesystem::path> files;
    for (const auto& entry : std::filesystem::directory_iterator(ProjectDir(cwd), ec))
        if (entry.path().extension() == ".jsonl")
            files.push_back(entry.path());
    for (const auto& f : files)
        removed += std::filesystem::remove(f, ec) ? 1 : 0;
    return removed;
}

bool TruncateTranscript(const std::filesystem::path& cwd, const std::string& id, int promptIndex)
{
    const std::filesystem::path file = ProjectDir(cwd) / (id + ".jsonl");
    std::ifstream in(file, std::ios::binary);
    if (!in)
        return false;
    std::string kept, line;
    int prompts = 0;
    bool cut = false;
    while (std::getline(in, line))
    {
        if (!line.empty())
        {
            const Json o = ParseJson(line);
            if (o.is_object() && IsPrompt(o) && prompts++ == promptIndex)
            {
                cut = true;
                break;
            }
        }
        kept += line + "\n";
    }
    in.close();
    if (!cut)
        return false;
    const std::filesystem::path tmp = file.string() + ".tmp";
    if (!WriteTextFile(tmp, kept))
        return false;
    std::error_code ec;
    std::filesystem::rename(tmp, file, ec);
    return !ec;
}

std::string CustomTitle(const std::string& id)
{
    static std::mutex mutex;
    std::lock_guard lock(mutex);
    const Json titles = ParseJson(ReadTextFile(TitlesFile()));
    return titles.is_object() ? titles.value(id, std::string()) : std::string();
}

void SetCustomTitle(const std::string& id, const std::string& title)
{
    Json titles = ParseJson(ReadTextFile(TitlesFile()));
    if (!titles.is_object())
        titles = Json::object();
    if (title.empty())
        titles.erase(id);
    else
        titles[id] = title;
    WriteTextFile(TitlesFile(), titles.dump(2));
}

}  // namespace aiad::ai
