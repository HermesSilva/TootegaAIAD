// JSON for the whole project (nlohmann/json 3.12.0, vendored in deps/_src/nlohmann_json).
#pragma once

#include <nlohmann/json.hpp>

namespace aiad
{
using Json = nlohmann::json;

// Parses without throwing; a discarded value means "not JSON".
inline Json ParseJson(std::string_view text)
{
    return Json::parse(text, nullptr, false);
}
}  // namespace aiad
