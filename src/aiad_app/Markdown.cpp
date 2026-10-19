#include "Markdown.h"

#include <imgui_internal.h>

#include <algorithm>
#include <vector>

namespace aiad::app
{

namespace
{

enum Style : int
{
    Plain = 0,
    Bold = 1,
    Italic = 2,
    Code = 4,
    Link = 8
};

struct Span
{
    std::string text;
    int style = Plain;
};

const ImU32 kCodeColour = IM_COL32(236, 160, 90, 255);
const ImU32 kLinkColour = IM_COL32(100, 170, 255, 255);

// Inline spans of one paragraph.
std::vector<Span> ParseInline(std::string_view s)
{
    std::vector<Span> spans;
    std::string current;
    int style = Plain;
    auto flush = [&] {
        if (!current.empty())
            spans.push_back({current, style});
        current.clear();
    };
    for (size_t i = 0; i < s.size(); ++i)
    {
        const char c = s[i];
        if (c == '`')
        {
            const size_t end = s.find('`', i + 1);
            if (end != std::string_view::npos)
            {
                flush();
                spans.push_back({std::string(s.substr(i + 1, end - i - 1)), style | Code});
                i = end;
                continue;
            }
        }
        if (c == '*' && i + 1 < s.size() && s[i + 1] == '*')
        {
            flush();
            style ^= Bold;
            ++i;
            continue;
        }
        if ((c == '*' || c == '_') && i + 1 < s.size() && s[i + 1] != ' ' && (i == 0 || s[i - 1] == ' ' || (style & Italic)))
        {
            const size_t end = s.find(c, i + 1);
            if (end != std::string_view::npos && !(style & Italic))
            {
                flush();
                style |= Italic;
                continue;
            }
            if (style & Italic)
            {
                flush();
                style &= ~Italic;
                continue;
            }
        }
        if (c == '*' && (style & Italic))
        {
            flush();
            style &= ~Italic;
            continue;
        }
        if (c == '[')
        {
            const size_t close = s.find("](", i);
            const size_t end = close == std::string_view::npos ? close : s.find(')', close);
            if (end != std::string_view::npos)
            {
                flush();
                spans.push_back({std::string(s.substr(i + 1, close - i - 1)), style | Link});
                i = end;
                continue;
            }
        }
        current += c;
    }
    flush();
    return spans;
}

ImFont* FontFor(int style, const UiFonts& f)
{
    if (style & Code)
        return f.mono;
    if (style & Bold)
        return f.bold;
    return f.regular;
}

// Word-wrapped spans starting at the current cursor; `indent` is where wrapped lines restart.
void Flow(const std::vector<Span>& spans, const UiFonts& fonts, float wrapWidth, float indent, float scale = 1.0f)
{
    ImGuiWindow* window = ImGui::GetCurrentWindow();
    ImDrawList* draw = window->DrawList;
    const float startX = window->DC.CursorPos.x;
    const float left = startX + indent;
    const float right = startX + wrapWidth;
    const float size = ImGui::GetFontSize() * scale;
    const float lineHeight = size + ImGui::GetStyle().ItemSpacing.y * 0.5f;
    ImVec2 pos = window->DC.CursorPos;
    const ImU32 textColour = ImGui::GetColorU32(ImGuiCol_Text);
    for (const Span& span : spans)
    {
        ImFont* font = FontFor(span.style, fonts);
        ImFontBaked* baked = font->GetFontBaked(size);
        const ImU32 colour = span.style & Code ? kCodeColour : span.style & Link ? kLinkColour : textColour;
        size_t i = 0;
        const std::string& t = span.text;
        while (i < t.size())
        {
            // Next word (with its trailing spaces).
            size_t j = i;
            while (j < t.size() && t[j] != ' ')
                ++j;
            while (j < t.size() && t[j] == ' ')
                ++j;
            const char* begin = t.data() + i;
            const char* end = t.data() + j;
            ImVec2 extent = font->CalcTextSizeA(size, FLT_MAX, 0.0f, begin, end);
            if (pos.x + extent.x > right && pos.x > left + 1.0f)
            {
                pos.x = left;
                pos.y += lineHeight;
                while (begin < end && *begin == ' ')
                    ++begin;
                extent = font->CalcTextSizeA(size, FLT_MAX, 0.0f, begin, end);
            }
            if (span.style & Code)
                draw->AddRectFilled(ImVec2(pos.x - 1, pos.y), ImVec2(pos.x + extent.x + 1, pos.y + size + 1), IM_COL32(255, 255, 255, 18), 3.0f);
            draw->AddText(font, size, pos, colour, begin, end);
            if (span.style & Bold && font == fonts.regular)
                draw->AddText(font, size, ImVec2(pos.x + 0.6f, pos.y), colour, begin, end);
            if (span.style & Link)
                draw->AddLine(ImVec2(pos.x, pos.y + size), ImVec2(pos.x + extent.x, pos.y + size), colour);
            pos.x += extent.x;
            i = j;
            (void)baked;
        }
    }
    ImGui::SetCursorScreenPos(ImVec2(startX, pos.y));
    ImGui::Dummy(ImVec2(wrapWidth, lineHeight));
}

bool IsTableRow(std::string_view line)
{
    const size_t first = line.find_first_not_of(' ');
    return first != std::string_view::npos && line[first] == '|';
}

std::vector<std::string> Cells(std::string_view line)
{
    std::vector<std::string> cells;
    size_t a = line.find('|') + 1;
    for (size_t b; (b = line.find('|', a)) != std::string_view::npos; a = b + 1)
    {
        std::string cell(line.substr(a, b - a));
        cell.erase(0, cell.find_first_not_of(' '));
        cell.erase(cell.find_last_not_of(' ') + 1);
        cells.push_back(cell);
    }
    return cells;
}

}  // namespace

void RenderMono(std::string_view text, const UiFonts& fonts, float wrapWidth)
{
    ImGui::PushFont(fonts.mono, 0.0f);
    ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + wrapWidth);
    ImGui::TextUnformatted(text.data(), text.data() + text.size());
    ImGui::PopTextWrapPos();
    ImGui::PopFont();
}

