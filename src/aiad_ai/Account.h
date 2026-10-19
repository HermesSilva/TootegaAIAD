// Clean, isolated reads outside the agent loop (as in the Cockpit): account usage limits (OAuth
// /usage, no tokens spent), the model catalogue (/v1/models), the latest CLI version (npm registry).
// The OAuth token is read-only: never written, never logged. Blocking calls: run off the main thread.
#pragma once

#include "Json.h"

#include <map>
#include <optional>
#include <string>
#include <vector>

namespace aiad::ai
{

struct HttpResult
{
    int status = 0;          // 0 = transport failure
    std::string body, error;
};
HttpResult HttpsGet(const std::string& host, const std::string& path, const std::vector<std::pair<std::string, std::string>>& headers,
                    int timeoutMs = 8000);

// Bearer for direct Anthropic calls, in the CLI's precedence: ANTHROPIC_AUTH_TOKEN,
// CLAUDE_CODE_OAUTH_TOKEN, then claudeAiOauth.accessToken of <claude config>/.credentials.json.
std::string ReadOauthToken();
bool HasCredentialsFile();
// Subscription type / plan from the credentials file ("max", "pro", ...), empty if unknown.
std::string SubscriptionType();

struct LimitWindow
{
    std::string label;       // "Current session", "Weekly (all models)", "Weekly (<model>)"
    double used = 0.0;       // 0..1
    std::string resetsAt;    // ISO 8601
};
struct AccountUsage
{
    bool ok = false;
    std::string error;
    std::vector<LimitWindow> windows;
};
AccountUsage ParseUsage(const Json& j);
AccountUsage FetchAccountUsage();

struct ModelInfo
{
    std::string id, displayName;
    int64_t maxInputTokens = 0;
};
std::vector<ModelInfo> FetchModels(std::string& error);

// Model prices [USD per million tokens] from Anthropic's public pricing page (there is no price
// endpoint). Cached a day in %APPDATA%\Tootega\AIAD\model-pricing.json. Key: "claude-opus-5-5".
struct Price
{
    double input = 0.0, output = 0.0;
};
std::map<std::string, Price> ParsePricingMarkdown(const std::string& markdown);
std::map<std::string, Price> LoadPricing();
// Price of a model id such as "claude-opus-5-5[1m]" or "claude-sonnet-5-5-20261001"; nullopt if unknown.
std::optional<Price> PriceFor(const std::map<std::string, Price>& prices, const std::string& model);

std::string LatestCliVersion();  // npm @anthropic-ai/claude-code, empty on failure
std::string ParseSemver(const std::string& text);
bool IsOutdated(const std::string& installed, const std::string& latest);

}  // namespace aiad::ai
