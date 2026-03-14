// json_utils.cpp — Lightweight JSON read/write for MDropDX12.

#include "json_utils.h"
#include <fstream>
#include <cwctype>
#include <cstring>

namespace mdrop {

static const JsonValue s_nullValue;

// ─── JsonValue accessors ────────────────────────────────────────────────

std::wstring JsonValue::asString(const wchar_t* def) const {
    if (type == String) return sVal;
    if (type == Number) {
        if (nVal == (int)nVal) return std::to_wstring((int)nVal);
        return std::to_wstring(nVal);
    }
    return def;
}

int JsonValue::asInt(int def) const {
    if (type == Number) return (int)nVal;
    if (type == String) { try { return std::stoi(sVal); } catch (...) {} }
    return def;
}

double JsonValue::asNumber(double def) const {
    if (type == Number) return nVal;
    if (type == String) { try { return std::stod(sVal); } catch (...) {} }
    return def;
}

float JsonValue::asFloat(float def) const {
    return (float)asNumber(def);
}

bool JsonValue::asBool(bool def) const {
    if (type == Bool) return bVal;
    if (type == Number) return nVal != 0;
    if (type == String) return sVal == L"true";
    return def;
}

const JsonValue& JsonValue::operator[](const wchar_t* key) const {
    if (type != Object) return s_nullValue;
    // Case-insensitive lookup (Milkwave Remote pattern)
    for (const auto& kv : members)
        if (_wcsicmp(kv.first.c_str(), key) == 0) return kv.second;
    return s_nullValue;
}

bool JsonValue::has(const wchar_t* key) const {
    if (type != Object) return false;
    for (const auto& kv : members)
        if (_wcsicmp(kv.first.c_str(), key) == 0) return true;
    return false;
}

size_t JsonValue::size() const {
    if (type == Array) return elements.size();
    if (type == Object) return members.size();
    return 0;
}

const JsonValue& JsonValue::at(size_t i) const {
    if (type == Array && i < elements.size()) return elements[i];
    return s_nullValue;
}

// ─── Escape / Unescape ─────────────────────────────────────────────────

std::wstring JsonEscape(const std::wstring& s) {
    std::wstring out;
    out.reserve(s.size() + 8);
    for (wchar_t c : s) {
        switch (c) {
        case L'\\': out += L"\\\\"; break;
        case L'"':  out += L"\\\""; break;
        case L'\n': out += L"\\n";  break;
        case L'\r': break;
        case L'\t': out += L"\\t";  break;
        default:
            if (c < 0x20) {
                // Escape control characters as \uXXXX (JSON spec requires this)
                wchar_t buf[8];
                swprintf(buf, 8, L"\\u%04x", (unsigned)c);
                out += buf;
            } else {
                out += c;
            }
            break;
        }
    }
    return out;
}

std::wstring JsonUnescape(const std::wstring& s) {
    std::wstring out;
    out.reserve(s.size());
    for (size_t i = 0; i < s.size(); i++) {
        if (s[i] == L'\\' && i + 1 < s.size()) {
            i++;
            switch (s[i]) {
            case L'n':  out += L'\n'; break;
            case L't':  out += L'\t'; break;
            case L'"':  out += L'"';  break;
            case L'\\': out += L'\\'; break;
            case L'/':  out += L'/';  break;
            case L'u':  // \uXXXX
                if (i + 4 < s.size()) {
                    wchar_t hex[5] = { s[i+1], s[i+2], s[i+3], s[i+4], 0 };
                    unsigned val = 0;
                    if (swscanf(hex, L"%x", &val) == 1)
                        out += (wchar_t)val;
                    i += 4;
                }
                break;
            default:    out += L'\\'; out += s[i]; break;
            }
        } else {
            out += s[i];
        }
    }
    return out;
}

// ─── Recursive-descent parser ───────────────────────────────────────────

namespace {

struct Parser {
    const wchar_t* p;
    const wchar_t* end;

    void SkipWS() {
        while (p < end && std::iswspace(*p)) p++;
        // Strip // line comments
        while (p + 1 < end && p[0] == L'/' && p[1] == L'/') {
            while (p < end && *p != L'\n') p++;
            while (p < end && std::iswspace(*p)) p++;
        }
    }

