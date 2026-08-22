// engine_preset_annotations.cpp — Preset annotation system (presets.json).
//
// Persistent per-preset metadata: ratings, flags (favorite/error/skip/broken),
// notes, and auto-captured shader error text. Augments the existing fRating
// system stored in .milk files.

#include "engine.h"
#include "preset_hash.h"
#include "shader_overrides.h"
#include "json_utils.h"
#include "utility.h"
#include <set>
#include <algorithm>

namespace mdrop {

// Locations recorded per preset.  Capped so a preset that turns up in hundreds
// of scanned folders cannot grow presets.json without bound.
static const size_t kMaxAnnotationPaths = 32;

// Annotation keys are bare filenames.
//
// m_szCurrentPresetFile holds a FULL PATH when a preset is loaded over IPC or
// from the command line, and a bare filename when it comes from the browser.
// Keying on the raw string gave the same preset two entries depending on how
// it had been opened, which is the confusion content identity exists to remove
// -- so every lookup normalizes first.
static std::wstring BareFilename(const wchar_t* fileOrPath) {
    if (!fileOrPath || !fileOrPath[0]) return std::wstring();
    std::wstring s(fileOrPath);
    size_t cut = s.find_last_of(L"\\/");
    return (cut == std::wstring::npos) ? s : s.substr(cut + 1);
}

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
        a.hash      = item[L"hash"].asString();
        a.lastUsed  = item[L"lastUsed"].asString();
        a.useCount  = item[L"useCount"].asInt(0);
        a.secondsShown = item[L"secondsShown"].asInt(0);

        const JsonValue& pathsArr = item[L"paths"];
        if (pathsArr.isArray()) {
            for (size_t t = 0; t < pathsArr.size(); t++) {
                std::wstring path = pathsArr.at(t).asString();
                if (!path.empty()) a.paths.push_back(path);
            }
        }

        const JsonValue& ratingsArr = item[L"ratings"];
        if (ratingsArr.isArray()) {
            for (size_t t = 0; t < ratingsArr.size(); t++) {
                const JsonValue& r = ratingsArr.at(t);
                RatingObservation obs;
                obs.hash  = r[L"hash"].asString();
                obs.value = r[L"value"].asInt(0);
                obs.when  = r[L"when"].asString();
                if (obs.value < 0) obs.value = 0;
                if (obs.value > 5) obs.value = 5;
                if (obs.value > 0) a.ratings.push_back(obs);
            }
        }
        const JsonValue& tagsArr = item[L"tags"];
        if (tagsArr.isArray()) {
            for (size_t t = 0; t < tagsArr.size(); t++) {
                std::wstring tag = tagsArr.at(t).asString();
                if (!tag.empty()) a.tags.push_back(tag);
            }
        }
        // Presence is the meaning here, not the value -- see PresetAnnotation.
        if (item.has(L"shaderOverride")) {
            a.hasShaderOverride = true;
            a.shaderOverride = item[L"shaderOverride"].asString();
        }
        if (item.has(L"vfxProfile")) {
            a.hasVfxProfile = true;
            a.vfxProfile = item[L"vfxProfile"].asString();
        }

        if (a.rating < 0) a.rating = 0;
        if (a.rating > 5) a.rating = 5;

        // Back-compat: an entry written before rating history existed carries a
        // bare "rating".  Adopt it as one observation against that entry's hash
        // so the old opinion survives and averages with anything added later.
        if (a.ratings.empty() && a.rating > 0) {
            RatingObservation obs;
            obs.hash  = a.hash;
            obs.value = a.rating;
            a.ratings.push_back(obs);
        }