void RenderMarkdown(std::string_view text, const UiFonts& fonts, float wrapWidth)
{
    std::vector<std::string_view> lines;
    for (size_t a = 0; a <= text.size();)
    {
        size_t b = text.find('\n', a);
        if (b == std::string_view::npos)
            b = text.size();
        std::string_view line = text.substr(a, b - a);
        if (!line.empty() && line.back() == '\r')
            line.remove_suffix(1);
        lines.push_back(line);
        a = b + 1;
    }
    int codeBlock = 0;
    for (size_t i = 0; i < lines.size(); ++i)
    {
        std::string_view line = lines[i];
        const size_t first = line.find_first_not_of(' ');
        if (first == std::string_view::npos)
        {
            ImGui::Dummy(ImVec2(1, ImGui::GetFontSize() * 0.35f));
            continue;
        }
        const std::string_view trimmed = line.substr(first);

        // Fenced code block.
        if (trimmed.starts_with("```"))
        {
            std::string code;
            size_t j = i + 1;
            for (; j < lines.size() && !lines[j].substr(std::min(lines[j].find_first_not_of(' '), lines[j].size())).starts_with("```"); ++j)
                code += std::string(lines[j]) + "\n";
            if (!code.empty())
                code.pop_back();
            i = j;
            ImGui::PushID(codeBlock++);
            ImDrawList* draw = ImGui::GetWindowDrawList();
            draw->ChannelsSplit(2);
            draw->ChannelsSetCurrent(1);
            const ImVec2 start = ImGui::GetCursorScreenPos();
            ImGui::Indent(8.0f);
            ImGui::Dummy(ImVec2(1, 4));
            RenderMono(code, fonts, wrapWidth - 16.0f);
            ImGui::Dummy(ImVec2(1, 4));
            ImGui::Unindent(8.0f);
            const ImVec2 end(start.x + wrapWidth, ImGui::GetCursorScreenPos().y);
            draw->ChannelsSetCurrent(0);
            draw->AddRectFilled(start, end, IM_COL32(12, 14, 18, 255), 4.0f);
            draw->AddRect(start, end, IM_COL32(255, 255, 255, 25), 4.0f);
            draw->ChannelsMerge();
            const ImVec2 back = ImGui::GetCursorScreenPos();
            ImGui::SetCursorScreenPos(ImVec2(end.x - 46, start.y + 3));
            if (ImGui::SmallButton("Copy"))
                ImGui::SetClipboardText(code.c_str());
            ImGui::SetCursorScreenPos(back);
            ImGui::Dummy(ImVec2(1, 2));
            ImGui::PopID();
            continue;
        }

        // Table: header row, separator row, body rows.
        if (IsTableRow(trimmed) && i + 1 < lines.size() && lines[i + 1].find("---") != std::string_view::npos && IsTableRow(lines[i + 1]))
        {
            const std::vector<std::string> header = Cells(trimmed);
            std::vector<std::vector<std::string>> rows;
            size_t j = i + 2;
            for (; j < lines.size() && IsTableRow(lines[j]); ++j)
                rows.push_back(Cells(lines[j]));
            i = j - 1;
            if (!header.empty() && ImGui::BeginTable("##md_table", int(header.size()),
                                                    ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchProp,
                                                    ImVec2(wrapWidth, 0)))
            {
                // Column weights from the longest cell of each column (an empty header must not starve it).
                for (size_t c = 0; c < header.size(); ++c)
                {
                    size_t longest = std::max<size_t>(header[c].size(), 3);
                    for (const auto& row : rows)
                        if (c < row.size())
                            longest = std::max(longest, row[c].size());
                    ImGui::TableSetupColumn(header[c].c_str(), ImGuiTableColumnFlags_WidthStretch, float(std::min<size_t>(longest, 60)));
                }
                ImGui::PushFont(fonts.bold, 0.0f);
                ImGui::TableHeadersRow();
                ImGui::PopFont();
                for (const auto& row : rows)
                {
                    ImGui::TableNextRow();
                    for (size_t c = 0; c < header.size(); ++c)
                    {
                        ImGui::TableNextColumn();
                        if (c < row.size())
                            Flow(ParseInline(row[c]), fonts, ImGui::GetContentRegionAvail().x, 0.0f);
                    }
                }
                ImGui::EndTable();
            }
            continue;
        }

        // Rule.
        if (trimmed == "---" || trimmed == "***" || trimmed == "___")
        {
            ImGui::Separator();
            continue;
        }

        // Heading.
        if (trimmed.starts_with('#'))
        {
            size_t level = trimmed.find_first_not_of('#');
            if (level != std::string_view::npos && trimmed[level] == ' ')
            {
                const float scale = level == 1 ? 1.35f : level == 2 ? 1.2f : 1.08f;
                ImGui::Dummy(ImVec2(1, 3));
                ImGui::PushFont(fonts.bold, fonts.size * scale);
                std::vector<Span> spans = ParseInline(trimmed.substr(level + 1));
                for (Span& s : spans)
                    s.style |= Bold;
                Flow(spans, fonts, wrapWidth, 0.0f);
                ImGui::PopFont();
                continue;
            }
        }

        // Lists and quotes (one level of indentation per two leading spaces).
        const float depth = float(first / 2) * 16.0f;
        if (trimmed.starts_with("- ") || trimmed.starts_with("* ") || trimmed.starts_with("+ "))
        {
            ImGui::Indent(depth + 4.0f);
            const ImVec2 p = ImGui::GetCursorScreenPos();
            ImGui::GetWindowDrawList()->AddCircleFilled(ImVec2(p.x + 4, p.y + ImGui::GetFontSize() * 0.55f), 2.4f, ImGui::GetColorU32(ImGuiCol_Text));
            ImGui::SetCursorScreenPos(ImVec2(p.x + 14, p.y));
            Flow(ParseInline(trimmed.substr(2)), fonts, wrapWidth - depth - 18.0f, 0.0f);
            ImGui::Unindent(depth + 4.0f);
            continue;
        }
        size_t digits = 0;
        while (digits < trimmed.size() && std::isdigit(uint8_t(trimmed[digits])))
            ++digits;
        if (digits > 0 && digits + 1 < trimmed.size() && (trimmed[digits] == '.' || trimmed[digits] == ')') && trimmed[digits + 1] == ' ')
        {
            ImGui::Indent(depth + 4.0f);
            const ImVec2 p = ImGui::GetCursorScreenPos();
            const std::string number(trimmed.substr(0, digits + 1));
            ImGui::GetWindowDrawList()->AddText(p, ImGui::GetColorU32(ImGuiCol_TextDisabled), number.c_str());
            ImGui::SetCursorScreenPos(ImVec2(p.x + 22, p.y));
            Flow(ParseInline(trimmed.substr(digits + 2)), fonts, wrapWidth - depth - 26.0f, 0.0f);
            ImGui::Unindent(depth + 4.0f);
            continue;
        }
        if (trimmed.starts_with('>'))
        {
            const ImVec2 p = ImGui::GetCursorScreenPos();
            ImGui::Indent(12.0f);
            ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
            Flow(ParseInline(trimmed.substr(trimmed.size() > 1 && trimmed[1] == ' ' ? 2 : 1)), fonts, wrapWidth - 12.0f, 0.0f);
            ImGui::PopStyleColor();
            ImGui::Unindent(12.0f);
            ImGui::GetWindowDrawList()->AddLine(ImVec2(p.x + 3, p.y), ImVec2(p.x + 3, ImGui::GetCursorScreenPos().y - 2), IM_COL32(120, 140, 170, 200), 3.0f);
            continue;
        }

        // Paragraph: joins the following plain lines.
        std::string paragraph(trimmed);
        while (i + 1 < lines.size())
        {
            const std::string_view next = lines[i + 1];
            const size_t f = next.find_first_not_of(' ');
            if (f == std::string_view::npos)
                break;
            const std::string_view n = next.substr(f);
            if (n.starts_with('#') || n.starts_with("```") || n.starts_with("- ") || n.starts_with("* ") || n.starts_with('>') || IsTableRow(n) ||
                (std::isdigit(uint8_t(n[0])) && n.find(". ") != std::string_view::npos && n.find(". ") < 4))
                break;
            paragraph += ' ';
            paragraph += n;
            ++i;
        }
        Flow(ParseInline(paragraph), fonts, wrapWidth, 0.0f);
    }
}

}  // namespace aiad::app
