// engine_preset_annotations.cpp — Preset annotation system (presets.json).
//
// Persistent per-preset metadata: ratings, flags (favorite/error/skip/broken),
// notes, and auto-captured shader error text. Augments the existing fRating
// system stored in .milk files.

#include "engine.h"
#include "json_utils.h"
#include "utility.h"

namespace mdrop {

//----------------------------------------------------------------------
// Flag serialization helpers
//----------------------------------------------------------------------

static uint32_t FlagsFromJson(const JsonValue& arr) {
    uint32_t flags = 0;
    for (size_t i = 0; i < arr.size(); i++) {
        std::wstring s = arr.at(i).asString();
        if (s == L"favorite") flags |= PFLAG_FAVORITE;
        else if (s == L"error")  flags |= PFLAG_ERROR;
        else if (s == L"skip")   flags |= PFLAG_SKIP;
        else if (s == L"broken") flags |= PFLAG_BROKEN;
    }
    return flags;
}

static std::wstring FlagsToString(uint32_t flags) {
    std::wstring s = L"[";
    bool first = true;
    auto emit = [&](const wchar_t* name) {
        if (!first) s += L", ";
        s += L"\"";
        s += name;
        s += L"\"";
        first = false;
    };
    if (flags & PFLAG_FAVORITE) emit(L"favorite");
    if (flags & PFLAG_ERROR)    emit(L"error");
    if (flags & PFLAG_SKIP)     emit(L"skip");
    if (flags & PFLAG_BROKEN)   emit(L"broken");
    s += L"]";
    return s;
}

//----------------------------------------------------------------------
// LoadPresetAnnotations — read presets.json from disk
//----------------------------------------------------------------------

void Engine::LoadPresetAnnotations()
{
    wchar_t szPath[MAX_PATH];
    swprintf(szPath, MAX_PATH, L"%spresets.json", m_szBaseDir);

    JsonValue root = JsonLoadFile(szPath);
    if (root.isNull()) return;

    const JsonValue& arr = root[L"presets"];
    if (!arr.isArray()) return;

    m_presetAnnotations.clear();
    for (size_t i = 0; i < arr.size(); i++) {
        const JsonValue& item = arr.at(i);
        PresetAnnotation a;
        a.filename  = item[L"filename"].asString();
        a.rating    = item[L"rating"].asInt(0);
        a.flags     = FlagsFromJson(item[L"flags"]);
        a.notes     = item[L"notes"].asString();
        a.errorText = item[L"errorText"].asString();
        if (a.rating < 0) a.rating = 0;
        if (a.rating > 5) a.rating = 5;
        if (!a.filename.empty())
            m_presetAnnotations[a.filename] = std::move(a);
    }
    m_bAnnotationsDirty = false;
    DebugLogA("LoadPresetAnnotations: loaded %d entries", (int)m_presetAnnotations.size());
}

//----------------------------------------------------------------------
// SavePresetAnnotations — write presets.json to disk
//----------------------------------------------------------------------

void Engine::SavePresetAnnotations()
{
    if (!m_bAnnotationsDirty) return;

    // Build JSON manually — JsonWriter lacks anonymous array element support
    std::wostringstream ss;
    ss << L"{\n  \"presets\": [\n";

    bool first = true;
    for (auto& [key, a] : m_presetAnnotations) {
        if (!first) ss << L",\n";
        first = false;
        ss << L"    {\n";
        ss << L"      \"filename\": \"" << JsonEscape(a.filename) << L"\",\n";
        ss << L"      \"rating\": " << a.rating << L",\n";
        ss << L"      \"flags\": " << FlagsToString(a.flags) << L",\n";
        ss << L"      \"notes\": \"" << JsonEscape(a.notes) << L"\",\n";
        ss << L"      \"errorText\": \"" << JsonEscape(a.errorText) << L"\"\n";
        ss << L"    }";
    }

    ss << L"\n  ]\n}\n";

    wchar_t szPath[MAX_PATH];
    swprintf(szPath, MAX_PATH, L"%spresets.json", m_szBaseDir);
    JsonSaveFile(szPath, ss.str());
    m_bAnnotationsDirty = false;
}

//----------------------------------------------------------------------
// GetAnnotation — lookup by filename, optionally create
//----------------------------------------------------------------------

PresetAnnotation* Engine::GetAnnotation(const wchar_t* filename, bool create)
{
    if (!filename || !filename[0]) return nullptr;

    auto it = m_presetAnnotations.find(filename);
    if (it != m_presetAnnotations.end())
        return &it->second;

    if (!create) return nullptr;

    PresetAnnotation a;
    a.filename = filename;
    auto [iter, ok] = m_presetAnnotations.emplace(filename, std::move(a));
    return &iter->second;
}

//----------------------------------------------------------------------
// SetPresetFlag — set or clear a flag bit
//----------------------------------------------------------------------

void Engine::SetPresetFlag(const wchar_t* filename, uint32_t flag, bool set)
{
    PresetAnnotation* a = GetAnnotation(filename, set); // only create if setting
    if (!a) return;

    if (set)
        a->flags |= flag;
    else
        a->flags &= ~flag;

    m_bAnnotationsDirty = true;
    SavePresetAnnotations();
}

//----------------------------------------------------------------------
// SetPresetNote — set notes text
//----------------------------------------------------------------------

void Engine::SetPresetNote(const wchar_t* filename, const std::wstring& note)
{
    PresetAnnotation* a = GetAnnotation(filename, true);
    if (!a) return;
    a->notes = note;
    m_bAnnotationsDirty = true;
    SavePresetAnnotations();
}

//----------------------------------------------------------------------
// AutoFlagPresetError — called from shader compilation failure path
//----------------------------------------------------------------------

void Engine::AutoFlagPresetError(const wchar_t* filename, const std::wstring& errorMsg)
{
    if (!filename || !filename[0]) return;
    PresetAnnotation* a = GetAnnotation(filename, true);
    if (!a) return;
    a->flags |= PFLAG_ERROR;
    a->errorText = errorMsg;
    m_bAnnotationsDirty = true;
    SavePresetAnnotations();
}

//----------------------------------------------------------------------
// ParseAnnotationsFile — parse annotations from an arbitrary presets.json
//----------------------------------------------------------------------

std::unordered_map<std::wstring, PresetAnnotation>
Engine::ParseAnnotationsFile(const wchar_t* path)
{
    std::unordered_map<std::wstring, PresetAnnotation> result;
    JsonValue root = JsonLoadFile(path);
    if (root.isNull()) return result;

    const JsonValue& arr = root[L"presets"];
    if (!arr.isArray()) return result;

    for (size_t i = 0; i < arr.size(); i++) {
        const JsonValue& item = arr.at(i);
        PresetAnnotation a;
        a.filename  = item[L"filename"].asString();
        a.rating    = item[L"rating"].asInt(0);
        a.flags     = FlagsFromJson(item[L"flags"]);
        a.notes     = item[L"notes"].asString();
        a.errorText = item[L"errorText"].asString();
        if (a.rating < 0) a.rating = 0;
        if (a.rating > 5) a.rating = 5;
        if (!a.filename.empty())
            result[a.filename] = std::move(a);
    }
    return result;
}

//----------------------------------------------------------------------
// ScanPresetsForRatings — build a map from fRatingThis in loaded presets.
// Returns all presets with non-default ratings for user review.
//----------------------------------------------------------------------

std::unordered_map<std::wstring, PresetAnnotation> Engine::ScanPresetsForRatings()
{
    std::unordered_map<std::wstring, PresetAnnotation> result;
    for (int i = m_nDirs; i < m_nPresets; i++) {
        const wchar_t* fn = m_presets[i].szFilename.c_str();
        float r = m_presets[i].fRatingThis;
        int rating = (int)(r + 0.5f);
        if (rating < 0) rating = 0;
        if (rating > 5) rating = 5;

        PresetAnnotation a;
        a.filename = fn;
        a.rating = rating;
        result[fn] = std::move(a);
    }
    return result;
}

} // namespace mdrop