        if (!a.filename.empty())
            m_presetAnnotations[a.filename] = std::move(a);
    }
    RebuildAnnotationHashIndex();
    m_bAnnotationsDirty = false;
    DLOG_INFO("LoadPresetAnnotations: loaded %d entries (%d with a hash)",
              (int)m_presetAnnotations.size(), (int)m_annotationsByHash.size());
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
        ss << L"      \"hash\": \"" << JsonEscape(a.hash) << L"\",\n";

        ss << L"      \"paths\": [";
        for (size_t t = 0; t < a.paths.size(); t++) {
            if (t > 0) ss << L", ";
            ss << L"\"" << JsonEscape(a.paths[t]) << L"\"";
        }
        ss << L"],\n";

        ss << L"      \"ratings\": [";
        for (size_t t = 0; t < a.ratings.size(); t++) {
            if (t > 0) ss << L", ";
            ss << L"{ \"hash\": \"" << JsonEscape(a.ratings[t].hash)
               << L"\", \"value\": " << a.ratings[t].value
               << L", \"when\": \"" << JsonEscape(a.ratings[t].when) << L"\" }";
        }
        ss << L"],\n";

        // Derived average, kept so every reader that predates rating history --
        // including older builds of this program -- still finds the field it
        // expects.  AverageRating() is the source of truth.
        ss << L"      \"rating\": " << AverageRating(a) << L",\n";
        ss << L"      \"flags\": " << FlagsToString(a.flags) << L",\n";
        ss << L"      \"notes\": \"" << JsonEscape(a.notes) << L"\",\n";
        ss << L"      \"errorText\": \"" << JsonEscape(a.errorText) << L"\",\n";
        ss << L"      \"tags\": [";
        for (size_t t = 0; t < a.tags.size(); t++) {
            if (t > 0) ss << L", ";
            ss << L"\"" << JsonEscape(a.tags[t]) << L"\"";
        }
        ss << L"],\n";
        // Emitted ONLY when set. Writing these unconditionally would turn every
        // entry's "inherit from tags" into "explicitly none" -- they are
        // different states and the file is where that distinction lives.
        if (a.hasShaderOverride)
            ss << L"      \"shaderOverride\": \"" << JsonEscape(a.shaderOverride) << L"\",\n";
        if (a.hasVfxProfile)
            ss << L"      \"vfxProfile\": \"" << JsonEscape(a.vfxProfile) << L"\",\n";
        ss << L"      \"lastUsed\": \"" << JsonEscape(a.lastUsed) << L"\",\n";
        ss << L"      \"useCount\": " << a.useCount << L",\n";
        ss << L"      \"secondsShown\": " << a.secondsShown << L"\n";
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
    const std::wstring key = BareFilename(filename);
    if (key.empty()) return nullptr;

    auto it = m_presetAnnotations.find(key);
    if (it != m_presetAnnotations.end())
        return &it->second;

    if (!create) return nullptr;

    PresetAnnotation a;
    a.filename = key;
    auto [iter, ok] = m_presetAnnotations.emplace(key, std::move(a));
    return &iter->second;
}

//----------------------------------------------------------------------
// ResolveShaderOverrideForPreset — tags -> rule -> override
//
// Runs on the preset-load thread, after Import and before the shaders compile.
// Nothing is written into pState: see ActiveShaderOverride in engine.h.
//----------------------------------------------------------------------

