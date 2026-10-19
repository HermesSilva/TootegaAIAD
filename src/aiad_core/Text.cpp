#include "Text.h"

#include <windows.h>
#include <bcrypt.h>

#include <algorithm>
#include <iterator>

namespace aiad
{

std::wstring Widen(std::string_view utf8)
{
    if (utf8.empty())
        return {};
    const int n = MultiByteToWideChar(CP_UTF8, 0, utf8.data(), int(utf8.size()), nullptr, 0);
    std::wstring out(size_t(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, utf8.data(), int(utf8.size()), out.data(), n);
    return out;
}

std::string Narrow(std::wstring_view utf16)
{
    if (utf16.empty())
        return {};
    const int n = WideCharToMultiByte(CP_UTF8, 0, utf16.data(), int(utf16.size()), nullptr, 0, nullptr, nullptr);
    std::string out(size_t(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, utf16.data(), int(utf16.size()), out.data(), n, nullptr, nullptr);
    return out;
}

static constexpr char kAlphabet[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

std::string Base64Encode(const void* data, size_t size)
{
    const auto* p = static_cast<const uint8_t*>(data);
    std::string out;
    out.reserve((size + 2) / 3 * 4);
    for (size_t i = 0; i < size; i += 3)
    {
        const uint32_t a = p[i], b = i + 1 < size ? p[i + 1] : 0, c = i + 2 < size ? p[i + 2] : 0;
        const uint32_t v = (a << 16) | (b << 8) | c;
        out += kAlphabet[(v >> 18) & 63];
        out += kAlphabet[(v >> 12) & 63];
        out += i + 1 < size ? kAlphabet[(v >> 6) & 63] : '=';
        out += i + 2 < size ? kAlphabet[v & 63] : '=';
    }
    return out;
}

std::vector<uint8_t> Base64Decode(std::string_view text)
{
    int table[256];
    std::fill(std::begin(table), std::end(table), -1);
    for (int i = 0; i < 64; ++i)
        table[uint8_t(kAlphabet[i])] = i;
    std::vector<uint8_t> out;
    out.reserve(text.size() * 3 / 4);
    uint32_t buffer = 0;
    int bits = 0;
    for (const char ch : text)
    {
        const int v = table[uint8_t(ch)];
        if (v < 0)
            continue;  // padding, whitespace
        buffer = (buffer << 6) | uint32_t(v);
        bits += 6;
        if (bits >= 8)
        {
            bits -= 8;
            out.push_back(uint8_t(buffer >> bits));
        }
    }
    return out;
}

std::string RandomHex(size_t bytes)
{
    std::vector<uint8_t> raw(bytes);
    BCryptGenRandom(nullptr, raw.data(), ULONG(raw.size()), BCRYPT_USE_SYSTEM_PREFERRED_RNG);
    static constexpr char hex[] = "0123456789abcdef";
    std::string out;
    for (const uint8_t b : raw)
    {
        out += hex[b >> 4];
        out += hex[b & 15];
    }
    return out;
}

std::string LastErrorText()
{
    const DWORD code = GetLastError();
    wchar_t* message = nullptr;
    FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS, nullptr, code, 0,
                   reinterpret_cast<wchar_t*>(&message), 0, nullptr);
    std::string text = std::to_string(code);
    if (message)
    {
        std::string m = Narrow(message);
        while (!m.empty() && (m.back() == '\n' || m.back() == '\r' || m.back() == ' '))
            m.pop_back();
        text += ": " + m;
        LocalFree(message);
    }
    return text;
}

}  // namespace aiad
