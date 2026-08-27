#pragma once
// json_utils.h — Lightweight JSON read/write for MDropDX12.
//
// Follows Milkwave Remote patterns: readable property names, pretty-printed
// output, case-insensitive loading, // comment support.
//
// Provides:
//   JsonValue   — DOM node representing any JSON value (object, array, string,
//                 number, bool, null).  Navigate with operator[] and at().
//   JsonWriter  — Builds indented JSON output incrementally.
//   JsonParse() — Recursive-descent parser (strips // line comments).
//   JsonEscape / JsonUnescape — String encoding helpers.

#include <string>
#include <vector>
#include <sstream>

namespace mdrop {

// ─── Escape / Unescape ──────────────────────────────────────────────────

std::wstring JsonEscape(const std::wstring& s);
std::wstring JsonUnescape(const std::wstring& s);

// ─── JsonValue ──────────────────────────────────────────────────────────

struct JsonValue {
    enum Type { Null, String, Number, Bool, Object, Array };
    Type         type = Null;
    std::wstring sVal;          // String
    double       nVal = 0;      // Number
    bool         bVal = false;  // Bool

    // Object: ordered key-value pairs
    std::vector<std::pair<std::wstring, JsonValue>> members;
    // Array: ordered values
    std::vector<JsonValue> elements;

    // Constructors
    JsonValue() = default;
    JsonValue(const wchar_t* s)        : type(String), sVal(s) {}
    JsonValue(const std::wstring& s)   : type(String), sVal(s) {}
    JsonValue(int n)                   : type(Number), nVal(n) {}
    JsonValue(double n)                : type(Number), nVal(n) {}
    JsonValue(bool b)                  : type(Bool), bVal(b) {}

    // Accessors with defaults
    std::wstring asString(const wchar_t* def = L"") const;
    int          asInt(int def = 0) const;
    double       asNumber(double def = 0) const;
    float        asFloat(float def = 0.f) const;
    bool         asBool(bool def = false) const;

    // Object member access
    const JsonValue& operator[](const wchar_t* key) const;
    bool has(const wchar_t* key) const;

    // Array element access
    size_t           size() const;
    const JsonValue& at(size_t i) const;

    // Type checks
    bool isNull()   const { return type == Null; }
    bool isString() const { return type == String; }
    bool isNumber() const { return type == Number; }
    bool isBool()   const { return type == Bool; }
    bool isObject() const { return type == Object; }
    bool isArray()  const { return type == Array; }
};

// Parse JSON text (strips // line comments)
JsonValue JsonParse(const std::wstring& text);

// File I/O
JsonValue JsonLoadFile(const wchar_t* path);
bool      JsonSaveFile(const wchar_t* path, const std::wstring& jsonText);

// ─── JsonWriter ─────────────────────────────────────────────────────────

class JsonWriter {
public:
    void BeginObject();
    void BeginObject(const wchar_t* key);  // named nested object
    void EndObject();
    void BeginArray(const wchar_t* key);
    void BeginArrayAnon();   // inside another array
    void EndArray();

    void String(const wchar_t* key, const std::wstring& val);
    void Int(const wchar_t* key, int val);
    void Float(const wchar_t* key, float val);

    // Nine significant digits — the round-trip width for binary32. Float()
    // uses the stream default of six, which turns a transcribed constant like
    // 0.326781557 into 0.326782. Separate from Float() on purpose: widening
    // every number would change JSON that characterization tests pin.
    void FloatPrecise(const wchar_t* key, float val);
    void FloatPreciseAnon(float val);   // inside an array
    void Bool(const wchar_t* key, bool val);

    // Write a parsed value back out verbatim, whatever shape it has.
    // Needed when a file holds sections this build does not understand: a
    // rewrite has to put them back rather than silently dropping them.
    void Value(const wchar_t* key, const JsonValue& v);
    void ValueAnon(const JsonValue& v);   // inside an array

    std::wstring ToString() const { return m_ss.str(); }
    bool SaveToFile(const wchar_t* path) const;

private:
    std::wostringstream m_ss;
    int  m_indent = 0;
    bool m_needComma = false;
    std::vector<bool> m_stack;  // nesting tracker

    void Newline();
    void Comma();
    void WriteKey(const wchar_t* key);
};

} // namespace mdrop