void Engine::ResolveShaderOverrideForPreset(CState* pState)
{
    // Leaving whatever preset was on before this one. Done FIRST and outside
    // every early return below, so the VFX state is restored even when the new
    // preset resolves to nothing -- including when overrides are switched off.
    PopScopedVFXProfile();

    m_activeOverride.Clear();
    m_resolvedVFXProfile.clear();
    m_resolvedVFXSource = OverrideSource::None;
    if (!pState) return;

    ShaderOverrideStore& store = ShaderOverrides();
    // The master switch governs BOTH slots. A per-preset entry is a more
    // specific selection, not a way around the off switch.
    if (!store.IsEnabled()) return;

    // Tags come from the annotation, found by content hash first so a preset
    // keeps its rules wherever it lives on disk.
    const char* h = pState->m_szPresetHash;
    std::wstring wHash(h, h + strlen(h));

    PresetAnnotation* a = GetAnnotationByHash(wHash);
    if (!a) a = GetAnnotation(m_szLoadingPreset[0] ? m_szLoadingPreset
                                                  : m_szCurrentPresetFile, false);
    // NOT "|| a->tags.empty()": a preset may carry a per-preset override and no
    // tags at all, and that is the whole point of the per-preset entry.
    if (!a) return;

    std::wstring matchedTag;
    const ShaderRule* rule = a->tags.empty() ? nullptr
                                             : store.ResolveRule(a->tags, &matchedTag);

    // ── shader slot: the preset's own entry wins, then the rule ──
    std::wstring shaderName;
    OverrideSource shaderSrc = OverrideSource::None;
    if (a->hasShaderOverride) {
        shaderName = a->shaderOverride;          // may be empty: explicitly none
        shaderSrc  = OverrideSource::Preset;
    } else if (rule && !rule->overrideName.empty()) {
        shaderName = rule->overrideName;
        shaderSrc  = OverrideSource::Rule;
    }

    if (!shaderName.empty()) {
        if (const ShaderOverride* o = store.Find(shaderName)) {
            m_activeOverride.name       = o->name;
            m_activeOverride.warpText   = o->warpText;
            m_activeOverride.compText   = o->compText;
            m_activeOverride.fromRule   = (shaderSrc == OverrideSource::Rule);
            m_activeOverride.matchedTag = matchedTag;
        }
    }
    m_activeOverride.source = shaderSrc;

    // ── VFX slot: same precedence, resolved independently ──
    if (a->hasVfxProfile) {
        m_resolvedVFXProfile = a->vfxProfile;    // may be empty: explicitly none
        m_resolvedVFXSource  = OverrideSource::Preset;
    } else if (rule && !rule->vfxProfile.empty()) {
        m_resolvedVFXProfile = rule->vfxProfile;
        m_resolvedVFXSource  = OverrideSource::Rule;
    }

    DLOG_INFO("Overrides: shader='%ls' (src %d), vfx='%ls' (src %d), tag='%ls'",
              m_activeOverride.name.c_str(), (int)shaderSrc,
              m_resolvedVFXProfile.c_str(), (int)m_resolvedVFXSource,
              matchedTag.c_str());

    // An empty name here is "explicitly none", which is a decision to apply
    // nothing rather than a profile to apply.
    if (!m_resolvedVFXProfile.empty())
        PushScopedVFXProfile(m_resolvedVFXProfile);
}

//----------------------------------------------------------------------
// ApplyOverrideToCurrentPreset / RevertOverrideOnCurrentPreset — ad hoc
//----------------------------------------------------------------------

bool Engine::ApplyOverrideToCurrentPreset(const std::wstring& name)
{
    const ShaderOverride* o = ShaderOverrides().Find(name);
    if (!o) return false;
    if (m_bShadertoyMode) return false;   // no warp/comp pass to override

    m_activeOverride.Clear();
    m_activeOverride.name     = o->name;
    m_activeOverride.warpText = o->warpText;
    m_activeOverride.compText = o->compText;
    m_activeOverride.fromRule = false;
    RequestShaderRecompile();
    return true;
}

void Engine::RevertOverrideOnCurrentPreset()
{
    m_activeOverride.Clear();
    RequestShaderRecompile();
}

//----------------------------------------------------------------------
// RequestShaderRecompile — rebuild the running preset's shaders
//----------------------------------------------------------------------

void Engine::RequestShaderRecompile()
{
    EnqueueRenderCmd(RenderCmd::ApplyShaderOverride);
}

//----------------------------------------------------------------------
// RebuildAnnotationHashIndex — hash -> map key
//----------------------------------------------------------------------

void Engine::RebuildAnnotationHashIndex()
{
    m_annotationsByHash.clear();
    for (auto& [key, a] : m_presetAnnotations)
        if (!a.hash.empty())
            m_annotationsByHash[a.hash] = key;
}

