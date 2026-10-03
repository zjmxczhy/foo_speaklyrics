#pragma once

#include <cstdint>
#include <cwctype>
#include <iomanip>
#include <sstream>
#include <string>

namespace speaklyrics_log_privacy {

inline uint64_t text_hash(const wchar_t* text) {
    constexpr uint64_t prime = 1099511628211ULL;
    uint64_t hash = 1469598103934665603ULL;
    if (!text) return hash;
    for (const wchar_t* cursor = text; *cursor; ++cursor) {
        const uint32_t value = static_cast<uint32_t>(*cursor);
        for (unsigned shift = 0; shift < 32; shift += 8) {
            hash ^= static_cast<uint8_t>((value >> shift) & 0xff);
            hash *= prime;
        }
    }
    return hash;
}

inline std::wstring hash_label(const std::wstring& text) {
    std::wostringstream out;
    out << std::uppercase << std::hex << std::setw(16) << std::setfill(L'0')
        << text_hash(text.c_str());
    return out.str();
}

inline std::wstring single_line(const std::wstring& text) {
    std::wstring out;
    out.reserve(text.size());
    for (const wchar_t ch : text) {
        if (ch == L'\r') out += L"\\r";
        else if (ch == L'\n') out += L"\\n";
        else if (ch == L'\t') out += L"\\t";
        else if (ch < L' ') out += L'?';
        else out += ch;
    }
    return out;
}

inline std::wstring private_text(const std::wstring& text, bool detailed) {
    if (detailed || text.empty()) return single_line(text);
    return L"[已隐藏；字符=" + std::to_wstring(text.size()) +
        L"；哈希=" + hash_label(text) + L"]";
}

inline std::wstring path_label(const std::wstring& path, bool detailed) {
    if (detailed || path.empty()) return single_line(path);
    std::wstring trimmed = path;
    while (!trimmed.empty() && (trimmed.back() == L'\\' || trimmed.back() == L'/'))
        trimmed.pop_back();
    const size_t slash = trimmed.find_last_of(L"\\/");
    std::wstring name = slash == std::wstring::npos ? L"[路径]" : trimmed.substr(slash + 1);
    // A drive root is not a file name.
    if (name.empty() || (name.size() == 2 && name[1] == L':')) name = L"[目录]";
    return single_line(name) + L" [路径哈希=" + hash_label(path) + L"]";
}

inline bool is_absolute_path_start(const std::wstring& text, size_t pos) {
    if (pos + 2 < text.size() && iswalpha(text[pos]) && text[pos + 1] == L':' &&
        (text[pos + 2] == L'\\' || text[pos + 2] == L'/')) return true;
    if (pos + 1 < text.size() && text[pos] == L'\\' && text[pos + 1] == L'\\')
        return true;
    return text.compare(pos, 7, L"file://") == 0;
}

// Fallback for paths embedded in a system/downloader error message. Normal
// path arguments should use path_label() before formatting, including names
// that themselves contain punctuation used to separate log fields.
inline std::wstring mask_embedded_paths(const std::wstring& text) {
    std::wstring out;
    for (size_t pos = 0; pos < text.size();) {
        if (!is_absolute_path_start(text, pos)) {
            out += text[pos++];
            continue;
        }
        size_t end = pos;
        while (end < text.size() && std::wstring(L"\r\n\"<>|，。；,").find(text[end]) ==
            std::wstring::npos) ++end;
        out += path_label(text.substr(pos, end - pos), false);
        pos = end;
    }
    return out;
}

inline std::wstring message(const std::wstring& text, bool detailed) {
    if (detailed) return single_line(text);
    std::wstring out = mask_embedded_paths(text);
    // All speech diagnostic content fields are the last field of the record.
    // This also protects a detailed excerpt obtained immediately before the
    // user turned off detailed logging on another thread.
    const size_t content = out.find(L"内容=");
    if (content != std::wstring::npos) out.replace(content + 3,
        std::wstring::npos, L"[已隐藏；可开启详细诊断日志查看]。");
    return single_line(out);
}

} // namespace speaklyrics_log_privacy
