#include "Account.h"

#include "Paths.h"
#include "Text.h"

#include <windows.h>
#include <winhttp.h>

#include <algorithm>
#include <cstdio>
#include <format>
#include <regex>

namespace aiad::ai
{

HttpResult HttpsGet(const std::string& host, const std::string& path, const std::vector<std::pair<std::string, std::string>>& headers, int timeoutMs)
{
    HttpResult result;
    HINTERNET session = WinHttpOpen(L"TootegaAIAD/0.1", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!session)
    {
        result.error = "WinHttpOpen: " + LastErrorText();
        return result;
    }
    WinHttpSetTimeouts(session, timeoutMs, timeoutMs, timeoutMs, timeoutMs);
    HINTERNET connection = WinHttpConnect(session, Widen(host).c_str(), INTERNET_DEFAULT_HTTPS_PORT, 0);
    HINTERNET request = connection ? WinHttpOpenRequest(connection, L"GET", Widen(path).c_str(), nullptr, WINHTTP_NO_REFERER,
                                                        WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE)
                                   : nullptr;
    if (request)
    {
        std::wstring headerText;
        for (const auto& [k, v] : headers)
            headerText += Widen(k + ": " + v + "\r\n");
        if (WinHttpSendRequest(request, headerText.empty() ? WINHTTP_NO_ADDITIONAL_HEADERS : headerText.c_str(), DWORD(-1), WINHTTP_NO_REQUEST_DATA, 0, 0, 0) &&
            WinHttpReceiveResponse(request, nullptr))
        {
            DWORD status = 0, size = sizeof(status);
            WinHttpQueryHeaders(request, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX, &status, &size,
                                WINHTTP_NO_HEADER_INDEX);
            result.status = int(status);
            for (DWORD available = 0; WinHttpQueryDataAvailable(request, &available) && available > 0;)
            {
                std::string chunk(available, '\0');
                DWORD read = 0;
                if (!WinHttpReadData(request, chunk.data(), available, &read))
                    break;
                result.body.append(chunk.data(), read);
            }
        }
        else
            result.error = LastErrorText();
    }
    else
        result.error = LastErrorText();
    if (request)
        WinHttpCloseHandle(request);
    if (connection)
        WinHttpCloseHandle(connection);
    WinHttpCloseHandle(session);
    return result;
}

static std::string EnvVar(const char* name)
{
    char buffer[8192];
    const DWORD n = GetEnvironmentVariableA(name, buffer, sizeof(buffer));
    std::string v = n > 0 && n < sizeof(buffer) ? std::string(buffer, n) : std::string();
    while (!v.empty() && v.back() == ' ')
        v.pop_back();
    return v;
}

static Json OauthObject()
{
    const Json j = ParseJson(ReadTextFile(ClaudeConfigDir() / ".credentials.json"));
    return j.is_object() && j.contains("claudeAiOauth") && j["claudeAiOauth"].is_object() ? j["claudeAiOauth"] : Json::object();
}

std::string ReadOauthToken()
{
    if (std::string t = EnvVar("ANTHROPIC_AUTH_TOKEN"); !t.empty())
        return t;
    if (std::string t = EnvVar("CLAUDE_CODE_OAUTH_TOKEN"); !t.empty())
        return t;
    const Json oauth = OauthObject();
    return oauth.contains("accessToken") && oauth["accessToken"].is_string() ? oauth["accessToken"].get<std::string>() : std::string();
}

bool HasCredentialsFile()
{
    std::error_code ec;
    return std::filesystem::exists(ClaudeConfigDir() / ".credentials.json", ec);
}

std::string SubscriptionType()
{
    const Json oauth = OauthObject();
    return oauth.contains("subscriptionType") && oauth["subscriptionType"].is_string() ? oauth["subscriptionType"].get<std::string>() : std::string();
}

AccountUsage ParseUsage(const Json& j)
{
    AccountUsage usage;
    auto window = [](const Json& w, std::string label) -> std::optional<LimitWindow> {
        if (!w.is_object())
            return std::nullopt;
        const Json pct = w.contains("utilization") ? w["utilization"] : w.value("percent", Json());
        if (!pct.is_number())
            return std::nullopt;
        return LimitWindow{std::move(label), std::clamp(pct.get<double>() / 100.0, 0.0, 1.0),
                           w.contains("resets_at") && w["resets_at"].is_string() ? w["resets_at"].get<std::string>() : std::string()};
    };
    if (j.contains("limits") && j["limits"].is_array())
        for (const Json& l : j["limits"])
        {
            const std::string kind = l.value("kind", "");
            std::string label = kind == "session" ? "Current session" : kind == "weekly_all" ? "Weekly (all models)" : "";
            if (kind == "weekly_scoped")
            {
                std::string model;
                if (l.contains("scope") && l["scope"].contains("model"))
                    model = l["scope"]["model"].value("display_name", "");
                label = "Weekly (" + (model.empty() ? std::string("model") : model) + ")";
            }
            if (!label.empty())
                if (auto w = window(l, label))
                    usage.windows.push_back(*w);
        }
    if (usage.windows.empty())
    {
        for (const auto& [key, label] : {std::pair{"five_hour", "Current session"}, std::pair{"seven_day", "Weekly (all models)"},
                                         std::pair{"seven_day_opus", "Weekly (Opus)"}, std::pair{"seven_day_sonnet", "Weekly (Sonnet)"}})
            if (j.contains(key))
                if (auto w = window(j[key], label))
                    usage.windows.push_back(*w);
    }
    usage.ok = !usage.windows.empty();
    return usage;
}

AccountUsage FetchAccountUsage()
{
    const std::string token = ReadOauthToken();
    if (token.empty())
        return {false, "No Claude login found (run `claude` and /login, or set CLAUDE_CODE_OAUTH_TOKEN)."};
    HttpResult r;
    for (int attempt = 0; attempt < 2; ++attempt)
    {
        r = HttpsGet("api.anthropic.com", "/api/oauth/usage",
                     {{"Authorization", "Bearer " + token}, {"anthropic-beta", "oauth-2025-04-20"}, {"anthropic-version", "2023-06-01"}});
        if (r.status == 200 || (r.status != 0 && r.status != 429 && r.status < 500))
            break;
        Sleep(700);
    }
    if (r.status != 200)
        return {false, r.status ? std::format("HTTP {}", r.status) : "network: " + r.error};
    const Json j = ParseJson(r.body);
    if (j.is_discarded())
        return {false, "bad JSON from /usage"};
    AccountUsage usage = ParseUsage(j);
    if (!usage.ok)
        usage.error = "no limit windows in the answer";
    return usage;
}

std::vector<ModelInfo> FetchModels(std::string& error)
{
    std::vector<std::pair<std::string, std::string>> headers = {{"anthropic-version", "2023-06-01"}};
    if (const std::string key = EnvVar("ANTHROPIC_API_KEY"); !key.empty())
        headers.push_back({"x-api-key", key});
    else if (const std::string token = ReadOauthToken(); !token.empty())
    {
        headers.push_back({"Authorization", "Bearer " + token});
        headers.push_back({"anthropic-beta", "oauth-2025-04-20"});
    }
    else
    {
        error = "no credentials";
        return {};
    }
    const HttpResult r = HttpsGet("api.anthropic.com", "/v1/models?limit=100", headers);
    const Json j = ParseJson(r.body);
    std::vector<ModelInfo> models;
    if (r.status != 200 || !j.is_object() || !j.contains("data"))
    {
        error = r.status ? std::format("HTTP {}", r.status) : r.error;
        return models;
    }
    for (const Json& m : j["data"])
        models.push_back({m.value("id", ""), m.value("display_name", m.value("id", "")), m.value("max_input_tokens", int64_t(0))});
    return models;
}

std::map<std::string, Price> ParsePricingMarkdown(const std::string& markdown)
{
    std::map<std::string, Price> prices;
    size_t start = markdown.find("## Model pricing");
    if (start == std::string::npos)
        start = 0;
    const size_t end = markdown.find("\n## ", start + 4);
    const std::string block = markdown.substr(start, end == std::string::npos ? std::string::npos : end - start);
    static const std::regex name(R"(^\s*Claude\s+([A-Za-z]+)\s+(\d+(?:\.\d+)?))", std::regex::icase);
    static const std::regex usd(R"(\$\s*([\d.]+))");
    size_t pos = 0;
    while (pos < block.size())
    {
        size_t nl = block.find('\n', pos);
        if (nl == std::string::npos)
            nl = block.size();
        const std::string line = block.substr(pos, nl - pos);
        pos = nl + 1;
        if (line.find('|') == std::string::npos)
            continue;
        std::vector<std::string> cells;
        size_t a = line.find('|') + 1;
        for (size_t b; (b = line.find('|', a)) != std::string::npos; a = b + 1)
            cells.push_back(line.substr(a, b - a));
        if (cells.size() < 3)
            continue;
        // "[Claude Haiku 5.5](/docs/...)" keeps its text; footnote links after the name go away harmlessly.
        std::string model = std::regex_replace(cells[0], std::regex(R"(\[([^\]]*)\]\([^)]*\))"), "$1");
        std::smatch m, in, out;
        if (!std::regex_search(model, m, name) || !std::regex_search(cells[1], in, usd) || !std::regex_search(cells.back(), out, usd))
            continue;
        std::string family = m[1].str(), version = m[2].str();
        std::transform(family.begin(), family.end(), family.begin(), [](unsigned char c) { return char(std::tolower(c)); });
        std::replace(version.begin(), version.end(), '.', '-');
        const std::string id = "claude-" + family + "-" + version;
        if (!prices.contains(id))
            prices[id] = {std::stod(in[1].str()), std::stod(out[1].str())};
    }
    return prices;
}

std::map<std::string, Price> LoadPricing()
{
    const std::filesystem::path cache = AppDataDir() / "model-pricing.json";
    std::error_code ec;
    const bool fresh = std::filesystem::exists(cache, ec) &&
                       std::filesystem::file_time_type::clock::now() - std::filesystem::last_write_time(cache, ec) < std::chrono::hours(24);
    auto fromJson = [](const Json& j) {
        std::map<std::string, Price> prices;
        if (j.is_object())
            for (const auto& [k, v] : j.items())
                prices[k] = {v.value("input", 0.0), v.value("output", 0.0)};
        return prices;
    };
    if (fresh)
        return fromJson(ParseJson(ReadTextFile(cache)));
    const HttpResult r = HttpsGet("platform.claude.com", "/docs/en/about-claude/pricing.md", {{"Accept", "text/markdown, text/plain, */*"}});
    std::map<std::string, Price> prices = r.status == 200 ? ParsePricingMarkdown(r.body) : std::map<std::string, Price>();
    if (prices.empty())
        return fromJson(ParseJson(ReadTextFile(cache)));  // stale beats nothing
    Json j = Json::object();
    for (const auto& [k, p] : prices)
        j[k] = {{"input", p.input}, {"output", p.output}};
    WriteTextFile(cache, j.dump(2));
    return prices;
}

std::optional<Price> PriceFor(const std::map<std::string, Price>& prices, const std::string& model)
{
    std::string id = model.substr(0, model.find('['));
    // Longest known prefix ("claude-opus-5-5-20261001" -> "claude-opus-5-5").
    std::optional<Price> best;
    size_t bestLength = 0;
    for (const auto& [k, p] : prices)
        if (id.starts_with(k) && k.size() > bestLength)
        {
            best = p;
            bestLength = k.size();
        }
    return best;
}

std::string LatestCliVersion()
{
    const HttpResult r = HttpsGet("registry.npmjs.org", "/@anthropic-ai/claude-code/latest", {}, 5000);
    const Json j = ParseJson(r.body);
    return r.status == 200 && j.is_object() ? j.value("version", "") : std::string();
}

std::string ParseSemver(const std::string& text)
{
    std::smatch m;
    static const std::regex semver(R"((\d+)\.(\d+)\.(\d+))");
    return std::regex_search(text, m, semver) ? m[0].str() : std::string();
}

bool IsOutdated(const std::string& installed, const std::string& latest)
{
    const std::string a = ParseSemver(installed), b = ParseSemver(latest);
    if (a.empty() || b.empty())
        return false;
    int pa[3], pb[3];
    std::sscanf(a.c_str(), "%d.%d.%d", &pa[0], &pa[1], &pa[2]);
    std::sscanf(b.c_str(), "%d.%d.%d", &pb[0], &pb[1], &pb[2]);
    for (int i = 0; i < 3; ++i)
        if (pa[i] != pb[i])
            return pa[i] < pb[i];
    return false;
}

}  // namespace aiad::ai