//----------------------------------------------------------------------
// GetAnnotationByHash — lookup by content identity
//----------------------------------------------------------------------

PresetAnnotation* Engine::GetAnnotationByHash(const std::wstring& hash)
{
    if (hash.empty()) return nullptr;
    auto it = m_annotationsByHash.find(hash);
    if (it == m_annotationsByHash.end()) return nullptr;
    auto ann = m_presetAnnotations.find(it->second);
    return (ann == m_presetAnnotations.end()) ? nullptr : &ann->second;
}

//----------------------------------------------------------------------
// AverageRating — mean of the observations, rounded; 0 when there are none
//----------------------------------------------------------------------

int Engine::AverageRating(const PresetAnnotation& a)
{
    if (a.ratings.empty()) return 0;
    int sum = 0;
    for (const auto& r : a.ratings) sum += r.value;
    return (int)((sum / (double)a.ratings.size()) + 0.5);
}

//----------------------------------------------------------------------
// MergeAnnotations — fold "from" into "into" without discarding anything
//----------------------------------------------------------------------

void Engine::MergeAnnotations(PresetAnnotation& into, const PresetAnnotation& from)
{
    into.flags |= from.flags;

    for (const auto& t : from.tags) {
        bool have = false;
        for (const auto& existing : into.tags)
            if (existing == t) { have = true; break; }
        if (!have) into.tags.push_back(t);
    }

    for (const auto& p : from.paths) {
        bool have = false;
        for (const auto& existing : into.paths)
            if (_wcsicmp(existing.c_str(), p.c_str()) == 0) { have = true; break; }
        if (!have && into.paths.size() < kMaxAnnotationPaths) into.paths.push_back(p);
    }

    // One observation per content version: a hash already present keeps the
    // rating it has rather than gaining a second entry for the same version.
    for (const auto& r : from.ratings) {
        bool have = false;
        for (const auto& existing : into.ratings)
            if (existing.hash == r.hash) { have = true; break; }
        if (!have) into.ratings.push_back(r);
    }

    // Adopt a per-preset override only where this entry has none, matching how
    // notes fold in: an import adds what is missing, it does not overwrite a
    // local choice.
    if (!into.hasShaderOverride && from.hasShaderOverride) {
        into.hasShaderOverride = true;
        into.shaderOverride = from.shaderOverride;
    }
    if (!into.hasVfxProfile && from.hasVfxProfile) {
        into.hasVfxProfile = true;
        into.vfxProfile = from.vfxProfile;
    }

    if (into.notes.empty())     into.notes = from.notes;
    if (into.errorText.empty()) into.errorText = from.errorText;

    into.useCount     += from.useCount;
    into.secondsShown += from.secondsShown;
    if (from.lastUsed > into.lastUsed) into.lastUsed = from.lastUsed;

    into.rating = AverageRating(into);
}

//----------------------------------------------------------------------
// ResolveAnnotation — hash first, filename second, re-stamp on a miss
//
// The two keys cover each other's failure modes.  The hash survives the preset
// being moved, copied or renamed; the filename survives it being edited, since
// an edit moves the hash.  A filename hit whose hash disagrees is the "edited
// in place" case, and re-stamping it there is what keeps rating history, tags
// and play counts attached across an edit.
//----------------------------------------------------------------------

