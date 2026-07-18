#include "stdafx.h"
#include "lrc_parser.h"
#include "config.h"

static std::wstring trim_copy(std::wstring s) {
    const wchar_t* ws = L" \t\r\n";
    const auto b = s.find_first_not_of(ws);
    if (b == std::wstring::npos) return L"";
    const auto e = s.find_last_not_of(ws);
    return s.substr(b, e - b + 1);
}

struct lrc_encoding_candidate {
    const char* id;
    const wchar_t* name;
    UINT codepage;
    bool write_bom;
};

static const lrc_encoding_candidate g_lrc_encoding_candidates[] = {
    { "auto", L"\u81ea\u52a8", CP_UTF8, false },
    { "utf8", L"Unicode (UTF-8)", CP_UTF8, false },
    { "utf8bom", L"Unicode (UTF-8 with BOM)", CP_UTF8, true },
    { "utf16le", L"Unicode (UTF-16 LE)", 1200, true },
    { "utf16be", L"Unicode (UTF-16 BE)", 1201, true },
    { "gbk", L"Simplified Chinese (GBK)", 936, false },
    { "gb18030", L"Chinese Simplified (GB18030)", 54936, false },
    { "big5", L"Traditional Chinese (Big5)", 950, false },
    { "shiftjis", L"ANSI/OEM Japanese (Shift-JIS)", 932, false },
    { "korean", L"ANSI/OEM Korean (Unified Hangul Code)", 949, false },
    { "windows1252", L"Western European (Windows)", 1252, false },
    { "windows1254", L"Turkish (Windows)", 1254, false },
    { "acp", L"\u7cfb\u7edf\u9ed8\u8ba4 ANSI", CP_ACP, false },
};

static const lrc_encoding_info g_lrc_encoding_options[] = {
    { "auto", L"\u81ea\u52a8" },
    { "utf8", L"Unicode (UTF-8)" },
    { "utf8bom", L"Unicode (UTF-8 with BOM)" },
    { "utf16le", L"Unicode (UTF-16 LE)" },
    { "utf16be", L"Unicode (UTF-16 BE)" },
    { "gbk", L"Simplified Chinese (GBK)" },
    { "gb18030", L"Chinese Simplified (GB18030)" },
    { "big5", L"Traditional Chinese (Big5)" },
    { "shiftjis", L"ANSI/OEM Japanese (Shift-JIS)" },
    { "korean", L"ANSI/OEM Korean (Unified Hangul Code)" },
    { "windows1252", L"Western European (Windows)" },
    { "windows1254", L"Turkish (Windows)" },
    { "acp", L"\u7cfb\u7edf\u9ed8\u8ba4 ANSI" },
};

static bool same_encoding_id(const char* a, const char* b) {
    return a && b && _stricmp(a, b) == 0;
}

static const lrc_encoding_candidate* find_encoding_candidate(const char* id) {
    if (!id || !*id) return nullptr;
    for (const auto& candidate : g_lrc_encoding_candidates) {
        if (same_encoding_id(id, candidate.id)) return &candidate;
    }
    return nullptr;
}

