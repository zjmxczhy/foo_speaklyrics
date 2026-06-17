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
};

static const lrc_encoding_candidate g_lrc_encoding_candidates[] = {
    { "utf8", L"UTF-8", CP_UTF8 },
    { "gb18030", L"GB18030", 54936 },
    { "gbk", L"GBK", 936 },
    { "big5", L"Big5", 950 },
    { "shiftjis", L"Shift-JIS", 932 },
    { "korean", L"Korean", 949 },
    { "windows1252", L"Windows-1252", 1252 },
    { "windows1254", L"Windows-1254", 1254 },
    { "acp", L"system ANSI", CP_ACP },
};

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
    if (need <= 0 && strict) {
        flags = 0;
        need = MultiByteToWideChar(codepage, flags, data, len, nullptr, 0);
    }
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
    if (id == "utf8" && bytes.size() >= 3 && bytes[0] == 0xEF && bytes[1] == 0xBB && bytes[2] == 0xBF) {
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

static std::wstring bytes_to_wide(const std::vector<uint8_t>& bytes) {
    if (bytes.empty()) return L"";

    if (bytes.size() >= 2 && bytes[0] == 0xFF && bytes[1] == 0xFE) {
        std::wstring out;
        if (decode_utf16_le(bytes.data(), bytes.size(), out)) return out;
    }
    if (bytes.size() >= 2 && bytes[0] == 0xFE && bytes[1] == 0xFF) {
        std::wstring out;
        if (decode_utf16_be(bytes.data(), bytes.size(), out)) return out;
    }

    std::string forced = cfg_lrc_encoding_id();
    if (!forced.empty() && forced != "auto") {
        bool ok = false;
        std::wstring out = decode_by_id(bytes, forced, ok);
        if (ok) return out;
    }

    const char* data = reinterpret_cast<const char*>(bytes.data());
    int len = static_cast<int>(bytes.size());
    if (bytes.size() >= 3 && bytes[0] == 0xEF && bytes[1] == 0xBB && bytes[2] == 0xBF) {
        bool ok = false;
        std::wstring out = decode_by_id(bytes, "utf8", ok);
        if (ok) return out;
    }

    std::wstring best;
    int bestScore = -1000000;
    for (const auto& candidate : g_lrc_encoding_candidates) {
        std::wstring decoded;
        if (!decode_codepage(data, len, candidate.codepage, candidate.codepage == CP_UTF8, decoded)) continue;
        int score = score_decoded_lrc(decoded);
        if (score > bestScore) {
            bestScore = score;
            best = std::move(decoded);
        }
    }
    return best;
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

bool lrc_document::load(const std::wstring& path, pfc::string8& error) {
    clear();
    std::ifstream f(path, std::ios::binary);
    if (!f) {
        error = "无法打开 LRC 文件。";
        return false;
    }
    std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    if (bytes.empty()) {
        error = "LRC 文件为空。";
        return false;
    }
    std::wstring text = bytes_to_wide(bytes);
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