PresetAnnotation* Engine::ResolveAnnotation(const wchar_t* filename, const wchar_t* hash,
                                            const wchar_t* fullPath, bool create)
{
    const std::wstring key = BareFilename(filename);
    if (key.empty()) return nullptr;
    const std::wstring wHash = hash ? hash : L"";

    PresetAnnotation* a = GetAnnotationByHash(wHash);

    if (!a) {
        auto it = m_presetAnnotations.find(key);
        if (it != m_presetAnnotations.end()) {
            a = &it->second;
            if (!wHash.empty() && a->hash.empty()) {
                // First time this entry has been seen with real content.  Bind
                // any observation migrated from a bare "rating" field to it, so
                // re-rating unchanged content updates that observation rather
                // than appending a second one for the same version.
                a->hash = wHash;
                m_annotationsByHash[wHash] = a->filename;
                AdoptHashIntoLegacyRatings(*a, wHash);
                m_bAnnotationsDirty = true;
            }
            else if (!wHash.empty() && a->hash != wHash) {
                // Same name, different content: the file was edited.  If some
                // other entry already owns this content, that entry is the same
                // preset in another place -- fold this one into it.
                PresetAnnotation* owner = GetAnnotationByHash(wHash);
                if (owner && owner != a) {
                    MergeAnnotations(*owner, *a);
                    m_presetAnnotations.erase(it);
                    RebuildAnnotationHashIndex();
                    m_bAnnotationsDirty = true;
                    DLOG_INFO("ResolveAnnotation: merged a duplicate entry into the one owning this content hash");
                    a = owner;
                } else {
                    if (!a->hash.empty()) m_annotationsByHash.erase(a->hash);
                    a->hash = wHash;
                    m_annotationsByHash[wHash] = a->filename;
                    m_bAnnotationsDirty = true;
                }
            }
        }
    }

    if (!a) {
        if (!create) return nullptr;
        PresetAnnotation fresh;
        fresh.filename = key;
        fresh.hash = wHash;
        auto [iter, ok] = m_presetAnnotations.emplace(key, std::move(fresh));
        a = &iter->second;
        if (!wHash.empty()) m_annotationsByHash[wHash] = a->filename;
        m_bAnnotationsDirty = true;
    }

    // Record where this copy lives.  Capped, because a preset that turns up in
    // hundreds of scanned folders should not grow presets.json without bound.
    if (fullPath && fullPath[0] && a->paths.size() < kMaxAnnotationPaths) {
        bool have = false;
        for (const auto& p : a->paths)
            if (_wcsicmp(p.c_str(), fullPath) == 0) { have = true; break; }
        if (!have) {
            a->paths.push_back(fullPath);
            m_bAnnotationsDirty = true;
        }
    }

    return a;
}

//----------------------------------------------------------------------
// AdoptHashIntoLegacyRatings — bind observations that have no hash
//----------------------------------------------------------------------

void Engine::AdoptHashIntoLegacyRatings(PresetAnnotation& a, const std::wstring& hash)
{
    if (hash.empty()) return;

    bool haveExplicit = false;
    for (const auto& r : a.ratings)
        if (r.hash == hash) { haveExplicit = true; break; }

    for (auto it = a.ratings.begin(); it != a.ratings.end(); ) {
        if (!it->hash.empty()) { ++it; continue; }
        if (haveExplicit) {
            it = a.ratings.erase(it);   // superseded by the explicit observation
        } else {
            it->hash = hash;
            haveExplicit = true;
            ++it;
        }
    }
}

//----------------------------------------------------------------------
// EffectiveRating — MDX12 rating wins; the file's fRating is the fallback
//----------------------------------------------------------------------

int Engine::EffectiveRating(const wchar_t* filename, float fFileRating) const
{
    const std::wstring key = BareFilename(filename);
    if (!key.empty()) {
        auto it = m_presetAnnotations.find(key);
        if (it != m_presetAnnotations.end() && !it->second.ratings.empty())
            return AverageRating(it->second);
    }
    if (fFileRating <= 0.f) return 0;
    return (int)(fFileRating + 0.5f);
}

//----------------------------------------------------------------------
// SetPresetRatingMDX — record the rating for the CURRENT content version
//----------------------------------------------------------------------

