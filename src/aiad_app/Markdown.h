// Markdown for the chat (what the agent writes): headings, paragraphs with **bold**, *italic*,
// `code` and [links](url), bullet and numbered lists, block quotes, fenced code blocks with a copy
// button, tables, rules. Word-wrapped with mixed fonts.
#pragma once

#include <imgui.h>

#include <string>
#include <string_view>

namespace aiad::app
{

struct UiFonts
{
    ImFont* regular = nullptr;
    ImFont* bold = nullptr;
    ImFont* mono = nullptr;
    float size = 15.0f;  // base size (unscaled)
};

void RenderMarkdown(std::string_view text, const UiFonts& fonts, float wrapWidth);

// Plain text in the mono font, wrapped (tool input/output).
void RenderMono(std::string_view text, const UiFonts& fonts, float wrapWidth);

}  // namespace aiad::app
