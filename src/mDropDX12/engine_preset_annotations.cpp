// engine_preset_annotations.cpp — Preset annotation system (presets.json).
//
// Persistent per-preset metadata: ratings, flags (favorite/error/skip/broken),
// notes, and auto-captured shader error text. Augments the existing fRating
// system stored in .milk files.

#include "engine.h"
#include "json_utils.h"
#include "utility.h"
#include <set>
#include <algorithm>

namespace mdrop {

extern CRITICAL_SECTION g_cs;
extern CRITICAL_SECTION g_csPresetPending;
extern volatile bool g_bThreadAlive;
extern volatile int g_bThreadShouldQuit;
void CancelThread(int max_wait_time_ms);

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
        const JsonValue& tagsArr = item[L"tags"];
        if (tagsArr.isArray()) {
            for (size_t t = 0; t < tagsArr.size(); t++) {
                std::wstring tag = tagsArr.at(t).asString();
                if (!tag.empty()) a.tags.push_back(tag);
            }
        }
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
        ss << L"      \"errorText\": \"" << JsonEscape(a.errorText) << L"\",\n";
        ss << L"      \"tags\": [";
        for (size_t t = 0; t < a.tags.size(); t++) {
            if (t > 0) ss << L", ";
            ss << L"\"" << JsonEscape(a.tags[t]) << L"\"";
        }
        ss << L"]\n";
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
        const JsonValue& tagsArr = item[L"tags"];
        if (tagsArr.isArray()) {
            for (size_t t = 0; t < tagsArr.size(); t++) {
                std::wstring tag = tagsArr.at(t).asString();
                if (!tag.empty()) a.tags.push_back(tag);
            }
        }
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

//----------------------------------------------------------------------
// SetPresetTags — set tags for a preset
//----------------------------------------------------------------------

void Engine::SetPresetTags(const wchar_t* filename, const std::vector<std::wstring>& tags)
{
    PresetAnnotation* a = GetAnnotation(filename, true);
    if (!a) return;
    a->tags = tags;
    m_bAnnotationsDirty = true;
    SavePresetAnnotations();
}

//----------------------------------------------------------------------
// CollectAllTags — gather unique sorted list of all tags across annotations
//----------------------------------------------------------------------

void Engine::CollectAllTags(std::vector<std::wstring>& allTags) const
{
    std::set<std::wstring> tagSet;
    for (auto& [key, a] : m_presetAnnotations) {
        for (auto& t : a.tags)
            tagSet.insert(t);
    }
    allTags.assign(tagSet.begin(), tagSet.end());
}

//----------------------------------------------------------------------
// ImportMWRTags — import tags from Milkwave Remote's tags-remote.json
// Format: { "TagEntries": { "display_name": { "PresetPath": "...", "Tags": [...] } } }
// Matches by filename (last component of PresetPath or display name).
// Returns count of presets that got new tags merged.
//----------------------------------------------------------------------

int Engine::ImportMWRTags(const wchar_t* szTagsJsonPath)
{
    JsonValue root = JsonLoadFile(szTagsJsonPath);
    if (root.isNull()) return 0;

    const JsonValue& entries = root[L"TagEntries"];
    if (entries.isNull()) return 0;

    int nUpdated = 0;

    // Iterate all keys in TagEntries (object members)
    for (auto& [key, entry] : entries.members) {
        if (entry.isNull()) continue;

        // Get tags array
        const JsonValue& tagsArr = entry[L"Tags"];
        if (!tagsArr.isArray() || tagsArr.size() == 0) continue;

        std::vector<std::wstring> mwrTags;
        for (size_t t = 0; t < tagsArr.size(); t++) {
            std::wstring tag = tagsArr.at(t).asString();
            if (!tag.empty()) mwrTags.push_back(tag);
        }
        if (mwrTags.empty()) continue;

        // Extract filename from PresetPath (last path component)
        std::wstring presetPath = entry[L"PresetPath"].asString();
        std::wstring filename;
        if (!presetPath.empty()) {
            size_t lastSlash = presetPath.find_last_of(L"\\/");
            filename = (lastSlash != std::wstring::npos) ? presetPath.substr(lastSlash + 1) : presetPath;
        }
        // Fallback: use the key itself (display name) — may have subdir prefix
        if (filename.empty()) {
            size_t lastSlash = key.find_last_of(L"\\/");
            filename = (lastSlash != std::wstring::npos) ? key.substr(lastSlash + 1) : key;
            // MWR display names omit extension — try adding common extensions
            if (filename.find(L'.') == std::wstring::npos)
                filename += L".milk";
        }

        if (filename.empty()) continue;

        // Find or create annotation and merge tags
        PresetAnnotation* a = GetAnnotation(filename.c_str(), true);
        if (!a) continue;

        bool changed = false;
        for (auto& newTag : mwrTags) {
            bool found = false;
            for (auto& existing : a->tags) {
                if (_wcsicmp(existing.c_str(), newTag.c_str()) == 0) {
                    found = true;
                    break;
                }
            }
            if (!found) {
                a->tags.push_back(newTag);
                changed = true;
            }
        }
        if (changed) nUpdated++;
    }

    if (nUpdated > 0) {
        m_bAnnotationsDirty = true;
        SavePresetAnnotations();
    }

    DLOG_INFO("ImportMWRTags: %d presets updated from %ls", nUpdated, szTagsJsonPath);
    return nUpdated;
}

//----------------------------------------------------------------------
// Preset Lists — save/load named subsets of presets (one path per line)
//----------------------------------------------------------------------

void Engine::GetPresetListDir(wchar_t* szDir, int nMax) const
{
    swprintf(szDir, nMax, L"%spreset_lists\\", m_szBaseDir);
}

void Engine::EnumPresetLists(std::vector<std::wstring>& names) const
{
    names.clear();
    wchar_t szDir[MAX_PATH];
    GetPresetListDir(szDir, MAX_PATH);

    wchar_t szMask[MAX_PATH];
    swprintf(szMask, L"%s*.txt", szDir);

    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW(szMask, &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) {
            // Strip .txt extension for display
            std::wstring name = fd.cFileName;
            size_t dot = name.rfind(L'.');
            if (dot != std::wstring::npos) name = name.substr(0, dot);
            names.push_back(name);
        }
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    std::sort(names.begin(), names.end());
}

bool Engine::SavePresetList(const wchar_t* listName)
{
    wchar_t szDir[MAX_PATH];
    GetPresetListDir(szDir, MAX_PATH);
    CreateDirectoryW(szDir, NULL);

    wchar_t szPath[MAX_PATH];
    swprintf(szPath, MAX_PATH, L"%s%s.txt", szDir, listName);

    FILE* f = _wfopen(szPath, L"w, ccs=UTF-8");
    if (!f) return false;

    fwprintf(f, L"# Preset list: %s\n", listName);
    fwprintf(f, L"@basedir=%s\n", m_szPresetDir);

    for (int i = m_nDirs; i < m_nPresets; i++) {
        // Save absolute paths so the list works regardless of current preset dir
        wchar_t szFile[MAX_PATH];
        BuildPresetPath(i, szFile, MAX_PATH);
        fwprintf(f, L"%s\n", szFile);
    }

    fclose(f);
    m_szActivePresetList = listName;
    DLOG_INFO("SavePresetList: saved %d presets to %ls", m_nPresets - m_nDirs, szPath);
    return true;
}

bool Engine::LoadPresetList(const wchar_t* listPath)
{
    FILE* f = _wfopen(listPath, L"r, ccs=UTF-8");
    if (!f) return false;

    PresetList temp_presets;
    int temp_nPresets = 0;
    wchar_t line[MAX_PATH];
    wchar_t savedBaseDir[MAX_PATH] = {};
    int baseDirLen = 0;

    // Also extract list file's directory as fallback basedir
    wchar_t listDir[MAX_PATH] = {};
    lstrcpynW(listDir, listPath, MAX_PATH);
    wchar_t* pLastSlash = wcsrchr(listDir, L'\\');
    if (pLastSlash) pLastSlash[1] = 0;
    else listDir[0] = 0;

    while (fgetws(line, MAX_PATH, f)) {
        // Strip newline
        int len = (int)wcslen(line);
        while (len > 0 && (line[len-1] == L'\n' || line[len-1] == L'\r'))
            line[--len] = 0;
        if (len == 0) continue;

        // Parse comments — look for "# Base directory:" as basedir fallback
        if (line[0] == L'#') {
            if (baseDirLen == 0 && _wcsnicmp(line, L"# Base directory:", 17) == 0) {
                const wchar_t* p = line + 17;
                while (*p == L' ') p++;
                lstrcpynW(savedBaseDir, p, MAX_PATH);
                baseDirLen = lstrlenW(savedBaseDir);
                // Ensure trailing backslash
                if (baseDirLen > 0 && savedBaseDir[baseDirLen-1] != L'\\') {
                    savedBaseDir[baseDirLen] = L'\\';
                    savedBaseDir[baseDirLen+1] = 0;
                    baseDirLen++;
                }
            }
            continue;
        }

        // Parse @basedir= header
        if (wcsncmp(line, L"@basedir=", 9) == 0) {
            lstrcpynW(savedBaseDir, line + 9, MAX_PATH);
            baseDirLen = lstrlenW(savedBaseDir);
            continue;
        }

        // Always store absolute paths so BuildPresetPath works regardless of m_szPresetDir
        const wchar_t* fn = line;
        bool bAbsolute = (fn[0] && fn[1] == L':') || (fn[0] == L'\\' && fn[1] == L'\\');

        wchar_t resolved[MAX_PATH];
        if (!bAbsolute) {
            // Relative path — resolve to absolute using basedir or list file directory
            const wchar_t* base = (baseDirLen > 0) ? savedBaseDir : listDir;
            if (base[0]) {
                swprintf(resolved, MAX_PATH, L"%s%s", base, fn);
                fn = resolved;
            }
        }
        // Absolute paths stored as-is

        PresetInfo pi;
        pi.szFilename = fn;
        pi.fRatingThis = 3.0f;
        pi.fRatingCum = (temp_nPresets > 0 ? temp_presets[temp_nPresets-1].fRatingCum : 0) + 3.0f;
        temp_presets.push_back(pi);
        temp_nPresets++;
    }
    fclose(f);

    if (temp_nPresets == 0) return false;

    // Cancel any running background preset scan so it doesn't overwrite our list
    if (g_bThreadAlive)
        CancelThread(500);

    // Clear any pending swap that a just-finished scan might have queued
    EnterCriticalSection(&g_csPresetPending);
    m_bPendingPresetSwap.store(false, std::memory_order_release);
    m_bPendingRatingsSwap.store(false, std::memory_order_release);
    LeaveCriticalSection(&g_csPresetPending);

    // Replace the current preset list directly (called from main/UI thread)
    EnterCriticalSection(&g_cs);
    m_presets = std::move(temp_presets);
    m_nPresets = temp_nPresets;
    m_nDirs = 0;
    m_nPresetListCurPos = 0;
    m_bPresetListReady = true;
    m_nCurrentPreset = -1;
    m_bRecursivePresets = true;  // list acts like recursive mode (no dir navigation)
    LeaveCriticalSection(&g_cs);

    // Extract list name from path for display
    const wchar_t* pName = wcsrchr(listPath, L'\\');
    pName = pName ? (pName + 1) : listPath;
    m_szActivePresetList = pName;
    size_t dot = m_szActivePresetList.rfind(L'.');
    if (dot != std::wstring::npos) m_szActivePresetList = m_szActivePresetList.substr(0, dot);

    DLOG_INFO("LoadPresetList: loaded %d presets from %ls", temp_nPresets, listPath);
    return true;
}

} // namespace mdrop
