// Well-known folders.
#pragma once

#include <filesystem>

namespace aiad
{
std::filesystem::path ExeDir();
std::filesystem::path HomeDir();          // %USERPROFILE%
std::filesystem::path AppDataDir();       // %APPDATA%\Tootega\AIAD (created on demand)
std::filesystem::path ClaudeConfigDir();  // CLAUDE_CONFIG_DIR or %USERPROFILE%\.claude

// Writes a whole file (UTF-8 bytes as given); false on failure.
bool WriteTextFile(const std::filesystem::path& path, std::string_view content);
std::string ReadTextFile(const std::filesystem::path& path);
}  // namespace aiad
