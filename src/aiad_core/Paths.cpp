#include "Paths.h"

#include <windows.h>

#include <fstream>
#include <sstream>

namespace aiad
{

std::filesystem::path ExeDir()
{
    wchar_t path[MAX_PATH];
    GetModuleFileNameW(nullptr, path, MAX_PATH);
    return std::filesystem::path(path).parent_path();
}

static std::filesystem::path Env(const wchar_t* name)
{
    wchar_t buffer[MAX_PATH];
    const DWORD n = GetEnvironmentVariableW(name, buffer, MAX_PATH);
    return n > 0 && n < MAX_PATH ? std::filesystem::path(buffer) : std::filesystem::path();
}

std::filesystem::path HomeDir()
{
    return Env(L"USERPROFILE");
}

std::filesystem::path AppDataDir()
{
    std::filesystem::path dir = Env(L"APPDATA") / "Tootega" / "AIAD";
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    return dir;
}

std::filesystem::path ClaudeConfigDir()
{
    const std::filesystem::path configured = Env(L"CLAUDE_CONFIG_DIR");
    return configured.empty() ? HomeDir() / ".claude" : configured;
}

bool WriteTextFile(const std::filesystem::path& path, std::string_view content)
{
    std::error_code ec;
    if (path.has_parent_path())
        std::filesystem::create_directories(path.parent_path(), ec);
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out.write(content.data(), std::streamsize(content.size()));
    return bool(out);
}

std::string ReadTextFile(const std::filesystem::path& path)
{
    std::ifstream in(path, std::ios::binary);
    std::ostringstream text;
    text << in.rdbuf();
    return text.str();
}

}  // namespace aiad