void Engine::SetPresetRatingMDX(const wchar_t* filename, int value)
{
    if (!filename || !filename[0]) return;
    if (value < 0) value = 0;
    if (value > 5) value = 5;

    const char* h = (m_pState ? m_pState->m_szPresetHash : "");
    std::wstring wHash(h, h + strlen(h));

    PresetAnnotation* a = ResolveAnnotation(filename, wHash.c_str(), nullptr, true);
    if (!a) return;

    SYSTEMTIME st; GetLocalTime(&st);
    wchar_t when[32];
    swprintf_s(when, L"%04d-%02d-%02dT%02d:%02d:%02d",
               st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);

    // One observation per content version: re-rating unchanged content updates
    // the existing entry rather than appending, so a preset rated ten times
    // cannot outvote one rated once.
    bool updated = false;
    for (auto& r : a->ratings) {
        if (r.hash == wHash) { r.value = value; r.when = when; updated = true; break; }
    }
    if (!updated) {
        RatingObservation obs;
        obs.hash = wHash; obs.value = value; obs.when = when;
        a->ratings.push_back(obs);
    }

    a->rating = AverageRating(*a);
    m_bAnnotationsDirty = true;
    SavePresetAnnotations();
}

//----------------------------------------------------------------------
// SetPresetRatingForFile — rate a preset that may not be the running one
//
// Uses the running preset's hash when the filename matches it, and the entry's
// stored hash otherwise.  An entry with no hash yet records the observation
// with an empty one; AdoptHashIntoLegacyRatings binds it as soon as the
// preset is next loaded.
//----------------------------------------------------------------------

void Engine::SetPresetRatingForFile(const wchar_t* filename, int value)
{
    if (!filename || !filename[0]) return;

    if (m_szCurrentPresetFile[0] &&
        BareFilename(filename) == BareFilename(m_szCurrentPresetFile)) {
        SetPresetRatingMDX(filename, value);
        return;
    }

    if (value < 0) value = 0;
    if (value > 5) value = 5;

    PresetAnnotation* a = GetAnnotation(filename, true);
    if (!a) return;

    SYSTEMTIME st; GetLocalTime(&st);
    wchar_t when[32];
    swprintf_s(when, L"%04d-%02d-%02dT%02d:%02d:%02d",
               st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);

    bool updated = false;
    for (auto& r : a->ratings)
        if (r.hash == a->hash) { r.value = value; r.when = when; updated = true; break; }
    if (!updated) {
        RatingObservation obs;
        obs.hash = a->hash; obs.value = value; obs.when = when;
        a->ratings.push_back(obs);
    }

    a->rating = AverageRating(*a);
    m_bAnnotationsDirty = true;
    SavePresetAnnotations();
}

//----------------------------------------------------------------------
// BeginPresetUsage — start timing a newly loaded preset
//----------------------------------------------------------------------

void Engine::BeginPresetUsage(const wchar_t* fullPath)
{
    FlushPresetUsage();   // bank whatever the previous preset earned
    lstrcpynW(m_szUsagePresetFile, fullPath ? fullPath : L"", 512);
    m_fPresetUsageStart = GetTime();
    m_bPresetUsageCounted = false;
}

//----------------------------------------------------------------------
// TickPresetUsage — count the play once the threshold is crossed
//
// The annotation entry is CREATED here, even for a preset with no tags, rating
// or notes; otherwise usage could not be recorded for untagged presets, which
// is most of them.  Presets dismissed inside the threshold create no entry, so
// presets.json grows by one entry per preset actually watched rather than one
// per preset glanced at.
//----------------------------------------------------------------------

void Engine::TickPresetUsage()
{
    if (m_bPresetUsageCounted) return;
    if (!m_szUsagePresetFile[0]) return;
    if (m_fPresetUsageStart <= 0) return;
    if (GetTime() - m_fPresetUsageStart < kUsageCountThresholdSec) return;

    const char* h = (m_pState ? m_pState->m_szPresetHash : "");
    std::wstring wHash(h, h + strlen(h));

    PresetAnnotation* a = ResolveAnnotation(m_szUsagePresetFile, wHash.c_str(),
                                            m_szUsagePresetFile, true);
    m_bPresetUsageCounted = true;   // set regardless, so a failure is not retried every frame
    if (!a) return;

    SYSTEMTIME st; GetLocalTime(&st);
    wchar_t when[32];
    swprintf_s(when, L"%04d-%02d-%02dT%02d:%02d:%02d",
               st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);

    a->useCount++;
    a->lastUsed = when;
    m_bAnnotationsDirty = true;
    SavePresetAnnotations();
}