static std::string cfg_lrc_encoding_id() {
    pfc::string8 value = cfg_lrc_encoding.get();
    std::string id = value.c_str();
    std::transform(id.begin(), id.end(), id.begin(), [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
    return id;
}

static bool decode_codepage(const char* data, int len, UINT codepage, bool strict, std::wstring& out) {
    out.clear();
    if (len <= 0) return true;
    DWORD flags = strict ? MB_ERR_INVALID_CHARS : 0;
    int need = MultiByteToWideChar(codepage, flags, data, len, nullptr, 0);
    if (need <= 0) return false;
    out.assign(static_cast<size_t>(need), L'\0');
    int written = MultiByteToWideChar(codepage, flags, data, len, out.data(), need);
    if (written <= 0) {
        out.clear();
        return false;
    }
    out.resize(static_cast<size_t>(written));
    return true;
}

static bool decode_utf16_le(const uint8_t* data, size_t size, std::wstring& out) {
    out.clear();
    if (size < 2) return true;
    size_t start = (size >= 2 && data[0] == 0xFF && data[1] == 0xFE) ? 2 : 0;
    if (((size - start) % 2) != 0) return false;
    out.reserve((size - start) / 2);
    for (size_t i = start; i + 1 < size; i += 2) {
        out.push_back(static_cast<wchar_t>(data[i] | (data[i + 1] << 8)));
    }
    return true;
}

static bool decode_utf16_be(const uint8_t* data, size_t size, std::wstring& out) {
    out.clear();
    if (size < 2) return true;
    size_t start = (size >= 2 && data[0] == 0xFE && data[1] == 0xFF) ? 2 : 0;
    if (((size - start) % 2) != 0) return false;
    out.reserve((size - start) / 2);
    for (size_t i = start; i + 1 < size; i += 2) {
        out.push_back(static_cast<wchar_t>((data[i] << 8) | data[i + 1]));
    }
    return true;
}

static bool looks_like_lrc_timestamp_at(const std::wstring& text, size_t pos) {
    return pos + 5 < text.size() && text[pos] == L'[' && iswdigit(text[pos + 1]) && iswdigit(text[pos + 2]) && text[pos + 3] == L':' && iswdigit(text[pos + 4]) && iswdigit(text[pos + 5]);
}

static int score_decoded_lrc(const std::wstring& text) {
    if (text.empty()) return -100000;
    int score = 0;
    int timestamps = 0;
    int replacement = 0;
    int controls = 0;
    int cjk = 0;
    int kana = 0;
    int hangul = 0;
    int latin = 0;
    for (size_t i = 0; i < text.size(); ++i) {
        wchar_t ch = text[i];
        if (looks_like_lrc_timestamp_at(text, i)) timestamps++;
        if (ch == 0xFFFD) replacement++;
        if (ch < 32 && ch != L'\r' && ch != L'\n' && ch != L'\t') controls++;
        if ((ch >= 0x4E00 && ch <= 0x9FFF) || (ch >= 0x3400 && ch <= 0x4DBF)) cjk++;
        else if ((ch >= 0x3040 && ch <= 0x30FF)) kana++;
        else if ((ch >= 0xAC00 && ch <= 0xD7AF)) hangul++;
        else if ((ch >= L'a' && ch <= L'z') || (ch >= L'A' && ch <= L'Z')) latin++;
    }
    score += timestamps * 1000;
    score += (std::min)(cjk + kana + hangul, 200) * 3;
    score += (std::min)(latin, 200);
    score -= replacement * 800;
    score -= controls * 500;
    if (text.find(L"???") != std::wstring::npos) score -= 2000;
    if (text.find(L"???") != std::wstring::npos) score -= 1000;
    if (text.find(L"?") != std::wstring::npos) score -= 200;
    return score;
}

static std::wstring decode_by_id(const std::vector<uint8_t>& bytes, const std::string& id, bool& ok) {
    ok = false;
    std::wstring out;
    if (id == "utf16le") {
        ok = decode_utf16_le(bytes.data(), bytes.size(), out);
        return out;
    }
    if (id == "utf16be") {
        ok = decode_utf16_be(bytes.data(), bytes.size(), out);
        return out;
    }
    const char* data = reinterpret_cast<const char*>(bytes.data());
    int len = static_cast<int>(bytes.size());
    if ((id == "utf8" || id == "utf8bom") && bytes.size() >= 3 && bytes[0] == 0xEF && bytes[1] == 0xBB && bytes[2] == 0xBF) {
        data += 3;
        len -= 3;
    }
    for (const auto& candidate : g_lrc_encoding_candidates) {
        if (id == candidate.id) {
            ok = decode_codepage(data, len, candidate.codepage, candidate.codepage == CP_UTF8, out);
            return out;
        }
    }
    ok = decode_codepage(data, len, CP_ACP, false, out);
    return out;
}

static std::wstring bytes_to_wide(const std::vector<uint8_t>& bytes, std::string* detected_encoding_id, bool use_forced_config) {
    if (detected_encoding_id) detected_encoding_id->clear();
    if (bytes.empty()) return L"";

    if (bytes.size() >= 2 && bytes[0] == 0xFF && bytes[1] == 0xFE) {
        std::wstring out;
        if (decode_utf16_le(bytes.data(), bytes.size(), out)) {
            if (detected_encoding_id) *detected_encoding_id = "utf16le";
            return out;
        }
    }
    if (bytes.size() >= 2 && bytes[0] == 0xFE && bytes[1] == 0xFF) {
        std::wstring out;
        if (decode_utf16_be(bytes.data(), bytes.size(), out)) {
            if (detected_encoding_id) *detected_encoding_id = "utf16be";
            return out;
        }
    }

    std::string forced = use_forced_config ? cfg_lrc_encoding_id() : std::string();
    if (!forced.empty() && forced != "auto") {
        bool ok = false;
        std::wstring out = decode_by_id(bytes, forced, ok);
        if (ok) {
            if (detected_encoding_id) *detected_encoding_id = forced;
            return out;
        }
    }

    const char* data = reinterpret_cast<const char*>(bytes.data());
    int len = static_cast<int>(bytes.size());
    if (bytes.size() >= 3 && bytes[0] == 0xEF && bytes[1] == 0xBB && bytes[2] == 0xBF) {
        bool ok = false;
        std::wstring out = decode_by_id(bytes, "utf8", ok);
        if (ok) {
            if (detected_encoding_id) *detected_encoding_id = "utf8bom";
            return out;
        }
    }

    {
        std::wstring utf8;
        if (decode_codepage(data, len, CP_UTF8, true, utf8)) {
            if (detected_encoding_id) *detected_encoding_id = "utf8";
            return utf8;
        }
    }

    std::wstring best;
    std::string bestId;
    int bestScore = -1000000;
    for (const auto& candidate : g_lrc_encoding_candidates) {
        if (same_encoding_id(candidate.id, "auto") || same_encoding_id(candidate.id, "utf8bom") || same_encoding_id(candidate.id, "utf16le") || same_encoding_id(candidate.id, "utf16be")) continue;
        std::wstring decoded;
        if (!decode_codepage(data, len, candidate.codepage, candidate.codepage == CP_UTF8, decoded)) continue;
        int score = score_decoded_lrc(decoded);
        if (score > bestScore) {
            bestScore = score;
            bestId = candidate.id;
            best = std::move(decoded);
        }
    }
    if (detected_encoding_id) *detected_encoding_id = bestId;
    return best;
}

static std::wstring bytes_to_wide(const std::vector<uint8_t>& bytes) {
    return bytes_to_wide(bytes, nullptr, true);
}

static bool parse_timestamp(const std::wstring& s, int& out_ms) {
    size_t colon = s.find(L':');
    if (colon == std::wstring::npos) return false;
    try {
        int minutes = std::stoi(s.substr(0, colon));
        size_t pos = colon + 1;
        int seconds = 0;
        while (pos < s.size() && iswdigit(s[pos])) {
            seconds = seconds * 10 + (s[pos] - L'0');
            ++pos;
        }
        int frac = 0;
        if (pos < s.size() && (s[pos] == L'.' || s[pos] == L':')) {
            ++pos;
            int digits = 0;
            while (pos < s.size() && iswdigit(s[pos]) && digits < 3) {
                frac = frac * 10 + (s[pos] - L'0');
                ++pos;
                ++digits;
            }
            if (digits == 1) frac *= 100;
            else if (digits == 2) frac *= 10;
        }
        if (minutes < 0 || seconds < 0 || seconds >= 60) return false;
        out_ms = minutes * 60000 + seconds * 1000 + frac;
        return true;
    } catch (...) {
        return false;
    }
}

static bool is_enhanced_timestamp_tag(const std::wstring& text, size_t open, size_t close) {
    if (open >= close || text[open] != L'<' || text[close] != L'>') return false;
    int ms = 0;
    return parse_timestamp(text.substr(open + 1, close - open - 1), ms);
}

static std::wstring strip_enhanced_lrc_tags(const std::wstring& text) {
    std::wstring out;
    out.reserve(text.size());
    for (size_t i = 0; i < text.size();) {
        if (text[i] == L'<') {
            size_t close = text.find(L'>', i + 1);
            if (close != std::wstring::npos && is_enhanced_timestamp_tag(text, i, close)) {
                i = close + 1;
                continue;
            }
        }
        out.push_back(text[i++]);
    }
    return out;
}

static void parse_line(const std::wstring& line, std::vector<lrc_line>& out, int& offset) {
    std::vector<int> times;
    size_t pos = 0;
    size_t textStart = 0;
    while (pos < line.size() && line[pos] == L'[') {
        size_t end = line.find(L']', pos + 1);
        if (end == std::wstring::npos) break;
        std::wstring tag = line.substr(pos + 1, end - pos - 1);
        int ms = 0;
        if (parse_timestamp(tag, ms)) {
            times.push_back(ms);
        } else {
            std::wstring low = tag;
            std::transform(low.begin(), low.end(), low.begin(), [](wchar_t c) { return (wchar_t)towlower(c); });
            if (low.rfind(L"offset:", 0) == 0) {
                try { offset = std::stoi(low.substr(7)); } catch (...) {}
            }
        }
        pos = end + 1;
        textStart = pos;
    }
    if (times.empty()) return;
    std::wstring text = trim_copy(strip_enhanced_lrc_tags(line.substr(textStart)));
    if (text.empty()) return;
    for (int t : times) out.push_back({ (std::max)(0, t + offset), text });
}

const lrc_encoding_info* lrc_get_encoding_options(size_t& count) {
    count = _countof(g_lrc_encoding_options);
    return g_lrc_encoding_options;
}

int lrc_find_encoding_index(const char* id) {
    if (!id || !*id) return 0;
    for (int i = 0; i < static_cast<int>(_countof(g_lrc_encoding_options)); ++i) {
        if (same_encoding_id(g_lrc_encoding_options[i].id, id)) return i;
    }
    return 0;
}

static std::wstring lrc_read_text_file_impl(const std::wstring& path, std::string* detected_encoding_id, pfc::string8& error, bool use_forced_config) {
    error = "";
    std::ifstream f(path, std::ios::binary);
    if (!f) {
        error = "无法打开 LRC 文件。";
        return std::wstring();
    }
    std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    if (bytes.empty()) {
        error = "LRC 文件为空。";
        return std::wstring();
    }
    return bytes_to_wide(bytes, detected_encoding_id, use_forced_config);
}

std::wstring lrc_read_text_file(const std::wstring& path, std::string* detected_encoding_id, pfc::string8& error) {
    return lrc_read_text_file_impl(path, detected_encoding_id, error, true);
}

std::wstring lrc_read_text_file_auto(const std::wstring& path, std::string* detected_encoding_id, pfc::string8& error) {
    return lrc_read_text_file_impl(path, detected_encoding_id, error, false);
}

static bool append_multibyte_encoded(std::vector<uint8_t>& bytes, const std::wstring& text, UINT codepage, bool strict) {
    if (text.empty()) return true;
    DWORD flags = strict ? WC_ERR_INVALID_CHARS : 0;
    int need = WideCharToMultiByte(codepage, flags, text.data(), static_cast<int>(text.size()), nullptr, 0, nullptr, nullptr);
    if (need <= 0 && strict) {
        flags = 0;
        need = WideCharToMultiByte(codepage, flags, text.data(), static_cast<int>(text.size()), nullptr, 0, nullptr, nullptr);
    }
    if (need <= 0) return false;
    size_t oldSize = bytes.size();
    bytes.resize(oldSize + static_cast<size_t>(need));
    int written = WideCharToMultiByte(codepage, flags, text.data(), static_cast<int>(text.size()), reinterpret_cast<char*>(bytes.data() + oldSize), need, nullptr, nullptr);
    if (written <= 0) {
        bytes.resize(oldSize);
        return false;
    }
    bytes.resize(oldSize + static_cast<size_t>(written));
    return true;
}

bool lrc_write_text_file(const std::wstring& path, const std::wstring& text, const char* encoding_id, pfc::string8& error) {
    error = "";
    const char* id = encoding_id && *encoding_id ? encoding_id : "utf8";
    if (same_encoding_id(id, "auto")) id = "utf8";
    const lrc_encoding_candidate* encoding = find_encoding_candidate(id);
    if (!encoding || same_encoding_id(encoding->id, "auto")) encoding = find_encoding_candidate("utf8");
    if (!encoding) {
        error = "不支持的 LRC 编码。";
        return false;
    }

    std::vector<uint8_t> bytes;
    if (same_encoding_id(encoding->id, "utf8bom")) {
        bytes.push_back(0xEF);
        bytes.push_back(0xBB);
        bytes.push_back(0xBF);
        if (!append_multibyte_encoded(bytes, text, CP_UTF8, true)) {
            error = "无法按 UTF-8 编码保存 LRC 文件。";
            return false;
        }
    } else if (same_encoding_id(encoding->id, "utf16le")) {
        bytes.push_back(0xFF);
        bytes.push_back(0xFE);
        for (wchar_t ch : text) {
            bytes.push_back(static_cast<uint8_t>(ch & 0xFF));
            bytes.push_back(static_cast<uint8_t>((ch >> 8) & 0xFF));
        }
    } else if (same_encoding_id(encoding->id, "utf16be")) {
        bytes.push_back(0xFE);
        bytes.push_back(0xFF);
        for (wchar_t ch : text) {
            bytes.push_back(static_cast<uint8_t>((ch >> 8) & 0xFF));
            bytes.push_back(static_cast<uint8_t>(ch & 0xFF));
        }
    } else {
        if (!append_multibyte_encoded(bytes, text, encoding->codepage, encoding->codepage == CP_UTF8)) {
            error = "无法按选择的编码保存 LRC 文件。";
            return false;
        }
    }

    std::ofstream f(path, std::ios::binary | std::ios::trunc);
    if (!f) {
        error = "无法写入 LRC 文件。";
        return false;
    }
    if (!bytes.empty()) f.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    if (!f) {
        error = "写入 LRC 文件失败。";
        return false;
    }
    return true;
}

bool lrc_document::load(const std::wstring& path, pfc::string8& error) {
    clear();
    std::wstring text = lrc_read_text_file(path, nullptr, error);
    if (error.length() > 0) return false;
    int offset = 0;
    size_t start = 0;
    while (start <= text.size()) {
        size_t end = text.find_first_of(L"\r\n", start);
        std::wstring line = end == std::wstring::npos ? text.substr(start) : text.substr(start, end - start);
        parse_line(line, m_lines, offset);
        if (end == std::wstring::npos) break;
        start = end + 1;
        if (start < text.size() && text[start - 1] == L'\r' && text[start] == L'\n') ++start;
    }
    std::sort(m_lines.begin(), m_lines.end(), [](const lrc_line& a, const lrc_line& b) { return a.time_ms < b.time_ms; });
    m_path = path;
    if (m_lines.empty()) {
        error = "没有解析到带时间标签的歌词行。";
        return false;
    }
    return true;
}

void lrc_document::clear() {
    m_lines.clear();
    m_path.clear();
}

int lrc_document::find_index_for_time(int ms) const {
    int result = -1;
    for (size_t i = 0; i < m_lines.size(); ++i) {
        if (m_lines[i].time_ms <= ms) result = static_cast<int>(i);
        else break;
    }
    return result;
}

const lrc_line* lrc_document::get(size_t index) const {
    return index < m_lines.size() ? &m_lines[index] : nullptr;
}

