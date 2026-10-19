// Text helpers: UTF-8 <-> UTF-16 (Win32 boundary), Base64, random tokens.
#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace aiad
{
std::wstring Widen(std::string_view utf8);
std::string Narrow(std::wstring_view utf16);

std::string Base64Encode(const void* data, size_t size);
std::vector<uint8_t> Base64Decode(std::string_view text);

// Cryptographically random hex string (BCryptGenRandom), 2 * bytes characters.
std::string RandomHex(size_t bytes);

// Last Win32 error as text ("<code>: <message>").
std::string LastErrorText();
}  // namespace aiad