//----------------------------------------------------------------------
// FlushPresetUsage — bank the seconds a counted preset was on screen
//----------------------------------------------------------------------

void Engine::FlushPresetUsage()
{
    if (!m_bPresetUsageCounted || !m_szUsagePresetFile[0]) {
        m_szUsagePresetFile[0] = 0;
        m_fPresetUsageStart = 0;
        m_bPresetUsageCounted = false;
        return;
    }

    const int seconds = (int)(GetTime() - m_fPresetUsageStart);
    if (seconds > 0) {
        PresetAnnotation* a = GetAnnotation(m_szUsagePresetFile, false);
        if (a) {
            a->secondsShown += seconds;
            m_bAnnotationsDirty = true;
            SavePresetAnnotations();
        }
    }

    m_szUsagePresetFile[0] = 0;
    m_fPresetUsageStart = 0;
    m_bPresetUsageCounted = false;
}

//----------------------------------------------------------------------
// ResetUsageStats — clear play data for one preset, or for all of them
//----------------------------------------------------------------------

void Engine::ResetUsageStats(const wchar_t* filenameOrNull)
{
    auto clear = [](PresetAnnotation& a) {
        a.lastUsed.clear(); a.useCount = 0; a.secondsShown = 0;
    };

    const std::wstring key = BareFilename(filenameOrNull);
    if (!key.empty()) {
        auto it = m_presetAnnotations.find(key);
        if (it == m_presetAnnotations.end()) return;
        clear(it->second);
    } else {
        for (auto& [key, a] : m_presetAnnotations) clear(a);
    }

    m_bAnnotationsDirty = true;
    SavePresetAnnotations();
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
        if (item.has(L"shaderOverride")) {
            a.hasShaderOverride = true;
            a.shaderOverride = item[L"shaderOverride"].asString();
        }
        if (item.has(L"vfxProfile")) {
            a.hasVfxProfile = true;
            a.vfxProfile = item[L"vfxProfile"].asString();
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
// SetPresetShaderOverride / SetPresetVFXProfile — the per-preset slots
//
// `present` is the whole point. false REMOVES the member, so the preset goes
// back to inheriting from its tags. true with an empty name means "explicitly
// none", which suppresses whatever the tags would have selected. Collapsing
// those two into one state is the mistake this signature exists to prevent.
//----------------------------------------------------------------------

void Engine::SetPresetShaderOverride(const wchar_t* filename,
                                     const std::wstring& name, bool present)
{
    PresetAnnotation* a = GetAnnotation(filename, true);
    if (!a) return;
    a->hasShaderOverride = present;
    a->shaderOverride = present ? name : std::wstring();
    m_bAnnotationsDirty = true;
    SavePresetAnnotations();
}

void Engine::SetPresetVFXProfile(const wchar_t* filename,
                                 const std::wstring& name, bool present)
{
    PresetAnnotation* a = GetAnnotation(filename, true);
    if (!a) return;
    a->hasVfxProfile = present;
    a->vfxProfile = present ? name : std::wstring();
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
                // Ensure trailing backslash (with bounds check)
                if (baseDirLen > 0 && baseDirLen < MAX_PATH - 1 && savedBaseDir[baseDirLen-1] != L'\\') {
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
            // Ensure trailing backslash (with bounds check)
            if (baseDirLen > 0 && baseDirLen < MAX_PATH - 1 && savedBaseDir[baseDirLen-1] != L'\\') {
                savedBaseDir[baseDirLen] = L'\\';
                savedBaseDir[baseDirLen+1] = 0;
                baseDirLen++;
            }
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