    wchar_t Peek() { SkipWS(); return p < end ? *p : 0; }
    void    Skip() { SkipWS(); if (p < end) p++; }

    std::wstring ParseString() {
        SkipWS();
        if (p >= end || *p != L'"') return L"";
        p++; // skip opening "
        std::wstring s;
        while (p < end && *p != L'"') {
            if (*p == L'\\' && p + 1 < end) {
                p++;
                switch (*p) {
                case L'n':  s += L'\n'; break;
                case L't':  s += L'\t'; break;
                case L'"':  s += L'"';  break;
                case L'\\': s += L'\\'; break;
                case L'/':  s += L'/';  break;
                case L'u':  // \uXXXX
                    if (p + 4 < end) {
                        wchar_t hex[5] = { p[1], p[2], p[3], p[4], 0 };
                        unsigned val = 0;
                        if (swscanf(hex, L"%x", &val) == 1)
                            s += (wchar_t)val;
                        p += 4;
                    }
                    break;
                default:    s += *p;    break;
                }
            } else {
                s += *p;
            }
            p++;
        }
        if (p < end) p++; // skip closing "
        return s;
    }

    JsonValue ParseValue() {
        SkipWS();
        if (p >= end) return {};

        wchar_t c = *p;

        // String
        if (c == L'"') {
            JsonValue v;
            v.type = JsonValue::String;
            v.sVal = ParseString();
            return v;
        }

        // Object
        if (c == L'{') {
            p++;
            JsonValue v;
            v.type = JsonValue::Object;
            while (Peek() != L'}' && Peek() != 0) {
                std::wstring key = ParseString();
                SkipWS();
                if (p < end && *p == L':') p++;
                v.members.push_back({ key, ParseValue() });
                SkipWS();
                if (p < end && *p == L',') p++;
            }
            if (p < end) p++; // skip }
            return v;
        }

        // Array
        if (c == L'[') {
            p++;
            JsonValue v;
            v.type = JsonValue::Array;
            while (Peek() != L']' && Peek() != 0) {
                v.elements.push_back(ParseValue());
                SkipWS();
                if (p < end && *p == L',') p++;
            }
            if (p < end) p++; // skip ]
            return v;
        }

        // true / false / null
        if (c == L't' && end - p >= 4 && wcsncmp(p, L"true", 4) == 0) {
            p += 4;
            JsonValue v; v.type = JsonValue::Bool; v.bVal = true;
            return v;
        }
        if (c == L'f' && end - p >= 5 && wcsncmp(p, L"false", 5) == 0) {
            p += 5;
            JsonValue v; v.type = JsonValue::Bool; v.bVal = false;
            return v;
        }
        if (c == L'n' && end - p >= 4 && wcsncmp(p, L"null", 4) == 0) {
            p += 4;
            return {};
        }

        // Number
        if (c == L'-' || (c >= L'0' && c <= L'9')) {
            const wchar_t* start = p;
            if (*p == L'-') p++;
            while (p < end && *p >= L'0' && *p <= L'9') p++;
            if (p < end && *p == L'.') {
                p++;
                while (p < end && *p >= L'0' && *p <= L'9') p++;
            }
            if (p < end && (*p == L'e' || *p == L'E')) {
                p++;
                if (p < end && (*p == L'+' || *p == L'-')) p++;
                while (p < end && *p >= L'0' && *p <= L'9') p++;
            }
            std::wstring numStr(start, p);
            JsonValue v;
            v.type = JsonValue::Number;
            try { v.nVal = std::stod(numStr); } catch (...) { v.nVal = 0; }
            return v;
        }

        // Unknown character — skip
        p++;
        return {};
    }
};

} // anon namespace

JsonValue JsonParse(const std::wstring& text) {
    if (text.empty()) return {};
    Parser parser;
    parser.p   = text.c_str();
    parser.end = text.c_str() + text.size();
    return parser.ParseValue();
}

JsonValue JsonLoadFile(const wchar_t* path) {
    // Read as UTF-8 binary and convert to wstring
    std::ifstream f(path, std::ios::binary);
    if (!f.is_open()) return {};
    std::string utf8((std::istreambuf_iterator<char>(f)),
                      std::istreambuf_iterator<char>());
    // Simple UTF-8 → wchar_t (BMP only, sufficient for JSON/HLSL/GLSL)
    std::wstring wide;
    wide.reserve(utf8.size());
    for (size_t i = 0; i < utf8.size(); ) {
        unsigned char c = utf8[i];
        if (c < 0x80) {
            wide += (wchar_t)c;
            i++;
        } else if (c < 0xE0 && i + 1 < utf8.size()) {
            wide += (wchar_t)(((c & 0x1F) << 6) | (utf8[i+1] & 0x3F));
            i += 2;
        } else if (c < 0xF0 && i + 2 < utf8.size()) {
            wide += (wchar_t)(((c & 0x0F) << 12) | ((utf8[i+1] & 0x3F) << 6) | (utf8[i+2] & 0x3F));
            i += 3;
        } else {
            wide += L'?';
            i++;
        }
    }
    return JsonParse(wide);
}

bool JsonSaveFile(const wchar_t* path, const std::wstring& jsonText) {
    // Write as UTF-8 binary to avoid locale-dependent encoding issues
    // (wofstream can choke on control characters in some locales).
    std::string utf8;
    utf8.reserve(jsonText.size());
    for (wchar_t c : jsonText) {
        if (c < 0x80) {
            utf8 += (char)c;
        } else if (c < 0x800) {
            utf8 += (char)(0xC0 | (c >> 6));
            utf8 += (char)(0x80 | (c & 0x3F));
        } else {
            utf8 += (char)(0xE0 | (c >> 12));
            utf8 += (char)(0x80 | ((c >> 6) & 0x3F));
            utf8 += (char)(0x80 | (c & 0x3F));
        }
    }
    std::ofstream f(path, std::ios::binary);
    if (!f.is_open()) return false;
    f.write(utf8.data(), utf8.size());
    return f.good();
}

// ─── JsonWriter ─────────────────────────────────────────────────────────

void JsonWriter::Newline() {
    m_ss << L"\n";
    for (int i = 0; i < m_indent; i++) m_ss << L"  ";
}

void JsonWriter::Comma() {
    if (m_needComma) m_ss << L",";
    m_needComma = true;
}

void JsonWriter::WriteKey(const wchar_t* key) {
    Comma();
    Newline();
    m_ss << L"\"" << key << L"\": ";
}

void JsonWriter::BeginObject() {
    if (!m_stack.empty()) {
        Comma();
        Newline();
    }
    m_ss << L"{";
    m_indent++;
    m_needComma = false;
    m_stack.push_back(false);
}

void JsonWriter::BeginObject(const wchar_t* key) {
    WriteKey(key);
    m_ss << L"{";
    m_indent++;
    m_needComma = false;
    m_stack.push_back(false);
}

void JsonWriter::EndObject() {
    m_indent--;
    Newline();
    m_ss << L"}";
    m_stack.pop_back();
    m_needComma = true;
}

void JsonWriter::BeginArray(const wchar_t* key) {
    WriteKey(key);
    m_ss << L"[";
    m_indent++;
    m_needComma = false;
    m_stack.push_back(false);
}

void JsonWriter::BeginArrayAnon() {
    Comma();
    Newline();
    m_ss << L"[";
    m_indent++;
    m_needComma = false;
    m_stack.push_back(false);
}

void JsonWriter::EndArray() {
    m_indent--;
    Newline();
    m_ss << L"]";
    m_stack.pop_back();
    m_needComma = true;
}

void JsonWriter::String(const wchar_t* key, const std::wstring& val) {
    WriteKey(key);
    m_ss << L"\"" << JsonEscape(val) << L"\"";
}

void JsonWriter::Int(const wchar_t* key, int val) {
    WriteKey(key);
    m_ss << val;
}

void JsonWriter::Float(const wchar_t* key, float val) {
    WriteKey(key);
    m_ss << val;
}

void JsonWriter::Bool(const wchar_t* key, bool val) {
    WriteKey(key);
    m_ss << (val ? L"true" : L"false");
}

bool JsonWriter::SaveToFile(const wchar_t* path) const {
    return JsonSaveFile(path, m_ss.str());
}

} // namespace mdrop
