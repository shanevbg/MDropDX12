// vfx_profile_store.cpp — see vfx_profile_store.h.
//
// Moved here wholesale from engine_spout_input.cpp, where it had ended up
// because the video effects rendering code already lived there. The behaviour
// and the on-disk format are unchanged; the file format is the compatibility
// surface and moving code is no reason to disturb it.

#include "vfx_profile_store.h"

#include <Windows.h>
#include <cstdio>

#include "json_utils.h"
#include "utility.h"
#include "config_store.h"

namespace mdrop {

namespace {

// Mutable member lookup. JsonValue::operator[] is const and returns a shared
// null for a missing key, which is the right shape for reading and no use at
// all for building.
JsonValue& MemberRef(JsonValue& obj, const wchar_t* key)
{
    obj.type = JsonValue::Object;
    for (auto& kv : obj.members)
        if (_wcsicmp(kv.first.c_str(), key) == 0) return kv.second;
    obj.members.emplace_back(key, JsonValue());
    obj.members.back().second.type = JsonValue::Object;
    return obj.members.back().second;
}

bool RemoveMember(JsonValue& obj, const wchar_t* key)
{
    for (size_t i = 0; i < obj.members.size(); i++)
        if (_wcsicmp(obj.members[i].first.c_str(), key) == 0) {
            obj.members.erase(obj.members.begin() + i);
            return true;
        }
    return false;
}

// A profile name becomes a JSON key, so it only has to be non-empty and free
// of the control characters the writer would have to escape. Path separators
// stopped meaning anything -- there is no file per profile to name any more.
bool IsUsableProfileName(const wchar_t* name)
{
    if (!name || !name[0]) return false;
    for (const wchar_t* p = name; *p; p++)
        if (*p < 0x20) return false;
    return true;
}

// name, default -- as handed in by SetTunables.
typedef std::vector<std::pair<std::wstring, int>> TunableList;

// How many tunables this build can carry in a VFXProfileData.
int TunableSlots(const TunableList& tun)
{
    const int n = (int)tun.size();
    return n < VFXProfileData::kMaxTunables ? n : VFXProfileData::kMaxTunables;
}

// Serialise a parameter set by writing it and reading it straight back. The
// writer stays the single description of the schema; hand-building a parallel
// DOM would be a second one to keep in step with it.
JsonValue VideoFXParamsToJson(const VFXProfileData& d, const TunableList& tun)
{
    JsonWriter w;
    w.BeginObject();

    w.BeginObject(L"transform");
    w.Float(L"posX", d.fx.posX);
    w.Float(L"posY", d.fx.posY);
    w.Float(L"scale", d.fx.scale);
    w.Float(L"rotation", d.fx.rotation);
    w.Bool(L"mirrorH", d.fx.mirrorH);
    w.Bool(L"mirrorV", d.fx.mirrorV);
    w.EndObject();

    w.BeginObject(L"color");
    w.Float(L"tintR", d.fx.tintR);
    w.Float(L"tintG", d.fx.tintG);
    w.Float(L"tintB", d.fx.tintB);
    w.Float(L"brightness", d.fx.brightness);
    w.Float(L"contrast", d.fx.contrast);
    w.Float(L"saturation", d.fx.saturation);
    w.Float(L"hueShift", d.fx.hueShift);
    w.Bool(L"invert", d.fx.invert);
    w.EndObject();

    // Render tunables travel with the profile: the same look often needs the
    // same glow and rib widths, and the profile's Save button is the only
    // place they are saved from.
    w.BeginObject(L"rendering");
    for (int i = 0; i < TunableSlots(tun); i++)
        w.Int(tun[i].first.c_str(), d.tunables[i]);
    w.EndObject();

    w.BeginObject(L"effects");
    w.Float(L"pixelation", d.fx.pixelation);
    w.Float(L"chromatic", d.fx.chromatic);
    w.Bool(L"edgeDetect", d.fx.edgeDetect);
    w.EndObject();

    w.Int(L"blendMode", d.fx.blendMode);

    auto writeAudioLink = [&](const wchar_t* key, const AudioLink& ar) {
        w.BeginObject(key);
        w.Int(L"source", ar.source);
        w.Float(L"intensity", ar.intensity);
        w.EndObject();
    };
    w.BeginObject(L"audio");
    writeAudioLink(L"posX",       d.fx.arPosX);
    writeAudioLink(L"posY",       d.fx.arPosY);
    writeAudioLink(L"scale",      d.fx.arScale);
    writeAudioLink(L"rotation",   d.fx.arRotation);
    writeAudioLink(L"brightness", d.fx.arBrightness);
    writeAudioLink(L"saturation", d.fx.arSaturation);
    writeAudioLink(L"chromatic",  d.fx.arChromatic);
    w.EndObject();

    w.EndObject();
    return JsonParse(w.ToString());
}

// Absent section leaves the seeded values alone rather than snapping them to
// defaults -- see the contract on VFXProfileStore::Load.
void VideoFXParamsFromJson(VFXProfileData& d, const JsonValue& profile,
                           const TunableList& tun)
{
    auto& t = profile[L"transform"];
    if (!t.isNull()) {
        d.fx.posX     = t[L"posX"].asFloat(0);
        d.fx.posY     = t[L"posY"].asFloat(0);
        d.fx.scale    = t[L"scale"].asFloat(1.0f);
        d.fx.rotation = t[L"rotation"].asFloat(0);
        d.fx.mirrorH  = t[L"mirrorH"].asBool(false);
        d.fx.mirrorV  = t[L"mirrorV"].asBool(false);
    }

    auto& c = profile[L"color"];
    if (!c.isNull()) {
        d.fx.tintR      = c[L"tintR"].asFloat(1);
        d.fx.tintG      = c[L"tintG"].asFloat(1);
        d.fx.tintB      = c[L"tintB"].asFloat(1);
        d.fx.brightness = c[L"brightness"].asFloat(0);
        d.fx.contrast   = c[L"contrast"].asFloat(1.0f);
        d.fx.saturation = c[L"saturation"].asFloat(1.0f);
        d.fx.hueShift   = c[L"hueShift"].asFloat(0);
        d.fx.invert     = c[L"invert"].asBool(false);
    }

    auto& e = profile[L"effects"];
    if (!e.isNull()) {
        d.fx.pixelation = e[L"pixelation"].asFloat(0);
        d.fx.chromatic  = e[L"chromatic"].asFloat(0);
        d.fx.edgeDetect = e[L"edgeDetect"].asBool(false);
    }

    // A profile written before this section existed leaves the current tuning
    // alone. The caller applies only what is flagged present, because applying
    // a tunable also writes its INI key.
    for (int i = 0; i < VFXProfileData::kMaxTunables; i++)
        d.tunablePresent[i] = false;

    auto& rr = profile[L"rendering"];
    if (!rr.isNull()) {
        for (int i = 0; i < TunableSlots(tun); i++) {
            const JsonValue& v = rr[tun[i].first.c_str()];
            if (!v.isNull()) {
                // Not clamped here: ApplyRenderTunable is the single
                // authority on a tunable's range, and the caller goes
                // through it.
                d.tunables[i] = v.asInt(tun[i].second);
                d.tunablePresent[i] = true;
            }
        }
    }

    // The one parameter that is not leave-alone: absent has always meant 0.
    d.fx.blendMode = profile[L"blendMode"].asInt(0);

    auto readAudioLink = [](const JsonValue& obj, AudioLink& ar) {
        if (!obj.isNull()) {
            ar.source    = obj[L"source"].asInt(0);
            ar.intensity = obj[L"intensity"].asFloat(0.5f);
        }
    };
    auto& a = profile[L"audio"];
    if (!a.isNull()) {
        readAudioLink(a[L"posX"],       d.fx.arPosX);
        readAudioLink(a[L"posY"],       d.fx.arPosY);
        readAudioLink(a[L"scale"],      d.fx.arScale);
        readAudioLink(a[L"rotation"],   d.fx.arRotation);
        readAudioLink(a[L"brightness"], d.fx.arBrightness);
        readAudioLink(a[L"saturation"], d.fx.arSaturation);
        readAudioLink(a[L"chromatic"],  d.fx.arChromatic);
    }
}

}  // namespace

// ---------------------------------------------------------------------------
// Store read / write
// ---------------------------------------------------------------------------

void VFXProfileStore::SetResourceDir(const wchar_t* dir)
{
    m_resourceDir = dir ? dir : L"";
}

void VFXProfileStore::SetTunables(const VFXTunableSpec* specs, int count)
{
    m_tunables.clear();
    for (int i = 0; i < count; i++)
        m_tunables.emplace_back(specs[i].name ? specs[i].name : L"", specs[i].defValue);
}

void VFXProfileStore::GetStorePath(wchar_t* out, size_t len) const
{
    swprintf_s(out, len, L"%svfxprofiles.json", m_resourceDir.c_str());
}

namespace {

bool LoadProfileStore(const VFXProfileStore* store, JsonValue& root)
{
    wchar_t path[MAX_PATH];
    store->GetStorePath(path, MAX_PATH);
    root = JsonLoadFile(path);
    if (!root.isObject()) {
        root = JsonValue();
        root.type = JsonValue::Object;
        return false;          // absent or unreadable: an empty store
    }
    return true;
}

bool SaveProfileStore(const VFXProfileStore* store, const JsonValue& root)
{
    wchar_t path[MAX_PATH];
    store->GetStorePath(path, MAX_PATH);

    JsonWriter w;
    w.BeginObject();
    bool bWroteVersion = false;
    for (const auto& kv : root.members) {
        if (_wcsicmp(kv.first.c_str(), L"version") == 0) bWroteVersion = true;
        w.Value(kv.first.c_str(), kv.second);
    }
    if (!bWroteVersion) w.Int(L"version", 1);
    w.EndObject();

    if (!w.SaveToFile(path)) {
        DLOG_ERROR("VideoFX: could not write %S", path);
        return false;
    }
    return true;
}

}  // namespace

// ---------------------------------------------------------------------------
// Named profiles
// ---------------------------------------------------------------------------

void VFXProfileStore::Names(std::vector<std::wstring>& out) const
{
    out.clear();
    JsonValue root;
    LoadProfileStore(this, root);
    const JsonValue& profiles = root[L"profiles"];
    if (!profiles.isObject()) return;
    for (const auto& kv : profiles.members)
        out.push_back(kv.first);
}

bool VFXProfileStore::Exists(const wchar_t* name) const
{
    if (!IsUsableProfileName(name)) return false;
    JsonValue root;
    LoadProfileStore(this, root);
    return root[L"profiles"].has(name);
}

bool VFXProfileStore::Save(const wchar_t* name, const VFXProfileData& d)
{
    if (!IsUsableProfileName(name)) {
        DLOG_WARN("VideoFX: refusing to save a profile with an unusable name");
        return false;
    }

    JsonValue root;
    LoadProfileStore(this, root);          // absent file just means an empty store
    MemberRef(MemberRef(root, L"profiles"), name) = VideoFXParamsToJson(d, m_tunables);

    if (!SaveProfileStore(this, root)) return false;
    DLOG_INFO("VideoFX: saved profile '%S'", name);
    return true;
}

bool VFXProfileStore::Load(const wchar_t* name, VFXProfileData& inout) const
{
    if (!IsUsableProfileName(name)) return false;

    JsonValue root;
    if (!LoadProfileStore(this, root)) return false;
    const JsonValue& profile = root[L"profiles"][name];
    if (!profile.isObject()) {
        DLOG_WARN("VideoFX: no profile named '%S'", name);
        return false;
    }

    VideoFXParamsFromJson(inout, profile, m_tunables);
    DLOG_INFO("VideoFX: loaded profile '%S'", name);
    return true;
}

bool VFXProfileStore::Delete(const wchar_t* name)
{
    if (!IsUsableProfileName(name)) return false;

    JsonValue root;
    if (!LoadProfileStore(this, root)) return false;
    JsonValue& profiles = MemberRef(root, L"profiles");
    if (!RemoveMember(profiles, name)) return false;

    if (!SaveProfileStore(this, root)) return false;
    DLOG_INFO("VideoFX: deleted profile '%S'", name);
    return true;
}

// ---------------------------------------------------------------------------
// Importing from files this build did not write — see the header.
// ---------------------------------------------------------------------------

namespace {

// The parameter keys as the INI held them. Kept as its own list rather than
// shared with the live reader: this one must go on understanding the old names
// even if the live schema moves on, which is the entire point of it.
JsonValue VFXProfileFromIni(const wchar_t* path, const TunableList& tun)
{
    JsonValue none;

    // An INI with no [VideoFX] parameters -- a current-format settings.ini,
    // say, where only the startup keys remain -- has nothing to import. Test
    // for real parameter keys rather than the section, which still exists.
    static const wchar_t* kProbe[] = { L"PosX", L"Scale", L"TintR", L"BlendMode", L"Brightness" };
    bool bAny = false;
    wchar_t probe[64];
    for (const wchar_t* k : kProbe) {
        ConfigFile(path).GetStringTo(L"VideoFX", k, L"", probe, 64);
        if (probe[0]) { bAny = true; break; }
    }
    if (!bAny) return none;

    wchar_t buf[64];

    // [VideoFX] is shared. It is also the Video Effects TOOL WINDOW's INI
    // section, and SaveWindowPosition writes PosX / PosY there -- the same two
    // key names the parameters used. A settings.ini that has both therefore
    // holds a window coordinate where the position parameter should be, which
    // is how a profile ends up claiming posX = -1501. Presence of the window
    // keys is the tell; when they are there, the position is not trustworthy.
    ConfigFile(path).GetStringTo(L"VideoFX", L"WndW", L"", buf, 64);
    const bool bSectionSharedWithWindow = (buf[0] != 0);
    if (bSectionSharedWithWindow)
        DLOG_WARN("VideoFX import: [VideoFX] in %S also holds tool window geometry; "
                  "ignoring PosX/PosY from it", path);

    auto readF = [&](const wchar_t* key, float def) -> float {
        ConfigFile(path).GetStringTo(L"VideoFX", key, L"", buf, 64);
        return buf[0] ? (float)_wtof(buf) : def;
    };
    auto readB = [&](const wchar_t* key, bool def) -> bool {
        return ConfigFile(path).GetInt(L"VideoFX", key, def ? 1 : 0) != 0;
    };
    // Clamp to the ranges the sliders actually offer. An imported file can be
    // any age, hand-edited, or -- as above -- carrying a value that was never a
    // parameter at all; none of that should produce a profile the UI cannot
    // represent.
    auto clampF = [](float v, float lo, float hi) {
        return v < lo ? lo : (v > hi ? hi : v);
    };
    auto readClamped = [&](const wchar_t* key, float def, float lo, float hi) {
        return clampF(readF(key, def), lo, hi);
    };

    JsonWriter w;
    w.BeginObject();

    w.BeginObject(L"transform");
    w.Float(L"posX", bSectionSharedWithWindow ? 0.0f : readClamped(L"PosX", 0, -1.0f, 1.0f));
    w.Float(L"posY", bSectionSharedWithWindow ? 0.0f : readClamped(L"PosY", 0, -1.0f, 1.0f));
    w.Float(L"scale", readClamped(L"Scale", 1.0f, 0.1f, 5.0f));
    w.Float(L"rotation", readClamped(L"Rotation", 0, 0, 360.0f));
    w.Bool(L"mirrorH", readB(L"MirrorH", false));
    w.Bool(L"mirrorV", readB(L"MirrorV", false));
    w.EndObject();

    w.BeginObject(L"color");
    w.Float(L"tintR", readClamped(L"TintR", 1, 0, 2.0f));
    w.Float(L"tintG", readClamped(L"TintG", 1, 0, 2.0f));
    w.Float(L"tintB", readClamped(L"TintB", 1, 0, 2.0f));
    w.Float(L"brightness", readClamped(L"Brightness", 0, -1.0f, 1.0f));
    w.Float(L"contrast", readClamped(L"Contrast", 1.0f, 0, 3.0f));
    w.Float(L"saturation", readClamped(L"Saturation", 1.0f, 0, 3.0f));
    w.Float(L"hueShift", readClamped(L"HueShift", 0, 0, 360.0f));
    w.Bool(L"invert", readB(L"Invert", false));
    w.EndObject();

    // Tunables lived in [Settings], not [VideoFX]. Absent keys are left out
    // entirely so loading the profile leaves the current tuning alone.
    w.BeginObject(L"rendering");
    for (const auto& t : tun) {
        ConfigFile(path).GetStringTo(L"Settings", t.first.c_str(), L"", buf, 64);
        if (buf[0]) w.Int(t.first.c_str(), _wtoi(buf));
    }
    w.EndObject();

    w.BeginObject(L"effects");
    w.Float(L"pixelation", readClamped(L"Pixelation", 0, 0, 1.0f));
    w.Float(L"chromatic", readClamped(L"Chromatic", 0, 0, 0.05f));
    w.Bool(L"edgeDetect", readB(L"EdgeDetect", false));
    w.EndObject();

    int blend = ConfigFile(path).GetInt(L"VideoFX", L"BlendMode", 0);
    if (blend < 0 || blend > 5) blend = 0;
    w.Int(L"blendMode", blend);

    auto writeAR = [&](const wchar_t* key, const wchar_t* prefix) {
        wchar_t k[64];
        w.BeginObject(key);
        swprintf(k, 64, L"%s_Source", prefix);
        int src = ConfigFile(path).GetInt(L"VideoFX", k, 0);
        if (src < 0 || src > 4) src = 0;     // None / Bass / Mid / Treb / Vol
        w.Int(L"source", src);
        swprintf(k, 64, L"%s_Intensity", prefix);
        w.Float(L"intensity", clampF(readF(k, 0.5f), 0, 1.0f));
        w.EndObject();
    };
    w.BeginObject(L"audio");
    writeAR(L"posX",       L"AR_PosX");
    writeAR(L"posY",       L"AR_PosY");
    writeAR(L"scale",      L"AR_Scale");
    writeAR(L"rotation",   L"AR_Rotation");
    writeAR(L"brightness", L"AR_Brightness");
    writeAR(L"saturation", L"AR_Saturation");
    writeAR(L"chromatic",  L"AR_Chromatic");
    w.EndObject();

    w.EndObject();
    return JsonParse(w.ToString());
}

// Does this object carry effect parameters, as opposed to being a container?
bool LooksLikeVFXProfile(const JsonValue& v)
{
    if (!v.isObject()) return false;
    return v.has(L"transform") || v.has(L"color") || v.has(L"effects") ||
           v.has(L"audio")     || v.has(L"blendMode");
}

// A name not already taken, by adding " (2)", " (3)" ... Importing must never
// quietly replace a profile the user already had.
std::wstring UniqueProfileName(const JsonValue& profiles, const std::wstring& base)
{
    if (!profiles.has(base.c_str())) return base;
    for (int n = 2; n < 1000; n++) {
        wchar_t buf[MAX_VFX_PROFILE_NAME + 16];
        swprintf_s(buf, L"%s (%d)", base.c_str(), n);
        if (!profiles.has(buf)) return buf;
    }
    return base;
}

// Everything importable in one file, in store order. An entry with an empty
// name is a bare parameter set that the file did not name.
std::vector<std::pair<std::wstring, JsonValue>> CollectImportable(const wchar_t* path,
                                                                 const TunableList& tun)
{
    std::vector<std::pair<std::wstring, JsonValue>> out;

    // Content, not extension: a .txt holding JSON still imports, and a .json
    // that is really an INI does not silently produce an empty profile.
    JsonValue root = JsonLoadFile(path);
    if (root.isObject()) {
        // A store, optionally behind a "videoFX" section.
        const JsonValue* holder = &root;
        if (root[L"videoFX"].isObject()) holder = &root[L"videoFX"];

        const JsonValue& profiles = (*holder)[L"profiles"];
        if (profiles.isObject() && !profiles.members.empty()) {
            for (const auto& kv : profiles.members)
                if (LooksLikeVFXProfile(kv.second))
                    out.emplace_back(kv.first, kv.second);
            if (!out.empty()) return out;
        }

        // A single profile file: videofx/<name>.json or current.json.
        if (LooksLikeVFXProfile(*holder)) {
            out.emplace_back(std::wstring(), *holder);
            return out;
        }
        return out;   // JSON, but nothing recognisable in it
    }

    JsonValue fromIni = VFXProfileFromIni(path, tun);
    if (!fromIni.isNull())
        out.emplace_back(std::wstring(), fromIni);
    return out;
}

// "current" and "settings" describe the file's role, not the look it holds, so
// they make poor profile names. Anything else the user named is worth keeping.
std::wstring SuggestNameFromPath(const wchar_t* path)
{
    std::wstring stem(path);
    const size_t slash = stem.find_last_of(L"\\/");
    if (slash != std::wstring::npos) stem.erase(0, slash + 1);
    const size_t dot = stem.rfind(L'.');
    if (dot != std::wstring::npos) stem.erase(dot);

    if (stem.empty() ||
        _wcsicmp(stem.c_str(), L"current") == 0 ||
        _wcsicmp(stem.c_str(), L"settings") == 0 ||
        _wcsicmp(stem.c_str(), L"vfxprofiles") == 0)
        return L"Imported";
    return stem;
}

}  // namespace

int VFXProfileStore::PeekImport(const wchar_t* path, std::wstring& suggestedName) const
{
    if (GetFileAttributesW(path) == INVALID_FILE_ATTRIBUTES) return -1;

    const auto found = CollectImportable(path, m_tunables);
    suggestedName = (found.size() == 1 && found[0].first.empty())
                    ? SuggestNameFromPath(path)
                    : std::wstring();
    return (int)found.size();
}

int VFXProfileStore::Import(const wchar_t* path, const wchar_t* nameForSingle)
{
    if (GetFileAttributesW(path) == INVALID_FILE_ATTRIBUTES) return -1;

    auto found = CollectImportable(path, m_tunables);
    if (found.empty()) return 0;

    JsonValue root;
    LoadProfileStore(this, root);
    JsonValue& profiles = MemberRef(root, L"profiles");

    int nImported = 0;
    for (auto& entry : found) {
        std::wstring name;
        if (entry.first.empty()) {
            // Unnamed: the caller asked for it under a specific name and has
            // already confirmed replacing anything there.
            name = (nameForSingle && nameForSingle[0]) ? nameForSingle
                                                       : SuggestNameFromPath(path);
            if (name.size() >= MAX_VFX_PROFILE_NAME)
                name.resize(MAX_VFX_PROFILE_NAME - 1);
        } else {
            name = entry.first;
            if (name.size() >= MAX_VFX_PROFILE_NAME)
                name.resize(MAX_VFX_PROFILE_NAME - 1);
            name = UniqueProfileName(profiles, name);
        }
        MemberRef(profiles, name.c_str()) = entry.second;
        nImported++;
    }

    if (!SaveProfileStore(this, root)) return -1;
    DLOG_INFO("VideoFX: imported %d profile(s) from %S", nImported, path);
    return nImported;
}

void VFXProfileStore::MigrateFolder()
{
    {
        JsonValue check;
        if (LoadProfileStore(this, check) && check[L"importedFromVideoFXFolder"].asBool(false))
            return;
    }

    wchar_t dir[MAX_PATH];
    swprintf_s(dir, MAX_PATH, L"%svideofx\\", m_resourceDir.c_str());
    wchar_t pattern[MAX_PATH];
    swprintf_s(pattern, MAX_PATH, L"%s*.json", dir);

    WIN32_FIND_DATAW fd;
    HANDLE hFind = FindFirstFileW(pattern, &fd);
    if (hFind == INVALID_HANDLE_VALUE) return;

    JsonValue root;
    LoadProfileStore(this, root);
    JsonValue& profiles = MemberRef(root, L"profiles");

    int nImported = 0;
    do {
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
        if (_wcsicmp(fd.cFileName, L"current.json") == 0) continue;

        std::wstring name(fd.cFileName);
        const size_t dot = name.rfind(L'.');
        if (dot != std::wstring::npos) name.erase(dot);
        if (!IsUsableProfileName(name.c_str())) continue;
        if (profiles.has(name.c_str())) continue;   // never overwrite the store

        wchar_t full[MAX_PATH];
        swprintf_s(full, MAX_PATH, L"%s%s", dir, fd.cFileName);
        JsonValue profile = JsonLoadFile(full);
        if (!profile.isObject()) continue;

        MemberRef(profiles, name.c_str()) = profile;
        nImported++;
    } while (FindNextFileW(hFind, &fd));
    FindClose(hFind);

    // Record the sweep even when it imported nothing, so it does not run
    // again. Only reached when the folder existed -- with no folder there is
    // nothing to re-import, and writing a marker would create the store on a
    // startup where no profile was ever saved.
    JsonValue& done = MemberRef(root, L"importedFromVideoFXFolder");
    done.type = JsonValue::Bool;
    done.bVal = true;
    SaveProfileStore(this, root);
    if (nImported > 0)
        DLOG_INFO("VideoFX: imported %d profile(s) from videofx\\ into vfxprofiles.json", nImported);
}

}  // namespace mdrop
