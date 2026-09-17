/*
  Plugin module: Preset Annotation System
  presets.json — persistent per-preset metadata, hash-keyed identity.
  Contains: Ignore-dir/testing-mode write gates, presets.json load/save/
            parse, content-hash + path resolution (ResolveLocked,
            MergeAnnotations, RepairAnnotationPaths), duplicate scan,
            per-preset override slots (shader/audio/VFX/replacement),
            MDX ratings (AverageRating, EffectiveRating), usage stats
            (BeginPresetUsage, TickPresetUsage, FlushPresetUsage),
            flags/notes + auto shader-error capture, tags + MWR import,
            preset lists (EnumPresetLists, Save/LoadPresetList)
*/

#include "engine.h"
#include "pipe_server.h"   // g_pipeServer, for the browse/annotate IPC below
#include "preset_hash.h"
#include "shader_overrides.h"
#include "json_utils.h"
#include "format_to.h"
#include "utility.h"
#include <set>
#include <map>
#include <algorithm>
#include <shlwapi.h>
#pragma comment(lib, "shlwapi.lib")

namespace mdrop {

// Locations recorded per preset.  Capped so a preset that turns up in hundreds
// of scanned folders cannot grow presets.json without bound.
static const size_t kMaxAnnotationPaths = 32;

//----------------------------------------------------------------------
// Keeping test runs out of presets.json
//
// Two gates, both write-only.  Reads are never blocked: an entry that already
// exists still resolves while testing mode is on, because hiding real data
// during a test run would be a worse bug than the pollution being prevented.
//----------------------------------------------------------------------

void Engine::ParseAnnotIgnoreDirs(const wchar_t* semicolonList)
{
    m_annotIgnoreDirs.clear();
    if (!semicolonList || !*semicolonList) return;

    std::wstring cur;
    for (const wchar_t* p = semicolonList; ; ++p) {
        if (*p == L';' || *p == 0) {
            // Trim: "TEST ; scratch" must not produce a segment named " scratch".
            size_t b = cur.find_first_not_of(L" \t");
            size_t e = cur.find_last_not_of(L" \t");
            if (b != std::wstring::npos)
                m_annotIgnoreDirs.push_back(cur.substr(b, e - b + 1));
            cur.clear();
            if (*p == 0) break;
        } else {
            cur += *p;
        }
    }
}

// True when any SEGMENT of the path equals an ignored name.
//
// Segment equality, not substring: a substring test would swallow a real
// library folder called "TESTAMENT" or "Latest", and silently refusing to
// record those presets would look exactly like annotations being lost.
bool Engine::IsAnnotationIgnoredPath(const wchar_t* fullPath) const
{
    if (!fullPath || !*fullPath || m_annotIgnoreDirs.empty()) return false;

    const std::wstring path(fullPath);
    size_t start = 0;
    while (start <= path.size()) {
        size_t cut = path.find_first_of(L"\\/", start);
        const size_t end = (cut == std::wstring::npos) ? path.size() : cut;
        if (end > start) {
            const std::wstring seg = path.substr(start, end - start);
            for (const std::wstring& ig : m_annotIgnoreDirs)
                if (_wcsicmp(seg.c_str(), ig.c_str()) == 0) return true;
        }
        if (cut == std::wstring::npos) break;
        start = cut + 1;
    }
    return false;
}

bool Engine::ShouldSkipAnnotationWrite(const wchar_t* fullPath) const
{
    if (m_bTestingMode) return true;
    return IsAnnotationIgnoredPath(fullPath);
}

//----------------------------------------------------------------------
// IsScratchPreset - a file that must not have an identity at all
//
// Testing mode plus an ignored directory (TEST/ and anything under it) means
// "this file is scratch". Scratch files get no annotation: no tags, no rating,
// and above all no per-preset OVERRIDES.
//
// Content-hash identity is what makes annotations follow a preset when it is
// moved or renamed, and that is precisely wrong for a copy made in order to
// measure the preset WITHOUT its overrides. A copy into TEST/ is byte-identical
// to the original and usually keeps its filename, so it matched on both the
// hash and the name and quietly inherited the original's canvasMax, shader
// override, VFX profile and audio profile -- the opposite of what copying it
// into a scratch folder was for. Neither route may bind.
//
// The existing gate, ShouldSkipAnnotationWrite, blocks WRITES only. This is its
// counterpart for reads, and it is deliberately narrower: it needs testing mode
// as well as the directory, so a preset that merely lives in a folder called
// TEST still behaves normally in ordinary use.
//----------------------------------------------------------------------

bool Engine::IsScratchPreset(const wchar_t* fullPath) const
{
    return m_bTestingMode && IsAnnotationIgnoredPath(fullPath);
}

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

// ISO 8601 local time, the shape the ratings and lastUsed fields already use.
static std::wstring IsoLocalNow() {
    SYSTEMTIME st; GetLocalTime(&st);
    wchar_t when[32];
    FormatTo(when, L"%04d-%02d-%02dT%02d:%02d:%02d",
             st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
    return when;
}

// Which producer wrote errorText.  Entries written before the field existed
// carry no "errorKind": everything AutoFlagPresetError recorded back then came
// from shader compilation except the SEH render-exception record, which names
// itself in its text, so the legacy split is exact rather than a guess.
static PresetErrorKind ErrorKindFrom(const JsonValue& item, const std::wstring& errorText) {
    if (item.has(L"errorKind"))
        return item[L"errorKind"].asString() == L"runtime" ? PresetErrorKind::Runtime
                                                           : PresetErrorKind::Shader;
    return errorText.compare(0, 4, L"SEH:") == 0 ? PresetErrorKind::Runtime
                                                 : PresetErrorKind::Shader;
}

// An entry keyed by a FULL PATH is unreachable.  Every lookup normalizes to the
// bare filename (BareFilename, above) while the map is keyed by the raw string
// out of the file, so nothing can match such a key again; with no hash either,
// it cannot be found the other way.  Older builds wrote these.
//
// Deleting user data on a heuristic is not acceptable, so the rule is not
// "looks dead" but "holds nothing a person put there": an auto-captured error
// and the flag that came with it are the entire content.  Anything else -- a
// note, a rating, a tag, a play count, an override, a second flag -- keeps the
// entry, dead key and all.
static bool IsDeadAutoErrorRecord(const PresetAnnotation& a) {
    const bool fullPath = (a.filename.size() > 1 && a.filename[1] == L':') ||
                          a.filename.compare(0, 2, L"\\\\") == 0;
    if (!fullPath) return false;
    if (!a.hash.empty() || !a.paths.empty()) return false;
    if (a.errorText.empty()) return false;
    if (a.flags != PFLAG_ERROR) return false;
    if (!a.notes.empty() || !a.tags.empty()) return false;
    if (a.rating != 0 || !a.ratings.empty()) return false;
    if (a.useCount != 0 || a.secondsShown != 0 || !a.lastUsed.empty()) return false;
    if (a.hasShaderOverride || a.hasVfxProfile || a.hasAudioProfile || a.hasCanvasMax ||
        a.hasFpsMax ||
        a.hasReplacementPreset)
        return false;
    return true;
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
        else if (s == L"canvas") flags |= PFLAG_CANVAS;
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
    if (flags & PFLAG_CANVAS)   emit(L"canvas");
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

    // Parsed into a LOCAL map and swapped in at the end, rather than clearing
    // the live store and filling it entry by entry (#11).
    //
    // This is the most destructive thing that happens to the store -- the
    // engine's own probe kills the process with 0xC0000409 by racing it against
    // a window refresh -- and building aside makes the critical section a move
    // and an index rebuild instead of the whole JSON parse. The store is also
    // never observable half-loaded, which the clear-then-fill version could not
    // promise even with a lock held throughout.
    std::unordered_map<std::wstring, PresetAnnotation> loaded;
    int nDropped = 0;
    for (size_t i = 0; i < arr.size(); i++) {
        const JsonValue& item = arr.at(i);
        PresetAnnotation a;
        a.filename  = item[L"filename"].asString();
        a.rating    = item[L"rating"].asInt(0);
        a.flags     = FlagsFromJson(item[L"flags"]);
        a.notes     = item[L"notes"].asString();
        a.errorText = item[L"errorText"].asString();
        a.errorTime = item[L"errorTime"].asString();
        a.errorKind = ErrorKindFrom(item, a.errorText);
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
        if (item.has(L"audioProfile")) {
            a.hasAudioProfile = true;
            a.audioProfile = item[L"audioProfile"].asString();
        }
        if (item.has(L"replacementPreset")) {
            a.hasReplacementPreset = true;
            a.replacementPreset = item[L"replacementPreset"].asString();
        }
        if (item.has(L"canvasMax")) {
            a.hasCanvasMax = true;
            a.canvasMax = item[L"canvasMax"].asInt();
        }
        if (item.has(L"feedbackDamp")) {
            a.hasFeedbackDamp = true;
            a.feedbackDamp = item[L"feedbackDamp"].asFloat();
        }
        if (item.has(L"fpsMax")) {
            a.hasFpsMax = true;
            a.fpsMax = item[L"fpsMax"].asInt();
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

        if (a.filename.empty()) continue;
        if (IsDeadAutoErrorRecord(a)) { nDropped++; continue; }
        loaded[a.filename] = std::move(a);
    }

    int nEntries = 0, nHashed = 0;
    m_annot.with([&](AnnotStore& s) {
        s.byKey = std::move(loaded);
        RebuildHashIndexLocked(s);
        s.dirty = false;
        nEntries = (int)s.byKey.size();
        nHashed  = (int)s.byHash.size();
    });
    DLOG_INFO("LoadPresetAnnotations: loaded %d entries (%d with a hash)",
              nEntries, nHashed);
    if (nDropped > 0) {
        // Written back now rather than waiting for the next annotation edit, so
        // the file stops carrying them even if nothing else changes this run.
        DLOG_WARN("LoadPresetAnnotations: dropped %d unreachable auto-error entries "
                  "(full-path key, no hash, nothing user-authored)", nDropped);
        m_annot.with([](AnnotStore& s) { s.dirty = true; });
        SavePresetAnnotations();
    }
}

//----------------------------------------------------------------------
// ResolveAnnotationPath — where does this preset actually live?
//----------------------------------------------------------------------

// Returns a full path that EXISTS on disk, or empty if the preset is gone.
//
// The Annotations window is meant to be one place to find every preset, so it
// cannot assume they all sit in the current preset directory: annotations
// outlive directory changes, and `paths` exists precisely to record where a
// preset was actually seen. Loading used to build m_szPresetDir + filename and
// nothing else, so any preset not in the directory currently browsed simply
// failed to load even though its real location was recorded.
//
// Three shapes have to be handled, all of which occur in real presets.json
// files: an entry whose filename is already a full path (legacy), an entry with
// recorded paths, and an entry with none.
std::wstring Engine::ResolveAnnotationPath(const PresetAnnotation& a) const
{
    // 1. The filename is itself a path (older entries were keyed this way).
    if (a.filename.find(L'\\') != std::wstring::npos ||
        a.filename.find(L'/') != std::wstring::npos) {
        if (PathFileExistsW(a.filename.c_str())) return a.filename;
    }

    // 2. Recorded locations, most recent first -- paths.back() is where it was
    //    last seen, so it is the likeliest to still be there.
    for (auto it = a.paths.rbegin(); it != a.paths.rend(); ++it)
        if (!it->empty() && PathFileExistsW(it->c_str())) return *it;

    // 3. The directory currently being browsed.
    if (m_szPresetDir[0]) {
        std::wstring bare = a.filename;
        const size_t cut = bare.find_last_of(L"\\/");
        if (cut != std::wstring::npos) bare = bare.substr(cut + 1);
        std::wstring guess = std::wstring(m_szPresetDir) + bare;
        if (PathFileExistsW(guess.c_str())) return guess;
    }

    return std::wstring();   // gone
}

// "Missing" means we KNOW where it was and it is not there any more.
//
// Deliberately NOT "ResolveAnnotationPath returned empty". Most entries in a
// real presets.json have no recorded `paths` at all -- 491 of 1633 in Shane's,
// 488 of them carrying ratings, notes or flags. Those fail to resolve simply
// because nothing was ever recorded and they are not in the directory being
// browsed, which is no evidence whatsoever that the file is gone. Treating them
// as missing would have deleted 299 entries' ratings on a library where exactly
// zero presets were actually absent.
//
// So an entry is only missing when it names locations and every one of them is
// dead. Absence of a path is not absence of the file.
bool Engine::IsAnnotationKnownMissing(const PresetAnnotation& a) const
{
    bool named = false;

    if (a.filename.find(L'\\') != std::wstring::npos ||
        a.filename.find(L'/') != std::wstring::npos)
        named = true;
    for (const std::wstring& pth : a.paths)
        if (!pth.empty()) { named = true; break; }

    if (!named) return false;                       // no evidence either way
    return ResolveAnnotationPath(a).empty();        // named, and none survive
}

//----------------------------------------------------------------------
// AnnotationCarriesUserValue - is there anything here that cannot be rebuilt?
//
// #192: a purge removed 105 entries carrying 638 plays and 21 hours of watch
// time, including a rating-4 preset with a canvasMax override and the note
// "Canvas-size feedback runaway -- the documented one". Not one of those files
// had been deleted. They had been MOVED, by a rehome that left every recorded
// path pointing at an abandoned directory -- which is exactly what a
// path-existence test reports as "gone".
//
// The lesson is not that the path test is wrong; it answers its own question
// honestly. It is that "no surviving path" is not sufficient grounds to
// DESTROY something. A path can be re-learned the next time the preset plays.
// A rating, a note, a play count and a shader error cannot be re-learned at
// all.
//
// So this decides what a purge may not touch. Counters included deliberately:
// useCount and secondsShown are measurements, and the issue's own table is the
// argument -- 2935 plays down to 2592, 309,344 seconds down to 302,305, gone
// in one write with nothing logged.
bool Engine::AnnotationCarriesUserValue(const PresetAnnotation& a)
{
    if (a.rating != 0)            return true;
    if (!a.ratings.empty())       return true;   // dated per-version observations
    if (a.flags != 0)             return true;   // favourite / error / skip / broken
    if (!a.notes.empty())         return true;
    if (!a.tags.empty())          return true;
    if (!a.errorText.empty())     return true;   // captured shader error
    if (a.useCount > 0)           return true;
    if (a.secondsShown > 0)       return true;
    if (!a.lastUsed.empty())      return true;
    // The override slots. Each is tri-state, so the has* flag is the question,
    // not whether the string is empty -- present-and-empty means "explicitly
    // none", which is itself a decision somebody made.
    if (a.hasShaderOverride)      return true;
    if (a.hasVfxProfile)          return true;
    if (a.hasAudioProfile)        return true;
    if (a.hasCanvasMax)           return true;
    if (a.hasFpsMax)              return true;
    if (a.hasFeedbackDamp)        return true;
    if (a.hasReplacementPreset)   return true;
    return false;
}

//----------------------------------------------------------------------
// ScanForDuplicatePresets — find files that ARE the same preset
//
// Grouped by content hash, never by filename, because the whole problem is
// copies that were renamed on the way: "Foo.milk", "Foo (2).milk" and
// "aa_Foo.milk" are one preset three times, and a name-based pass would call
// them three presets.  It is the same identity presets.json is keyed on, so a
// group here is exactly a group there.
//
// Deliberately NOT folded into the annotation store.  This reads every preset
// file under a root; running it implicitly -- on window open, on a timer --
// would stall the UI on a large library for no reason the user asked for.
//----------------------------------------------------------------------

static bool IsPresetFileName(const wchar_t* name)
{
    const wchar_t* dot = wcsrchr(name, L'.');
    if (!dot) return false;
    return _wcsicmp(dot, L".milk")  == 0 ||
           _wcsicmp(dot, L".milk2") == 0 ||
           _wcsicmp(dot, L".milk3") == 0;
}

// Depth-first, iterative.  A recursive walker is shorter, but preset libraries
// live on network shares and in sync folders where a symlink loop is a real
// possibility, and blowing the stack is a crash rather than a slow scan.
static bool CollectPresetFilesUnder(const std::wstring& root,
                                    std::vector<std::wstring>& out,
                                    int maxFiles,
                                    const Engine::DupeScanProgressFn& onProgress)
{
    std::vector<std::wstring> stack;
    stack.push_back(root);

    // Bounded independently of maxFiles: a loop that finds no preset files
    // would otherwise spin forever without ever tripping the file cap.
    int dirsVisited = 0;
    const int kMaxDirs = 20000;

    while (!stack.empty() && (int)out.size() < maxFiles && dirsVisited < kMaxDirs) {
        std::wstring dir = stack.back();
        stack.pop_back();
        dirsVisited++;

        if (!dir.empty() && dir.back() != L'\\' && dir.back() != L'/') dir += L'\\';

        WIN32_FIND_DATAW fd;
        const std::wstring mask = dir + L"*";
        HANDLE h = FindFirstFileW(mask.c_str(), &fd);
        if (h == INVALID_HANDLE_VALUE) continue;

        do {
            if (fd.cFileName[0] == L'.' &&
                (fd.cFileName[1] == 0 || (fd.cFileName[1] == L'.' && fd.cFileName[2] == 0)))
                continue;

            if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
                // Reparse points are where the loops come from -- a junction
                // pointing at an ancestor makes the walk infinite.  Skipping
                // them can miss presets that live only behind one, which is the
                // better failure: a missed copy is a copy not deleted.
                if (fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) continue;
                stack.push_back(dir + fd.cFileName);
            } else if (IsPresetFileName(fd.cFileName)) {
                out.push_back(dir + fd.cFileName);
            }
        } while (FindNextFileW(h, &fd) && (int)out.size() < maxFiles);

        FindClose(h);

        // Cancel is checked per DIRECTORY, not per file: the walk is cheap per
        // entry but a directory on a slow share is not, and this is the phase
        // that used to be uninterruptible -- Escape during it left the caller
        // blocked in the worker's join() until the whole tree had been read.
        // total = 0 marks the walking phase; out.size() is what has been found.
        if (onProgress && !onProgress((int)out.size(), 0, dir.c_str()))
            return false;
    }
    return true;
}

std::vector<Engine::DuplicateGroup>
Engine::ScanForDuplicatePresets(const wchar_t* root,
                                const DupeScanProgressFn& onProgress,
                                std::set<std::wstring>* outAllHashes) const
{
    std::vector<DuplicateGroup> groups;
    if (!root || !*root) return groups;

    // A cap rather than an unbounded walk: this runs against whatever folder
    // the user is browsing, and that is occasionally the root of a drive.
    const int kMaxFiles = 200000;
    std::vector<std::wstring> files;
    if (!CollectPresetFilesUnder(root, files, kMaxFiles, onProgress))
        return groups;      // cancelled while walking; report nothing

    // hash -> index into groups, so the first file of a hash creates the group
    // and later ones append in the order found.
    std::unordered_map<std::wstring, size_t> byHash;
    std::vector<DuplicateGroup> all;
    all.reserve(files.size() / 2 + 1);

    int seen = 0;
    for (const std::wstring& path : files) {
        seen++;
        // Hashing phase: total is now known, so the UI can show "n of m".
        if (onProgress && !onProgress(seen, (int)files.size(), path.c_str()))
            return std::vector<DuplicateGroup>();   // cancelled: a partial
                                                    // grouping would understate
                                                    // how many copies exist

        const std::string h = ComputePresetHashFile(path.c_str());
        if (h.empty()) continue;        // unreadable, or empty file
        const std::wstring wHash(h.begin(), h.end());
        if (outAllHashes) outAllHashes->insert(wHash);

        DuplicateFile df;
        df.path = path;
        WIN32_FILE_ATTRIBUTE_DATA fad = {};
        if (GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &fad)) {
            df.sizeBytes = ((uint64_t)fad.nFileSizeHigh << 32) | fad.nFileSizeLow;
            df.written   = fad.ftLastWriteTime;
        }

        auto it = byHash.find(wHash);
        if (it == byHash.end()) {
            DuplicateGroup g;
            g.hash = wHash;
            const size_t cut = path.find_last_of(L"\\/");
            g.displayName = (cut == std::wstring::npos) ? path : path.substr(cut + 1);
            g.files.push_back(std::move(df));
            byHash[wHash] = all.size();
            all.push_back(std::move(g));
        } else {
            all[it->second].files.push_back(std::move(df));
        }
    }

    for (DuplicateGroup& g : all)
        if (g.files.size() > 1) groups.push_back(std::move(g));

    // Worst offenders first: the point is to start deleting where it pays.
    // Name as the tiebreak so equal-sized groups do not shuffle between scans.
    std::sort(groups.begin(), groups.end(),
              [](const DuplicateGroup& a, const DuplicateGroup& b) {
                  if (a.files.size() != b.files.size())
                      return a.files.size() > b.files.size();
                  return _wcsicmp(a.displayName.c_str(), b.displayName.c_str()) < 0;
              });
    return groups;
}

void Engine::AdoptDuplicateScan(const std::vector<DuplicateGroup>& groups,
                                std::set<std::wstring>&& allHashes)
{
    m_dupeIndex.clear();
    for (const DuplicateGroup& g : groups) {
        std::vector<std::wstring> paths;
        paths.reserve(g.files.size());
        for (const DuplicateFile& f : g.files) paths.push_back(f.path);
        m_dupeIndex[g.hash] = std::move(paths);
    }
    m_dupeScannedHashes = std::move(allHashes);
    m_bDupeScanRun = true;
}

// How many files on disk carry this annotation's content.
//
// 0 means "unknown", not "none": before a scan has run there is nothing to
// report, and showing 1 would be a claim the app has not checked.
int Engine::DuplicateCountFor(const PresetAnnotation& a) const
{
    if (!m_bDupeScanRun || a.hash.empty()) return 0;
    auto it = m_dupeIndex.find(a.hash);
    if (it != m_dupeIndex.end()) return (int)it->second.size();
    // Seen by the scan and not duplicated: exactly one file. Never seen: the
    // preset lives outside the folder that was scanned, and the honest answer
    // is that we do not know.
    return m_dupeScannedHashes.count(a.hash) ? 1 : 0;
}

// Drop every annotation whose preset is KNOWN missing (see above).
// Returns how many were removed.
int Engine::RemoveMissingAnnotations()
{
    // Survey and erase in ONE lock hold. Two holds would let another thread
    // insert between them, and the second pass would erase a key the first
    // pass judged on different content.
    size_t spared = 0;
    std::vector<std::wstring> sparedNames;   // for the log, outside the lock
    const size_t removed = m_annot.with([&](AnnotStore& s) {
        std::vector<std::wstring> doomed;
        for (auto& kv : s.byKey) {
            if (!IsAnnotationKnownMissing(kv.second))
                continue;
            // A dead path is not grounds to destroy annotation that cannot be
            // rebuilt (#192). The file may be one rehome away, and the next
            // play re-learns the path by itself.
            if (AnnotationCarriesUserValue(kv.second)) {
                ++spared;
                if (sparedNames.size() < 12)
                    sparedNames.push_back(kv.second.filename);
                continue;
            }
            // Same content, different path, already known here: the preset is
            // demonstrably present under another entry, so this one is a stale
            // duplicate rather than a missing preset.
            if (!kv.second.hash.empty()) {
                auto it = s.byHash.find(kv.second.hash);
                if (it != s.byHash.end() && it->second != kv.first) {
                    ++spared;
                    continue;
                }
            }
            doomed.push_back(kv.first);
        }

        for (const std::wstring& key : doomed)
            s.byKey.erase(key);

        if (!doomed.empty()) {
            RebuildHashIndexLocked(s);
            s.dirty = true;
        }
        return doomed.size();
    });

    // SAY SO. The purge in #192 removed 105 entries and wrote not one line to
    // debug.log, which is why it was found by diffing a git commit six weeks
    // later instead of by anyone noticing at the time. A destructive operation
    // on user data that leaves no record is one nobody can audit.
    if (removed || spared) {
        DLOG_WARN("Annotations: purge removed %d entr%s, kept %d that carry "
                  "ratings/notes/history despite a dead path",
                  (int)removed, removed == 1 ? L"y" : L"ies", (int)spared);
        for (const std::wstring& n : sparedNames)
            DLOG_INFO("Annotations:   kept %ls", n.c_str());
    }

    // Outside the guard: SavePresetAnnotations writes a file.
    if (removed) SavePresetAnnotations();
    return (int)removed;
}

//----------------------------------------------------------------------
// AnnotationMatches — live search predicate for the Annotations list
//----------------------------------------------------------------------

static bool ContainsNoCase(const std::wstring& hay, const std::wstring& needle)
{
    if (needle.empty()) return true;
    if (needle.size() > hay.size()) return false;
    std::wstring h = hay, n = needle;
    std::transform(h.begin(), h.end(), h.begin(), ::towlower);
    std::transform(n.begin(), n.end(), n.begin(), ::towlower);
    return h.find(n) != std::wstring::npos;
}

// Substring by default, glob when the user types a wildcard.
//
// Substring matters for a live search: a whole-string match would show nothing
// until the name was almost fully typed, which is not "type a letter and it
// starts filtering". PathMatchSpecW is the platform's own glob (already
// case-insensitive), so * and ? behave the way they do everywhere else rather
// than however a hand-rolled matcher happened to.
bool Engine::AnnotationMatches(const PresetAnnotation& a, const wchar_t* query) const
{
    if (!query || !*query) return true;

    const std::wstring q(query);
    const bool glob = (q.find(L'*') != std::wstring::npos) ||
                      (q.find(L'?') != std::wstring::npos);

    if (glob) {
        if (PathMatchSpecW(a.filename.c_str(), q.c_str())) return true;
        return !a.notes.empty() && PathMatchSpecW(a.notes.c_str(), q.c_str());
    }

    // Notes are searched too, so a preset can be found by something written
    // about it rather than only by its filename.
    return ContainsNoCase(a.filename, q) || ContainsNoCase(a.notes, q);
}

//----------------------------------------------------------------------
// SavePresetAnnotations — write presets.json to disk
//----------------------------------------------------------------------

// Write a per-preset canvas limit by filename. 0 clears it: absent and zero
// are different states, so the flag is cleared rather than a 0 stored.
void Engine::SetPresetCanvasMaxByFile(const wchar_t* filenameOnly, int px)
{
    if (!filenameOnly || !*filenameOnly) return;
    if (px < 0) px = 0;

    // A scratch preset has no identity, so an entry written for one can never
    // be read back -- EffectiveFeedbackDamp and EffectiveCanvasLimit both
    // return early for it. Writing anyway just litters presets.json: a damp
    // measurement run left an entry for the ruler preset in
    // resources/presets/test/, which nothing could ever have used.
    //
    // This is NOT the testing-mode gate. An explicit setting still goes
    // through in testing mode -- the gates stop the app deciding on its own,
    // not the user. What is refused here is naming a file that is defined to
    // be anonymous.
    const wchar_t* curPath = CurrentPresetLeaf();
    if (_wcsicmp(curPath, filenameOnly) == 0 &&
        IsScratchPreset(m_szCurrentPresetFile))
        return;

    const bool changed = m_annot.with([&](AnnotStore& s) {
        PresetAnnotation* a = FindLocked(s, filenameOnly, /*create=*/true);
        if (!a) return false;
        a->canvasMax = px;
        a->hasCanvasMax = (px > 0);

        // SavePresetAnnotations early-outs unless this is set.
        s.dirty = true;
        return true;
    });
    // Outside the guard: SavePresetAnnotations writes a file.
    if (changed) SavePresetAnnotations();

    // Only rebuild when this is the preset actually on screen. Setting a limit
    // on some other preset in the browser must not resize the live canvas.
    const wchar_t* cur = CurrentPresetLeaf();
    if (_wcsicmp(cur, filenameOnly) == 0 &&
        EffectiveCanvasLimit() != m_nCanvasLimitApplied)
        EnqueueRenderCmd(RenderCmd::ReallocCanvas);
}

// The other half of the pair: the mitigation that leaves the canvas alone.
// Deliberately a separate setting rather than a mode switch -- Shane asked for
// both to be available and neither forced, because which one looks better is a
// judgement about a particular preset that this code cannot make. Strength is
// 0..1; 0 clears it. No canvas rebuild: the damp costs one quad per frame and
// takes effect on the very next one.
void Engine::SetPresetFeedbackDampByFile(const wchar_t* filenameOnly, float strength)
{
    if (!filenameOnly || !*filenameOnly) return;
    if (strength < 0.0f) strength = 0.0f;
    if (strength > 1.0f) strength = 1.0f;

    // A scratch preset has no identity, so an entry written for one can never
    // be read back -- EffectiveFeedbackDamp and EffectiveCanvasLimit both
    // return early for it. Writing anyway just litters presets.json: a damp
    // measurement run left an entry for the ruler preset in
    // resources/presets/test/, which nothing could ever have used.
    //
    // This is NOT the testing-mode gate. An explicit setting still goes
    // through in testing mode -- the gates stop the app deciding on its own,
    // not the user. What is refused here is naming a file that is defined to
    // be anonymous.
    const wchar_t* curPath = CurrentPresetLeaf();
    if (_wcsicmp(curPath, filenameOnly) == 0 &&
        IsScratchPreset(m_szCurrentPresetFile))
        return;

    const bool changed = m_annot.with([&](AnnotStore& s) {
        PresetAnnotation* a = FindLocked(s, filenameOnly, /*create=*/true);
        if (!a) return false;
        a->feedbackDamp = strength;
        a->hasFeedbackDamp = (strength > 0.0f);

        s.dirty = true;
        return true;
    });
    // Outside the guard: SavePresetAnnotations writes a file.
    if (changed) SavePresetAnnotations();

    // Only when this IS the preset on screen. Setting a damp on some other
    // preset in the browser must not change what is currently rendering --
    // the same rule SetPresetCanvasMaxByFile follows above.
    const wchar_t* cur = CurrentPresetLeaf();
    if (_wcsicmp(cur, filenameOnly) == 0)
        RefreshCurrentDampStrength();
}

void Engine::SavePresetAnnotations()
{
    // ONE saver at a time (#121), and the snapshot is taken INSIDE that lock.
    //
    // m_annotSaveMutex guards the FILE; m_annot guards the store. Two threads
    // each rewriting presets.json is how a database gets truncated, and the
    // dirty flag alone did not close it: it makes the common case
    // single-writer, but two threads that each mutate and then each save both
    // claim it and both write.
    //
    // The snapshot has to happen inside this lock, not before it. Taken
    // outside, thread A could snapshot, thread B snapshot a NEWER store, B win
    // the race to the disk, and A then overwrite it with the older copy. Inside,
    // whoever writes last also read last. It has a second benefit: the loser of
    // the race takes the lock, finds dirty already cleared, and writes nothing
    // at all rather than writing an identical file.
    //
    // LOCK ORDER: this mutex OUTSIDE m_annot, never the reverse.
    // SavePresetAnnotations is never called from inside an m_annot.with(),
    // because it writes a file -- see the guard's own note in engine.h.
    std::lock_guard<std::mutex> saveLock(m_annotSaveMutex);

    // Snapshot under the store guard, serialise and write outside it (#11).
    //
    // This is disk I/O, and TickPresetUsage calls it from the RENDER thread. A
    // ToolWindow thread parked on the STORE guard for the length of a file
    // write is a visible stall, so that lock covers the copy and nothing more
    // -- the same shape m_errors uses for its overlay draw (3319c3a).
    //
    // The copy is of the whole store, which is not free. It runs once per
    // preset actually watched past the usage threshold, not per frame.
    //
    // Write in a STABLE order, not byKey order.
    //
    // That container is an unordered_map, so its iteration order is
    // unspecified and, worse, changes wholesale whenever an insertion triggers
    // a rehash. Adding one preset therefore rewrote the position of every other
    // entry, turning a one-entry change into a diff across the whole file. That
    // is merely untidy for a program reading it back, and ruinous for the copy
    // kept under version control (shane/mdropdx12-config), where the point is
    // to be able to see what changed.
    //
    // Sorted by map key, which is unique, so the order is total and depends on
    // nothing but the content. Sorting the keys inside the lock and copying in
    // that order does both jobs in one pass.
    std::vector<PresetAnnotation> snapshot;
    const bool wasDirty = m_annot.with([&](AnnotStore& st) {
        if (!st.dirty) return false;
        std::vector<const std::wstring*> order;
        order.reserve(st.byKey.size());
        for (const auto& kv : st.byKey)
            order.push_back(&kv.first);
        std::sort(order.begin(), order.end(),
                  [](const std::wstring* l, const std::wstring* r) { return *l < *r; });
        snapshot.reserve(order.size());
        for (const std::wstring* pkey : order)
            snapshot.push_back(st.byKey[*pkey]);
        // Cleared here rather than after the write, so a mutation arriving
        // mid-write still marks the store dirty and earns its own save.
        st.dirty = false;
        return true;
    });
    if (!wasDirty) return;

    // Build JSON manually - JsonWriter lacks anonymous array element support
    std::wostringstream ss;
    ss << L"{\n  \"presets\": [\n";

    bool first = true;
    for (const PresetAnnotation& a : snapshot) {
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
        // Only alongside an actual error: emitting them for every entry would
        // bulk up the file to say nothing.
        if (!a.errorText.empty()) {
            ss << L"      \"errorTime\": \"" << JsonEscape(a.errorTime) << L"\",\n";
            ss << L"      \"errorKind\": \""
               << (a.errorKind == PresetErrorKind::Runtime ? L"runtime" : L"shader")
               << L"\",\n";
        }
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
        if (a.hasAudioProfile)
            ss << L"      \"audioProfile\": \"" << JsonEscape(a.audioProfile) << L"\",\n";
        if (a.hasReplacementPreset)
            ss << L"      \"replacementPreset\": \"" << JsonEscape(a.replacementPreset) << L"\",\n";
        // Emitted only when the flag is set, exactly like the slots above:
        // writing a bare 0 would turn every entry's "inherit" into an
        // explicit "no limit", which is a different state.
        if (a.hasCanvasMax)
            ss << L"      \"canvasMax\": " << a.canvasMax << L",\n";
        if (a.hasFpsMax && a.fpsMax > 0)
            ss << L"      \"fpsMax\": " << a.fpsMax << L",\n";
        if (a.hasFeedbackDamp)
            ss << L"      \"feedbackDamp\": " << a.feedbackDamp << L",\n";
        ss << L"      \"lastUsed\": \"" << JsonEscape(a.lastUsed) << L"\",\n";
        ss << L"      \"useCount\": " << a.useCount << L",\n";
        ss << L"      \"secondsShown\": " << a.secondsShown << L"\n";
        ss << L"    }";
    }

    ss << L"\n  ]\n}\n";

    wchar_t szPath[MAX_PATH];
    swprintf(szPath, MAX_PATH, L"%spresets.json", m_szBaseDir);
    if (!JsonSaveFile(szPath, ss.str())) {
        // The dirty flag was cleared by the snapshot, on the assumption the
        // write would happen. It did not, and JsonSaveFile leaves the file on
        // disk untouched -- so put the flag back and let the next edit, or
        // shutdown, try again. Without this the change is simply lost, quietly,
        // which is the failure mode this whole change exists to remove.
        m_annot.with([](AnnotStore& s) { s.dirty = true; });
        DLOG_WARN("SavePresetAnnotations: could not write %ls -- the database on "
                  "disk is unchanged and the edit is still pending", szPath);
    }
}

//----------------------------------------------------------------------
// GetAnnotation — lookup by filename, optionally create
//----------------------------------------------------------------------

PresetAnnotation* Engine::FindLocked(AnnotStore& s, const wchar_t* filename, bool create)
{
    const std::wstring key = BareFilename(filename);
    if (key.empty()) return nullptr;

    auto it = s.byKey.find(key);
    if (it != s.byKey.end())
        return &it->second;

    if (!create) return nullptr;

    PresetAnnotation a;
    a.filename = key;
    auto [iter, ok] = s.byKey.emplace(key, std::move(a));
    return &iter->second;
}


//----------------------------------------------------------------------
// WithAnnotation / WithAnnotationByHash / WithResolvedAnnotation
//
// The only way in from outside this file. The callback runs with the guard
// held; the PresetAnnotation& it receives is valid for exactly that long, and
// there is no way to keep it -- which is the whole of #11 in three functions.
//
// They return bool rather than void because "there is no entry" and "there is
// an entry with nothing set" are different answers, and at least two callers
// (EffectiveCanvasLimit, RefreshCurrentDampStrength) fall back from a hash
// lookup to a filename lookup on exactly that distinction.
//----------------------------------------------------------------------

bool Engine::WithAnnotation(const wchar_t* filename, bool create, const AnnotFn& fn)
{
    return m_annot.with([&](AnnotStore& s) {
        PresetAnnotation* a = FindLocked(s, filename, create);
        if (!a) return false;
        fn(*a);
        return true;
    });
}

bool Engine::WithAnnotationByHash(const std::wstring& hash, const AnnotConstFn& fn)
{
    return m_annot.with([&](AnnotStore& s) {
        PresetAnnotation* a = FindByHashLocked(s, hash);
        if (!a) return false;
        fn(*a);
        return true;
    });
}

bool Engine::WithResolvedAnnotation(const wchar_t* filename, const wchar_t* hash,
                                    const wchar_t* fullPath, bool create,
                                    const AnnotFn& fn)
{
    return m_annot.with([&](AnnotStore& s) {
        PresetAnnotation* a = ResolveLocked(s, filename, hash, fullPath, create);
        if (!a) return false;
        fn(*a);
        return true;
    });
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

    // Copied out: everything below resolves rules, compares strings and may
    // request a shader recompile. None of it belongs under the guard.
    //
    // NOT "|| a.tags.empty()": a preset may carry a per-preset override and no
    // tags at all, and that is the whole point of the per-preset entry.
    PresetAnnotation ann;
    bool have = m_annot.with([&](AnnotStore& s) {
        PresetAnnotation* e = FindByHashLocked(s, wHash);
        if (!e)
            e = FindLocked(s, m_szLoadingPreset[0] ? m_szLoadingPreset
                                                   : m_szCurrentPresetFile, false);
        if (!e) return false;
        ann = *e;
        return true;
    });
    if (!have) return;
    const PresetAnnotation* a = &ann;

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
// GetAnnotationForPreset — hash first, filename second
//
// Content identity is the primary key so a preset keeps its settings when it
// is moved or renamed; the filename is the fallback for an entry that has no
// hash yet. The point of having ONE of these is that a writer and a reader
// cannot pick different entries for the same preset.
//----------------------------------------------------------------------

PresetAnnotation* Engine::ForPresetLocked(AnnotStore& s, const wchar_t* filename,
                                          CState* pState, bool create)
{
    // Scratch files have no identity -- not by hash, not by name. See
    // IsScratchPreset.
    if (IsScratchPreset(filename)) return nullptr;

    if (pState) {
        const char* h = pState->m_szPresetHash;
        std::wstring wHash(h, h + strlen(h));
        if (PresetAnnotation* a = FindByHashLocked(s, wHash)) return a;
    }
    return FindLocked(s, filename, create);
}


//----------------------------------------------------------------------
// ResolveAudioProfileForPreset — which AudioProfile this preset is fed
//
// Deliberately NOT folded into ResolveShaderOverrideForPreset above. That
// function returns early when the custom-shader master switch is off, and that
// switch is documented as governing the shader and VFX slots. Audio is a
// different concern: someone who turned custom shaders off has not asked for
// their presets to be handed different audio.
//
// Runs on the preset-load thread, so it does NOT touch m_audioProfile. It
// names a profile and raises a flag; the render thread swaps it in at the top
// of the next frame, before anything reads it.
//----------------------------------------------------------------------

void Engine::ResolveAudioProfileForPreset(CState* pState)
{
    m_resolvedAudioProfile.clear();
    m_resolvedAudioSource = OverrideSource::None;

    PresetAnnotation ann;
    const bool a = m_annot.with([&](AnnotStore& s) {
        PresetAnnotation* e = ForPresetLocked(
            s, m_szLoadingPreset[0] ? m_szLoadingPreset : m_szCurrentPresetFile,
            pState, false);
        if (!e) return false;
        ann = *e;
        return true;
    });

    std::wstring name;
    if (a && ann.hasAudioProfile) {
        name = ann.audioProfile;             // may be empty: explicitly default
        m_resolvedAudioSource = OverrideSource::Preset;
    } else {
        const ShaderRule* rule = nullptr;
        if (a && !ann.tags.empty())
            rule = ShaderOverrides().ResolveRule(ann.tags, nullptr);
        if (rule && !rule->audioProfile.empty()) {
            name = rule->audioProfile;
            m_resolvedAudioSource = OverrideSource::Rule;
        } else {
            name = m_szDefaultAudioProfile;
            m_resolvedAudioSource = OverrideSource::None;   // global default
        }
    }

    // Empty at this point means "the default", whether that came from an
    // explicit none or from an unset global. There is always a live profile.
    if (name.empty()) name = L"MDropDX12";
    m_resolvedAudioProfile = name;

    CopyTo(m_szPendingAudioProfile, name.c_str());
    m_bAudioProfilePending.store(true, std::memory_order_release);

    DLOG_INFO("Audio profile: '%ls' (src %d, annot '%ls')", name.c_str(),
              (int)m_resolvedAudioSource, a ? ann.filename.c_str() : L"(none)");
}

//----------------------------------------------------------------------
// ApplyPendingAudioProfile — render thread; the only writer of m_audioProfile
//----------------------------------------------------------------------

void Engine::ApplyPendingAudioProfile()
{
    if (!m_bAudioProfilePending.exchange(false, std::memory_order_acq_rel))
        return;

    // Seed from the matching BUILT-IN, not from generic Defaults: Load() only
    // assigns fields the stored JSON carries, so a profile file written before
    // a field existed would otherwise reset that field to its generic default
    // forever. See AudioProfileStore::SeedFor.
    AudioProfile next = AudioProfileStore::SeedFor(m_szPendingAudioProfile);
    if (!AudioProfiles().Load(m_szPendingAudioProfile, next)) {
        DLOG_WARN("Audio profile '%ls' not found; keeping the current one",
                  m_szPendingAudioProfile);
        return;
    }

    // The smoothed and peak spectra hold history in the OLD profile's units,
    // and shaders read them as absolute values. Carrying them across a scale
    // change would leave the frame wrong until the filters re-converge, and
    // zeroing them would blank every FFT-driven preset for just as long.
    // Rescaling keeps what shaders see continuous through the switch.
    const float oldScale = m_audioProfile.fftScale;
    if (oldScale > 1e-12f && next.fftScale != oldScale) {
        const float k = next.fftScale / oldScale;
        for (int i = 0; i < MY_FFT_SAMPLES; i++) {
            m_fFFTSmoothed[i] *= k;
            m_fFFTPeak[i]     *= k;
        }
    }

    m_audioProfile = next;

    // The live attack/decay the FFT_ATTACK= / FFT_DECAY= commands set. Seeded
    // from the profile so a switch moves them; a later command still wins.
    m_fFFTAttackGlobal = next.fftAttack;
    m_fFFTDecayGlobal  = next.fftDecay;
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

void Engine::RebuildHashIndexLocked(AnnotStore& s)
{
    s.byHash.clear();
    for (auto& [key, a] : s.byKey)
        if (!a.hash.empty())
            s.byHash[a.hash] = key;
}


//----------------------------------------------------------------------
// GetAnnotationByHash — lookup by content identity
//----------------------------------------------------------------------

PresetAnnotation* Engine::FindByHashLocked(AnnotStore& s, const std::wstring& hash)
{
    if (hash.empty()) return nullptr;
    auto it = s.byHash.find(hash);
    if (it == s.byHash.end()) return nullptr;
    auto ann = s.byKey.find(it->second);
    return (ann == s.byKey.end()) ? nullptr : &ann->second;
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
    if (!into.hasAudioProfile && from.hasAudioProfile) {
        into.hasAudioProfile = true;
        into.audioProfile = from.audioProfile;
    }
    if (!into.hasReplacementPreset && from.hasReplacementPreset) {
        into.hasReplacementPreset = true;
        into.replacementPreset = from.replacementPreset;
    }

    if (into.notes.empty())     into.notes = from.notes;
    // One event, three fields: adopting the text without its date and kind
    // would recreate exactly the undated verdict this record exists to avoid.
    if (into.errorText.empty() && !from.errorText.empty()) {
        into.errorText = from.errorText;
        into.errorTime = from.errorTime;
        into.errorKind = from.errorKind;
    }

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

PresetAnnotation* Engine::ResolveLocked(AnnotStore& s, const wchar_t* filename,
                                        const wchar_t* hash, const wchar_t* fullPath,
                                        bool create)
{
    const std::wstring key = BareFilename(filename);
    if (key.empty()) return nullptr;
    const std::wstring wHash = hash ? hash : L"";

    // A scratch copy must not bind to the entry of the preset it was copied
    // from, by hash or by name. See IsScratchPreset.
    if (IsScratchPreset(fullPath)) return nullptr;

    PresetAnnotation* a = FindByHashLocked(s, wHash);

    if (!a) {
        auto it = s.byKey.find(key);
        if (it != s.byKey.end()) {
            a = &it->second;
            if (!wHash.empty() && a->hash.empty()) {
                // First time this entry has been seen with real content.  Bind
                // any observation migrated from a bare "rating" field to it, so
                // re-rating unchanged content updates that observation rather
                // than appending a second one for the same version.
                a->hash = wHash;
                s.byHash[wHash] = a->filename;
                AdoptHashIntoLegacyRatings(*a, wHash);
                s.dirty = true;
            }
            else if (!wHash.empty() && a->hash != wHash) {
                // Same name, different content: the file was edited.  If some
                // other entry already owns this content, that entry is the same
                // preset in another place -- fold this one into it.
                PresetAnnotation* owner = FindByHashLocked(s, wHash);
                if (owner && owner != a) {
                    MergeAnnotations(*owner, *a);
                    s.byKey.erase(it);
                    RebuildHashIndexLocked(s);
                    s.dirty = true;
                    DLOG_INFO("ResolveAnnotation: merged a duplicate entry into the one owning this content hash");
                    a = owner;
                } else {
                    if (!a->hash.empty()) s.byHash.erase(a->hash);
                    a->hash = wHash;
                    s.byHash[wHash] = a->filename;
                    s.dirty = true;
                }
            }
        }
    }

    if (!a) {
        if (!create) return nullptr;
        // NOT gated here, deliberately.
        //
        // The gate used to sit on this branch, on the reasoning that this was
        // "the automatic path". It is not: the only two callers that pass
        // create=true are TickPresetUsage, which already refuses earlier and
        // never reaches this line, and SetPresetRatingMDX -- an explicit "rate
        // this preset" from the user. So the gate did nothing where it was
        // meant to and silently threw away a real rating where it was not,
        // while SET_PRESET_RATING still replied success.
        //
        // The rule is automatic-versus-explicit, not which function you are in:
        // writes the user asked for always go through, writes the app decided
        // to make on its own are gated at the site that decides. See
        // TickPresetUsage and AutoFlagPresetError.
        PresetAnnotation fresh;
        fresh.filename = key;
        fresh.hash = wHash;
        auto [iter, ok] = s.byKey.emplace(key, std::move(fresh));
        a = &iter->second;
        if (!wHash.empty()) s.byHash[wHash] = a->filename;
        s.dirty = true;
    }

    // Record where this copy lives.  Capped, because a preset that turns up in
    // hundreds of scanned folders should not grow presets.json without bound.
    //
    // Ignored locations are not recorded even on an entry that already exists:
    // a real preset copied into TEST/ for a run would otherwise leave that
    // scratch path behind in presets.json forever, and ResolveAnnotationPath
    // prefers the most recently recorded path -- so "Load" on a real preset
    // would start opening the throwaway copy.
    if (fullPath && fullPath[0] && !ShouldSkipAnnotationWrite(fullPath) &&
        a->paths.size() < kMaxAnnotationPaths) {
        bool have = false;
        for (const auto& p : a->paths)
            if (_wcsicmp(p.c_str(), fullPath) == 0) { have = true; break; }
        if (!have) {
            a->paths.push_back(fullPath);
            s.dirty = true;
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
        // The const with() overload: this method only reads, but it still has
        // to lock out a concurrent rehash while it does.
        const int mdx = m_annot.with([&](const AnnotStore& s) {
            auto it = s.byKey.find(key);
            return (it != s.byKey.end() && !it->second.ratings.empty())
                       ? AverageRating(it->second) : -1;
        });
        if (mdx >= 0) return mdx;
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

    SYSTEMTIME st; GetLocalTime(&st);
    wchar_t when[32];
    FormatTo(when, L"%04d-%02d-%02dT%02d:%02d:%02d",
             st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);

    const bool rated = m_annot.with([&](AnnotStore& s) {
        PresetAnnotation* a = ResolveLocked(s, filename, wHash.c_str(), nullptr, true);
        if (!a) return false;

        // One observation per content version: re-rating unchanged content
        // updates the existing entry rather than appending, so a preset rated
        // ten times cannot outvote one rated once.
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
        s.dirty = true;
        return true;
    });
    // Outside the guard: SavePresetAnnotations writes a file.
    if (rated) SavePresetAnnotations();
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

    const bool changed = m_annot.with([&](AnnotStore& s) {
        PresetAnnotation* a = FindLocked(s, filename, true);
        if (!a) return false;

        SYSTEMTIME st; GetLocalTime(&st);
        wchar_t when[32];
        FormatTo(when, L"%04d-%02d-%02dT%02d:%02d:%02d",
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
        s.dirty = true;
        return true;
    });
    // Outside the guard: SavePresetAnnotations writes a file.
    if (changed) SavePresetAnnotations();
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
    m_bPresetUsageSuppressed = false;   // the new preset gets judged on its own
}

//----------------------------------------------------------------------
// RepairAnnotationPaths - drop aliases that belong to a different preset
//
// TickPresetUsage used to pair the timed preset's FILENAME with whatever hash
// m_pState happened to hold, so an entry could be handed a path belonging to a
// completely different preset. Once recorded, the alias stayed: it pooled play
// counts, and ResolveAnnotationPath prefers the most recently recorded path, so
// "Load" on one preset would open another.
//
// The cause is fixed, but the aliases already written are still there. Every
// recorded path is checked against the entry that claims it, and one whose file
// hashes to something else is removed. A path whose file is missing is LEFT
// ALONE -- it cannot be judged, and it may be a legitimate copy on a drive that
// is not mounted. Purge Missing is the tool for those, and it is the user's
// call, not this one's.
//----------------------------------------------------------------------

int Engine::RepairAnnotationPaths(int* pChecked, int* pUnjudged)
{
    int removed = 0, checked = 0, unjudged = 0;

    // THREE PHASES, because the middle one hashes files off disk (#11).
    //
    // The obvious conversion -- wrap the whole loop in one m_annot.with() --
    // would hold the guard across a ComputePresetHashFile for every recorded
    // path in the database, which on a large library is thousands of file
    // reads, some of them on drives that answer slowly or not at all. Every
    // ToolWindow thread would be parked behind it. So: snapshot what needs
    // judging, judge it with the lock released, then apply.
    struct Candidate {
        std::wstring key;
        std::wstring hash;
        std::vector<std::wstring> paths;
    };
    std::vector<Candidate> work;
    m_annot.with([&](AnnotStore& s) {
        work.reserve(s.byKey.size());
        for (auto& kv : s.byKey) {
            if (kv.second.hash.empty()) continue;   // nothing to check against
            work.push_back({kv.first, kv.second.hash, kv.second.paths});
        }
    });

    // Phase 2: all the disk work, guard released.
    std::vector<std::pair<std::wstring, std::vector<std::wstring>>> repaired;
    for (const Candidate& c : work) {
        std::vector<std::wstring> keep;
        keep.reserve(c.paths.size());
        for (const std::wstring& path : c.paths) {
            if (GetFileAttributesW(path.c_str()) == INVALID_FILE_ATTRIBUTES) {
                ++unjudged;
                keep.push_back(path);
                continue;
            }
            const std::string h = ComputePresetHashFile(path.c_str());
            if (h.empty()) {
                // The file is there but could not be read -- locked, or on a
                // drive that answered the attribute query and then failed. That
                // is not evidence of anything, and a path is only ever removed
                // on evidence.
                ++unjudged;
                keep.push_back(path);
                continue;
            }
            ++checked;
            const std::wstring wh(h.begin(), h.end());
            if (wh == c.hash) {
                keep.push_back(path);
            } else {
                ++removed;
                DLOG_WARN("RepairAnnotationPaths: a recorded path hashes to a "
                          "different preset than the entry holding it -- removed");
            }
        }
        if (keep.size() != c.paths.size())
            repaired.emplace_back(c.key, std::move(keep));
    }

    // Phase 3: apply, guard held, and only to entries that still look the way
    // phase 1 found them. An entry rewritten while the disk work was running
    // is left alone rather than overwritten with a decision made about its
    // previous contents.
    if (!repaired.empty()) {
        m_annot.with([&](AnnotStore& s) {
            for (auto& [key, keep] : repaired) {
                auto it = s.byKey.find(key);
                if (it == s.byKey.end()) continue;
                it->second.paths.swap(keep);
                s.dirty = true;
            }
        });
    }

    if (removed) SavePresetAnnotations();
    if (pChecked) *pChecked = checked;
    if (pUnjudged) *pUnjudged = unjudged;
    return removed;
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

    // A measurement run parks one preset on screen for minutes at a time and
    // steps through hundreds of them. Counting that as listening would make
    // "most played" a report on the test harness, so testing mode banks
    // nothing at all -- not even for presets that already have an entry.
    if (ShouldSkipAnnotationWrite(m_szUsagePresetFile)) {
        m_bPresetUsageCounted = true;     // don't re-test this every frame
        m_bPresetUsageSuppressed = true;  // ...and stop Flush banking the time
        return;
    }

    // Hash the file being TIMED, not whatever m_pState is holding now.
    //
    // These are not always the same preset, and when they differ the damage is
    // silent and permanent. ResolveAnnotation looks the entry up by HASH but
    // records the path it is given, so a mismatched pair files this preset's
    // path under a different preset's entry -- and that entry then owns it
    // forever, pooling their play counts and offering the wrong file to "Load".
    //
    // Observed: `Flexi - jellyfish jam.milk` had accumulated nine aliases, all
    // of them unrelated .milk3 Shadertoy imports, because those load through
    // RenderFrameShadertoy and leave m_pState->m_szPresetHash holding the
    // previous preset. Every .milk3 watched for more than the usage threshold
    // filed itself under whatever was loaded before it.
    //
    // Reading the file again costs one open and an FNV pass, once per preset
    // actually watched. That is not a hot path, and it cannot disagree with
    // itself.
    const std::string hash = ComputePresetHashFile(m_szUsagePresetFile);
    const std::wstring wHash(hash.begin(), hash.end());

    SYSTEMTIME st; GetLocalTime(&st);
    wchar_t when[32];
    FormatTo(when, L"%04d-%02d-%02dT%02d:%02d:%02d",
             st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);

    const bool counted = m_annot.with([&](AnnotStore& s) {
        PresetAnnotation* a = ResolveLocked(s, m_szUsagePresetFile, wHash.c_str(),
                                            m_szUsagePresetFile, true);
        if (!a) return false;
        a->useCount++;
        a->lastUsed = when;
        s.dirty = true;
        return true;
    });
    m_bPresetUsageCounted = true;   // set regardless, so a failure is not retried every frame
    // Outside the guard: this is the RENDER thread, and SavePresetAnnotations
    // writes a file. Holding the guard across that would park every ToolWindow
    // thread behind a frame's disk write.
    if (counted) SavePresetAnnotations();
}

//----------------------------------------------------------------------
// FlushPresetUsage — bank the seconds a counted preset was on screen
//----------------------------------------------------------------------

void Engine::FlushPresetUsage()
{
    // m_bPresetUsageSuppressed rides along with m_bPresetUsageCounted, which the
    // tick sets even when it refuses to count -- see engine.h. Reading only the
    // "counted" flag here would suppress the play and bank the seconds anyway.
    if (!m_bPresetUsageCounted || m_bPresetUsageSuppressed || !m_szUsagePresetFile[0]) {
        m_szUsagePresetFile[0] = 0;
        m_fPresetUsageStart = 0;
        m_bPresetUsageCounted = false;
        m_bPresetUsageSuppressed = false;
        return;
    }

    const int seconds = (int)(GetTime() - m_fPresetUsageStart);
    if (seconds > 0) {
        const bool banked = m_annot.with([&](AnnotStore& s) {
            PresetAnnotation* a = FindLocked(s, m_szUsagePresetFile, false);
            if (!a) return false;
            a->secondsShown += seconds;
            s.dirty = true;
            return true;
        });
        // Outside the guard: SavePresetAnnotations writes a file.
        if (banked) SavePresetAnnotations();
    }

    m_szUsagePresetFile[0] = 0;
    m_fPresetUsageStart = 0;
    m_bPresetUsageCounted = false;
    m_bPresetUsageSuppressed = false;
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
    const bool changed = m_annot.with([&](AnnotStore& s) {
        if (!key.empty()) {
            auto it = s.byKey.find(key);
            if (it == s.byKey.end()) return false;
            clear(it->second);
        } else {
            for (auto& kv : s.byKey) clear(kv.second);
        }
        s.dirty = true;
        return true;
    });
    // Outside the guard: SavePresetAnnotations writes a file.
    if (changed) SavePresetAnnotations();
}

//----------------------------------------------------------------------
// SetPresetFlag — set or clear a flag bit
//----------------------------------------------------------------------

void Engine::SetPresetFlag(const wchar_t* filename, uint32_t flag, bool set)
{
    const bool changed = m_annot.with([&](AnnotStore& s) {
        PresetAnnotation* a = FindLocked(s, filename, set); // only create if setting
        if (!a) return false;

        if (set)
            a->flags |= flag;
        else
            a->flags &= ~flag;

        s.dirty = true;
        return true;
    });
    // Outside the guard: SavePresetAnnotations writes a file.
    if (changed) SavePresetAnnotations();
}

//----------------------------------------------------------------------
// SetPresetNote — set notes text
//----------------------------------------------------------------------

void Engine::SetPresetNote(const wchar_t* filename, const std::wstring& note)
{
    const bool changed = m_annot.with([&](AnnotStore& s) {
        PresetAnnotation* a = FindLocked(s, filename, true);
        if (!a) return false;
        a->notes = note;
        s.dirty = true;
        return true;
    });
    // Outside the guard: SavePresetAnnotations writes a file.
    if (changed) SavePresetAnnotations();
}

//----------------------------------------------------------------------
// AutoFlagPresetError — called from shader compilation failure path
//----------------------------------------------------------------------

void Engine::AutoFlagPresetError(const wchar_t* filename, const std::wstring& errorMsg,
                                 PresetErrorKind kind)
{
    // Counted before anything below can bail out.  This is the signal that
    // stops the apply path from retracting the flag we are about to write, so
    // it has to survive an empty filename or a failed lookup.
    if (kind == PresetErrorKind::Shader) m_nShaderErrorsThisLoad++;

    if (!filename || !filename[0]) return;

    // The app decided to write this, not the user -- so it is gated.
    //
    // This is the automatic write that matters most for scratch presets: a
    // measurement run compiles broken and half-converted presets constantly,
    // and every failure used to mint a presets.json entry complete with an
    // ERROR flag. The counter above is deliberately still incremented, because
    // the apply path uses it to decide whether to retract a flag, and that
    // logic must behave the same whether or not the entry is recorded.
    if (ShouldSkipAnnotationWrite(filename)) return;

    const bool changed = m_annot.with([&](AnnotStore& s) {
        PresetAnnotation* a = FindLocked(s, filename, true);
        if (!a) return false;
        a->flags |= PFLAG_ERROR;
        a->errorText = errorMsg;
        a->errorTime = IsoLocalNow();
        a->errorKind = kind;
        s.dirty = true;
        return true;
    });
    // Outside the guard: SavePresetAnnotations writes a file.
    if (changed) SavePresetAnnotations();
}

//----------------------------------------------------------------------
// ClearPresetShaderError -- retract a shader error the preset has outgrown
//
// Nothing on the success path used to retract this flag, and the stored text
// carried no date, so a preset that a later build of the shader preprocessor
// learned to fix stayed marked broken forever, with evidence no one could tell
// was stale.  Called from the apply path once a load has finished without a
// single shader compile error.
//
// Only a Shader error is retracted.  A Runtime record says the preset crashed
// while drawing, and shaders building cleanly does not disprove that.
//----------------------------------------------------------------------

void Engine::ClearPresetShaderError(const wchar_t* filename)
{
    if (!filename || !filename[0]) return;
    // Keyed the way AutoFlagPresetError wrote it -- bare filename, no create --
    // so the retraction can only ever reach the entry the flag is on.
    // Gated exactly as AutoFlagPresetError is. Recording an error was already
    // blocked in testing mode and under an ignored directory; retracting one
    // was not, so a measurement run could never add a flag but could silently
    // remove somebody's -- and a -child instance, which is permanently in
    // testing mode, wrote to the shared presets.json every time a formerly
    // broken preset compiled. Retraction is the app deciding on its own, which
    // is the whole category testing mode exists to suppress.
    if (ShouldSkipAnnotationWrite(filename)) return;

    const bool changed = m_annot.with([&](AnnotStore& s) {
        PresetAnnotation* a = FindLocked(s, filename, false);
        if (!a) return false;
        if (a->errorKind != PresetErrorKind::Shader) return false;
        if (a->errorText.empty() && !(a->flags & PFLAG_ERROR)) return false;

        // The bare flag is retracted along with the text because AutoFlagPresetError
        // is the only thing that sets PFLAG_ERROR -- nothing lets a person mark a
        // preset as broken by hand.  A textless flag therefore came from an import
        // merging another machine's flags, which a local clean compile does answer.
        // If a manual "mark as error" is ever added, this must stop clearing flags
        // it cannot see evidence for.

        a->flags &= ~PFLAG_ERROR;
        a->errorText.clear();
        a->errorTime.clear();
        s.dirty = true;
        return true;
    });
    // Outside the guard: SavePresetAnnotations writes a file.
    if (changed) SavePresetAnnotations();
    DLOG_INFO("ClearPresetShaderError: %ls compiled clean -- stale error retracted",
              filename);
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
        a.errorTime = item[L"errorTime"].asString();
        a.errorKind = ErrorKindFrom(item, a.errorText);
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
        if (item.has(L"audioProfile")) {
            a.hasAudioProfile = true;
            a.audioProfile = item[L"audioProfile"].asString();
        }
        if (item.has(L"replacementPreset")) {
            a.hasReplacementPreset = true;
            a.replacementPreset = item[L"replacementPreset"].asString();
        }
        if (item.has(L"canvasMax")) {
            a.hasCanvasMax = true;
            a.canvasMax = item[L"canvasMax"].asInt();
        }
        if (item.has(L"feedbackDamp")) {
            a.hasFeedbackDamp = true;
            a.feedbackDamp = item[L"feedbackDamp"].asFloat();
        }
        if (item.has(L"fpsMax")) {
            a.hasFpsMax = true;
            a.fpsMax = item[L"fpsMax"].asInt();
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
        const std::wstring name_i = PresetNameAt(i);
        const wchar_t* fn = name_i.c_str();
        const float r = m_presets.with([&](const PresetList& v) {
            return i < (int)v.size() ? v[i].fRatingThis : 0.0f;
        });
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
    const bool changed = m_annot.with([&](AnnotStore& s) {
        PresetAnnotation* a = FindLocked(s, filename, true);
        if (!a) return false;
        a->tags = tags;
        s.dirty = true;
        return true;
    });
    // Outside the guard: SavePresetAnnotations writes a file.
    if (changed) SavePresetAnnotations();
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
    const bool changed = m_annot.with([&](AnnotStore& s) {
        PresetAnnotation* a = FindLocked(s, filename, true);
        if (!a) return false;
        a->hasShaderOverride = present;
        a->shaderOverride = present ? name : std::wstring();
        s.dirty = true;
        return true;
    });
    // Outside the guard: SavePresetAnnotations writes a file.
    if (changed) SavePresetAnnotations();
}

void Engine::SetPresetVFXProfile(const wchar_t* filename,
                                 const std::wstring& name, bool present)
{
    const bool changed = m_annot.with([&](AnnotStore& s) {
        PresetAnnotation* a = FindLocked(s, filename, true);
        if (!a) return false;
        a->hasVfxProfile = present;
        a->vfxProfile = present ? name : std::wstring();
        s.dirty = true;
        return true;
    });
    // Outside the guard: SavePresetAnnotations writes a file.
    if (changed) SavePresetAnnotations();
}

bool Engine::SetPresetAudioProfile(const wchar_t* filename,
                                   const std::wstring& name, bool present)
{
    // Through the same lookup the resolver uses. Writing by filename while
    // reading by hash puts the value on one entry and looks for it on another:
    // SOLID_blue.milk's hash resolves to SOLID_blend.milk2, the .milk2 wrapper
    // that contains it, so the setting was stored and then never found.
    const bool changed = m_annot.with([&](AnnotStore& s) {
        PresetAnnotation* a = ForPresetLocked(s, filename, m_pState, true);
        if (!a) return false;
        a->hasAudioProfile = present;
        a->audioProfile = present ? name : std::wstring();
        s.dirty = true;
        return true;
    });
    // Outside the guard: SavePresetAnnotations writes a file.
    if (changed) SavePresetAnnotations();
    // Whether it BOUND. False means ForPresetLocked refused to make an entry --
    // a scratch preset (testing mode plus an AnnotationIgnoreDirs folder) binds
    // no per-preset override at all. Returning void meant every caller assumed
    // success, and an IPC client that asked for a profile got silence whether
    // or not anything happened (#88).
    return changed;
}

//----------------------------------------------------------------------
// SetPresetReplacement — designate the file that runs in place of a preset
//----------------------------------------------------------------------

void Engine::SetPresetReplacement(const wchar_t* filename,
                                  const std::wstring& path, bool present)
{
    // By FILENAME, deliberately, and not through GetAnnotationForPreset.
    //
    // The resolver runs inside LoadPreset before the file is opened, so it has
    // no hash to resolve with -- only the name. Writing this by hash and reading
    // it by name would put the value on one entry and look for it on another,
    // which is the exact failure SetPresetAudioProfile documents above.
    const bool changed = m_annot.with([&](AnnotStore& s) {
        PresetAnnotation* a = FindLocked(s, filename, true);
        if (!a) return false;
        a->hasReplacementPreset = present;
        a->replacementPreset = present ? path : std::wstring();
        s.dirty = true;
        return true;
    });
    // Outside the guard: SavePresetAnnotations writes a file.
    if (changed) SavePresetAnnotations();
}

//----------------------------------------------------------------------
// PresetReplacementFor — the designated stand-in for a preset, or empty
//----------------------------------------------------------------------

std::wstring Engine::PresetReplacementFor(const wchar_t* filename)
{
    if (!filename || !filename[0]) return std::wstring();
    std::wstring replacement;
    WithAnnotation(filename, /*create=*/false, [&](PresetAnnotation& a) {
        if (a.hasReplacementPreset) replacement = a.replacementPreset;
    });
    return replacement;
}

//----------------------------------------------------------------------
// CollectAllTags — gather unique sorted list of all tags across annotations
//----------------------------------------------------------------------

void Engine::CollectAllTags(std::vector<std::wstring>& allTags) const
{
    std::set<std::wstring> tagSet;
    m_annot.with([&](const AnnotStore& s) {
        for (auto& [key, a] : s.byKey)
            for (auto& t : a.tags)
                tagSet.insert(t);
    });
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
        const bool changed = m_annot.with([&](AnnotStore& s) {
            PresetAnnotation* a = FindLocked(s, filename.c_str(), true);
            if (!a) return false;
            bool any = false;
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
                    any = true;
                }
            }
            return any;
        });
        if (changed) nUpdated++;
    }

    if (nUpdated > 0) {
        m_annot.with([](AnnotStore& s) { s.dirty = true; });
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
    FormatTo(szMask, L"%s*.txt", szDir);

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
    // Same lock order as the render thread's swap: the section outside, the
    // preset guard inside.
    EnterCriticalSection(&g_cs);
    m_presets.with([&](PresetList& v) { v = std::move(temp_presets); });
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

//----------------------------------------------------------------------
// Preset browsing over IPC (forgejo#41)
//----------------------------------------------------------------------
//
// A remote could load a preset by path and nothing else: it could not discover
// what exists, and every SET_PRESET_* writes to whatever is RUNNING. Browsing
// needs the opposite of both, because loading a preset in order to rate it
// changes what is on screen mid-show.
//
// presets.json is what gets browsed rather than the preset directory: 578
// curated entries against 23,601 files on disk here, and the only place
// ratings, flags, notes and tags live.

// The folder a preset sits in, from a full path.
//
// A preset in several places belongs to EVERY folder it appears in. 48 entries
// hold more than one path, and two folders exist only as an alias path, so
// taking paths[0] alone would hide those folders rather than merely miscount.
static std::wstring FolderOfPath(const std::wstring& path) {
    const size_t last = path.find_last_of(L"\\/");
    if (last == std::wstring::npos || last == 0) return L"";
    const size_t prev = path.find_last_of(L"\\/", last - 1);
    return prev == std::wstring::npos ? path.substr(0, last)
                                      : path.substr(prev + 1, last - prev - 1);
}

static bool BrowseContainsNoCase(const std::wstring& hay, const std::wstring& needle) {
    if (needle.empty()) return true;
    auto it = std::search(hay.begin(), hay.end(), needle.begin(), needle.end(),
                          [](wchar_t a, wchar_t b) { return towlower(a) == towlower(b); });
    return it != hay.end();
}

// Semicolon-joined flag names. NOT FlagsToString, which emits a JSON array for
// the file writer and would put brackets and quotes on the wire.
static std::wstring FlagNamesJoined(uint32_t flags) {
    std::wstring s;
    auto add = [&](const wchar_t* n) { if (!s.empty()) s += L";"; s += n; };
    if (flags & PFLAG_FAVORITE) add(L"favorite");
    if (flags & PFLAG_ERROR)    add(L"error");
    if (flags & PFLAG_SKIP)     add(L"skip");
    if (flags & PFLAG_BROKEN)   add(L"broken");
    if (flags & PFLAG_CANVAS)   add(L"canvas");
    return s;
}

static uint32_t FlagBitFromName(const std::wstring& n) {
    if (n == L"favorite") return PFLAG_FAVORITE;
    if (n == L"error")    return PFLAG_ERROR;
    if (n == L"skip")     return PFLAG_SKIP;
    if (n == L"broken")   return PFLAG_BROKEN;
    if (n == L"canvas")   return PFLAG_CANVAS;
    return 0;
}

// "key=value" out of a comma-separated argument list; empty when absent.
static std::wstring BrowseArg(const std::wstring& args, const wchar_t* key) {
    const std::wstring probe = std::wstring(L",") + key + L"=";
    const std::wstring padded = std::wstring(L",") + args;
    const size_t at = padded.find(probe);
    if (at == std::wstring::npos) return L"";
    const size_t from = at + probe.size();
    const size_t to = padded.find(L",", from);
    return padded.substr(from, to == std::wstring::npos ? std::wstring::npos : to - from);
}

// Walk one directory tree, recording every preset file by bare name.
static void IndexPresetDir(const std::wstring& dir,
                           std::unordered_map<std::wstring, std::vector<std::wstring>>& index,
                           int depth, int& budget) {
    if (depth > 12 || budget <= 0) return;

    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW((dir + L"\\*").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        if (fd.cFileName[0] == L'.') continue;          // . .. and dotfiles
        const std::wstring full = dir + L"\\" + fd.cFileName;
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            IndexPresetDir(full, index, depth + 1, budget);
        } else {
            const wchar_t* ext = wcsrchr(fd.cFileName, L'.');
            if (!ext || _wcsnicmp(ext, L".milk", 5) != 0) continue;
            std::wstring key = fd.cFileName;
            for (auto& c : key) c = towlower(c);
            index[key].push_back(full);
            if (--budget <= 0) break;
        }
    } while (FindNextFileW(h, &fd));
    FindClose(h);
}

// Index the whole collection, not just the directory being browsed.
//
// szPresetDir is routinely a SUBFOLDER -- on this install it is
// ...\MilkAssets\md31_rewritten\, holding 54 of 578 annotated presets -- so
// resolving against it alone leaves the rest unlocatable. The parent is the
// collection root in every layout seen so far, so that is what gets walked.
void Engine::BuildPresetPathIndex() {
    if (m_bPresetPathIndexBuilt) return;
    m_bPresetPathIndexBuilt = true;      // set first: a failed walk must not retry forever

    if (!m_szPresetDir[0]) return;

    std::wstring root = m_szPresetDir;
    while (!root.empty() && (root.back() == L'\\' || root.back() == L'/'))
        root.pop_back();

    // Up one, to the collection rather than the folder in use. Refuse to do
    // that if it would leave us at a drive root -- walking C:\ to find a preset
    // is not a trade anyone wants.
    const size_t cut = root.find_last_of(L"\\/");
    if (cut != std::wstring::npos && cut > 2) {
        const std::wstring parent = root.substr(0, cut);
        if (parent.size() > 3) root = parent;
    }

    int budget = 200000;                 // a runaway-tree backstop, never a real limit
    const DWORD started = GetTickCount();
    IndexPresetDir(root, m_presetPathIndex, 0, budget);
    DLOG_INFO("preset path index: %d name(s) under %ls in %lu ms",
              (int)m_presetPathIndex.size(), root.c_str(),
              (unsigned long)(GetTickCount() - started));
}

std::wstring Engine::FindPresetByName(const std::wstring& bareFilename) {
    if (bareFilename.empty()) return std::wstring();
    BuildPresetPathIndex();

    std::wstring key = bareFilename;
    const size_t cut = key.find_last_of(L"\\/");
    if (cut != std::wstring::npos) key = key.substr(cut + 1);
    for (auto& c : key) c = towlower(c);

    auto it = m_presetPathIndex.find(key);
    if (it == m_presetPathIndex.end() || it->second.empty()) return std::wstring();

    // A name can exist in several folders. Prefer one under the directory being
    // browsed, so a browse inside a folder resolves to that folder's copy
    // rather than an arbitrary duplicate elsewhere.
    if (m_szPresetDir[0]) {
        const std::wstring cur = m_szPresetDir;
        for (auto& p : it->second)
            if (_wcsnicmp(p.c_str(), cur.c_str(), cur.size()) == 0) return p;
    }
    return it->second.front();
}

bool Engine::HandlePresetBrowseIPC(const wchar_t* sMessage) {
    extern PipeServer g_pipeServer;

    if (MSG_IS(sMessage, L"RAISE_WINDOW")) {
        // Bring this instance's render window to the top without taking focus.
        // The parent sends it to a freshly placed child, which would otherwise
        // sit behind whatever was already on that monitor.
        HWND hRender = m_lpDX ? m_lpDX->GetHwnd() : nullptr;
        if (hRender)
            PostMessage(hRender, WM_MW_RAISE_WINDOW, 0, 0);
        g_pipeServer.Send(L"RAISED=1");
        return true;
    }

    if (MSG_IS(sMessage, L"LOWER_WINDOW")) {
        // The counterpart RAISE_WINDOW never had (#237). A display given over
        // to the visualiser could be raised over someone's editor from a
        // remote and then only minimised -- which blanks the screen -- or left
        // where it was.
        //
        // Posted to the render window rather than done here: the window
        // belongs to the render thread, and the raise goes the same way.
        HWND hRender = m_lpDX ? m_lpDX->GetHwnd() : nullptr;
        if (hRender)
            PostMessage(hRender, WM_MW_LOWER_WINDOW, 0, 0);
        g_pipeServer.Send(L"LOWERED=1");
        return true;
    }

    if (MSG_IS(sMessage, L"GET_PRESET_FOLDERS")) {
        // These counts sum to MORE than the number of entries, deliberately: a
        // preset in two places is in both folders. Not a partition.
        std::map<std::wstring, int> counts;
        // Counted under the guard, formatted and sent outside it -- the pipe
        // send blocks, and blocking IPC under this lock parks every ToolWindow.
        m_annot.with([&](const AnnotStore& s) {
            for (auto& kv : s.byKey) {
                std::set<std::wstring> seen;      // once per folder per entry
                for (auto& p : kv.second.paths) {
                    const std::wstring f = FolderOfPath(p);
                    if (!f.empty()) seen.insert(f);
                }
                for (auto& f : seen) counts[f]++;
            }
        });
        std::wstring reply = L"FOLDERS";
        for (auto& c : counts)
            reply += std::wstring(L"|") + c.first + L"=" + std::to_wstring(c.second);
        g_pipeServer.Send(reply.c_str());
        return true;
    }

    if (MSG_IS(sMessage, L"GET_PRESET_TAGS")) {
        std::map<std::wstring, int> counts;
        m_annot.with([&](const AnnotStore& s) {
            for (auto& kv : s.byKey)
                for (auto& t : kv.second.tags)
                    if (!t.empty()) counts[t]++;
        });
        std::wstring reply = L"TAGS";
        for (auto& c : counts)
            reply += std::wstring(L"|") + c.first + L"=" + std::to_wstring(c.second);
        g_pipeServer.Send(reply.c_str());
        return true;
    }

    if (MSG_IS(sMessage, L"GET_PRESET_NOTE_FOR=")) {
        // The read half of SET_PRESET_NOTE_FOR=, for a browser that wants to
        // show a preset's note before deciding whether to write one -- the
        // running-preset form already has both GET_PRESET_NOTE and
        // SET_PRESET_NOTE=, so the addressed form should too.
        const std::wstring hash(sMessage + 20);
        std::wstring note;
        const bool found = WithAnnotationByHash(hash, [&](const PresetAnnotation& a) {
            note = a.notes;
        });
        // Both sends are outside the guard: g_pipeServer.Send blocks.
        if (!found) {
            g_pipeServer.Send((std::wstring(L"PRESET_ANNOT=ERROR|no entry for hash ") + hash).c_str());
            return true;
        }
        g_pipeServer.Send((std::wstring(L"PRESET_NOTE_FOR=") + hash + L"|" + note).c_str());
        return true;
    }

    if (MSG_IS(sMessage, L"BROWSE_PRESETS=")) {
        // BROWSE_PRESETS=<offset>,<count>[,q=][,folder=][,tag=][,flag=]
        //                [,minrating=][,sort=name|rating|used|lastused]
        const std::wstring args(sMessage + 15);
        int offset = _wtoi(args.c_str());
        if (offset < 0) offset = 0;
        const size_t comma = args.find(L",");
        int want = (comma == std::wstring::npos) ? 50 : _wtoi(args.c_str() + comma + 1);
        // A page size the caller cannot raise. The whole database in one
        // message is ~0.24MB, and it would go through the pipe AND the TCP
        // broadcast on every filter keystroke.
        if (want < 1)   want = 1;
        if (want > 200) want = 200;

        const std::wstring q          = BrowseArg(args, L"q");
        const std::wstring folder     = BrowseArg(args, L"folder");
        const std::wstring tag        = BrowseArg(args, L"tag");
        const std::wstring flagName   = BrowseArg(args, L"flag");
        const std::wstring sortKey    = BrowseArg(args, L"sort");
        const std::wstring minRatingS = BrowseArg(args, L"minrating");
        const int minRating = minRatingS.empty() ? -1 : _wtoi(minRatingS.c_str());
        const uint32_t wantFlag = flagName.empty() ? 0u : FlagBitFromName(flagName);

        // THREE PHASES, because rendering a page does file I/O and blocking
        // IPC, neither of which may happen under the guard (#11).
        //
        // Phase 1 filters, sorts and COPIES the page's fields out. The old code
        // kept `PresetAnnotation*` into the map across every pipe send in the
        // loop below -- a reload arriving mid-page freed every one of them.
        struct Row {
            std::wstring key, hash, filename, lastUsed;
            std::vector<std::wstring> paths, tags;
            int rating = 0, useCount = 0, secondsShown = 0;
            uint32_t flags = 0;
        };
        std::vector<Row> page;
        int total = 0;
        m_annot.with([&](AnnotStore& st) {
            // Filter first, so total= is the size of the FILTERED set: a client
            // paging inside a folder needs that length, not the database's.
            std::vector<const PresetAnnotation*> hits;
            hits.reserve(st.byKey.size());
            for (auto& kv : st.byKey) {
                const PresetAnnotation& a = kv.second;
                if (!q.empty() && !BrowseContainsNoCase(a.filename, q)) continue;
                if (minRating >= 0 && a.rating < minRating) continue;
                if (wantFlag && !(a.flags & wantFlag)) continue;
                if (!tag.empty() &&
                    std::find(a.tags.begin(), a.tags.end(), tag) == a.tags.end()) continue;
                if (!folder.empty()) {
                    bool in = false;
                    for (auto& pp : a.paths)
                        if (_wcsicmp(FolderOfPath(pp).c_str(), folder.c_str()) == 0) { in = true; break; }
                    if (!in) continue;
                }
                hits.push_back(&a);
            }

            // Sorting belongs here: a client holding one page cannot order a
            // set it has not seen. Every comparison falls back to the filename
            // so the order is total -- an unstable sort silently duplicates and
            // drops rows across page boundaries.
            auto byName = [](const PresetAnnotation* x, const PresetAnnotation* y) {
                return _wcsicmp(x->filename.c_str(), y->filename.c_str()) < 0;
            };
            if (sortKey == L"rating")
                std::sort(hits.begin(), hits.end(),
                    [&](const PresetAnnotation* x, const PresetAnnotation* y) {
                        return x->rating != y->rating ? x->rating > y->rating : byName(x, y); });
            else if (sortKey == L"used")
                std::sort(hits.begin(), hits.end(),
                    [&](const PresetAnnotation* x, const PresetAnnotation* y) {
                        return x->useCount != y->useCount ? x->useCount > y->useCount : byName(x, y); });
            else if (sortKey == L"time")
                std::sort(hits.begin(), hits.end(),
                    [&](const PresetAnnotation* x, const PresetAnnotation* y) {
                        return x->secondsShown != y->secondsShown
                             ? x->secondsShown > y->secondsShown : byName(x, y); });
            else if (sortKey == L"lastused")
                std::sort(hits.begin(), hits.end(),
                    [&](const PresetAnnotation* x, const PresetAnnotation* y) {
                        return x->lastUsed != y->lastUsed ? x->lastUsed > y->lastUsed : byName(x, y); });
            else
                std::sort(hits.begin(), hits.end(), byName);

            total = (int)hits.size();
            for (int i = offset; i < total && (int)page.size() < want; ++i) {
                const PresetAnnotation& a = *hits[i];
                Row r;
                r.key = a.filename;      // the byKey key IS the bare filename
                r.hash = a.hash; r.filename = a.filename; r.lastUsed = a.lastUsed;
                r.paths = a.paths; r.tags = a.tags;
                r.rating = a.rating; r.useCount = a.useCount;
                r.secondsShown = a.secondsShown; r.flags = a.flags;
                page.push_back(std::move(r));
            }
        });

        // Phase 2: resolve paths, format and send, guard RELEASED.
        int sent = 0;
        int backfilled = 0;   // paths learned while rendering this page
        std::vector<std::pair<std::wstring, std::wstring>> learned;
        for (Row& a : page) {
            ++sent;

            // Report the path that matched the folder filter, else resolve one.
            //
            // Resolving matters more than it looks: 360 of 578 entries here
            // carry no recorded path at all, and that includes every one of the
            // 69 five-star presets -- they were created by scanning .milk
            // fRating values, which keys by filename only. Reporting paths[0]
            // would hand the client an empty path for exactly the presets it
            // most wants to load. ResolveAnnotationPath is a handful of
            // PathFileExistsW calls, not a directory scan, so it is fine per
            // row -- but it IS file I/O, which is why it sits out here rather
            // than inside the guard.
            std::wstring path;
            if (!folder.empty())
                for (auto& pp : a.paths)
                    if (_wcsicmp(FolderOfPath(pp).c_str(), folder.c_str()) == 0) { path = pp; break; }
            if (path.empty()) {
                PresetAnnotation probe;      // ResolveAnnotationPath reads only these
                probe.filename = a.filename;
                probe.hash = a.hash;
                probe.paths = a.paths;
                path = ResolveAnnotationPath(probe);
                if (path.empty())
                    path = FindPresetByName(a.filename);

                // Record what was resolved, so the entry stops being pathless.
                //
                // Browsing is the first thing that ever has to answer "where is
                // this preset?" for these entries, and resolving it invisibly on
                // every page leaves the database no better than it was. Writing
                // it back means the full path is there for anything that looks
                // later -- filtering, forking, or a person reading the file --
                // instead of each reader having to work it out again. Collected
                // here, applied in phase 3.
                if (!path.empty() &&
                    std::find(a.paths.begin(), a.paths.end(), path) == a.paths.end()) {
                    learned.emplace_back(a.key, path);
                    backfilled++;
                }
            }

            std::wstring tags;
            for (auto& t : a.tags) { if (!tags.empty()) tags += L";"; tags += t; }

            std::wstring line = std::wstring(L"PRESET_ROW|hash=") + a.hash;
            line += std::wstring(L"|name=")     + a.filename;
            line += std::wstring(L"|rating=")   + std::to_wstring(a.rating);
            line += std::wstring(L"|flags=")    + FlagNamesJoined(a.flags);
            line += std::wstring(L"|tags=")     + tags;
            line += std::wstring(L"|folder=")   + FolderOfPath(path);
            line += std::wstring(L"|used=")     + std::to_wstring(a.useCount);
            line += std::wstring(L"|seconds=")  + std::to_wstring(a.secondsShown);
            line += std::wstring(L"|lastUsed=") + a.lastUsed;
            // An entry nothing can locate. The client needs this to disable
            // loading rather than sending PRESET= with an empty path and
            // appearing to do nothing.
            line += std::wstring(L"|missing=")  + (path.empty() ? L"1" : L"0");
            line += std::wstring(L"|path=")     + path;
            g_pipeServer.Send(line.c_str());
        }

        // Phase 3: write the resolved paths back, guard held, skipping any
        // entry that has gone or has already learned the path meanwhile.
        //
        // One save for the whole page, never one per row: SavePresetAnnotations
        // rewrites the entire file, and doing that per row would turn a scroll
        // into hundreds of full-file writes.
        if (backfilled > 0) {
            m_annot.with([&](AnnotStore& st) {
                for (auto& kv : learned) {
                    auto it = st.byKey.find(kv.first);
                    if (it == st.byKey.end()) continue;
                    auto& paths = it->second.paths;
                    if (std::find(paths.begin(), paths.end(), kv.second) != paths.end()) continue;
                    paths.push_back(kv.second);
                    st.dirty = true;
                }
            });
            SavePresetAnnotations();
            DLOG_INFO("browse: recorded %d resolved path(s) into presets.json", backfilled);
        }

        wchar_t endBuf[128];
        FormatTo(endBuf, L"PRESET_ROWS_END|total=%d|offset=%d|count=%d|backfilled=%d",
                 total, offset, sent, backfilled);
        g_pipeServer.Send(endBuf);
        return true;
    }

    // Annotating a preset that is NOT the one playing.
    //
    // Addressed by content hash, the database's own primary key, which survives
    // the file being renamed or moved. The existing one-argument forms still
    // mean "the running preset", so Milkwave Remote is unaffected.
    // MSG_IS takes the literal directly: it is a macro over _countof, so a
    // const wchar_t* variable does not compile.
    const wchar_t* body = nullptr;
    int which = 0;
    if (MSG_IS(sMessage, L"SET_PRESET_RATING_FOR=")) { body = sMessage + 22; which = 1; }
    else if (MSG_IS(sMessage, L"SET_PRESET_FLAG_FOR=")) { body = sMessage + 20; which = 2; }
    else if (MSG_IS(sMessage, L"SET_PRESET_TAG_FOR=")) { body = sMessage + 19; which = 3; }
    else if (MSG_IS(sMessage, L"SET_PRESET_NOTE_FOR=")) { body = sMessage + 20; which = 4; }
    if (!which) return false;

    const std::wstring args(body);
    const size_t c1 = args.find(L",");
    if (c1 == std::wstring::npos) {
        g_pipeServer.Send(L"PRESET_ANNOT=ERROR|expected <hash>,<value>");
        return true;
    }
    const std::wstring hash = args.substr(0, c1);
    // The note's text is everything after the first comma, taken verbatim --
    // unlike flag/tag it is not itself <name>,<0|1>, so it must not be split
    // again. A note is free text and may contain commas of its own.
    const std::wstring rest = args.substr(c1 + 1);

    // Mutate under the guard, reply after it (#11).
    //
    // Every failure branch below used to Send and return from inside the block
    // that holds the entry. g_pipeServer.Send is blocking IPC, so doing that
    // under the guard would park every ToolWindow thread on the far end of a
    // pipe write. The outcome is carried out as a value and answered here.
    enum class AnnotResult { Ok, NoEntry, BadPair, UnknownFlag, EmptyTag };

    const AnnotResult outcome = m_annot.with([&](AnnotStore& s) {
        PresetAnnotation* a = FindByHashLocked(s, hash);
        if (!a) return AnnotResult::NoEntry;

        if (which == 1) {
            int r = _wtoi(rest.c_str());
            a->rating = r < 0 ? 0 : (r > 5 ? 5 : r);
        } else if (which == 4) {
            a->notes = rest;
        } else {
            const size_t c2 = rest.find(L",");
            if (c2 == std::wstring::npos) return AnnotResult::BadPair;
            const std::wstring name = rest.substr(0, c2);
            const bool on = _wtoi(rest.c_str() + c2 + 1) != 0;
            if (which == 2) {
                const uint32_t bit = FlagBitFromName(name);
                if (!bit) return AnnotResult::UnknownFlag;
                if (on) a->flags |= bit; else a->flags &= ~bit;
            } else {
                if (name.empty()) return AnnotResult::EmptyTag;
                auto it = std::find(a->tags.begin(), a->tags.end(), name);
                if (on) { if (it == a->tags.end()) a->tags.push_back(name); }
                else if (it != a->tags.end()) a->tags.erase(it);
            }
        }
        s.dirty = true;
        return AnnotResult::Ok;
    });

    switch (outcome) {
    case AnnotResult::NoEntry:
        g_pipeServer.Send((std::wstring(L"PRESET_ANNOT=ERROR|no entry for hash ") + hash).c_str());
        return true;
    case AnnotResult::BadPair:
        g_pipeServer.Send(L"PRESET_ANNOT=ERROR|expected <hash>,<name>,<0|1>");
        return true;
    case AnnotResult::UnknownFlag:
        g_pipeServer.Send(L"PRESET_ANNOT=ERROR|unknown flag, expected favorite|error|skip|broken|canvas");
        return true;
    case AnnotResult::EmptyTag:
        g_pipeServer.Send(L"PRESET_ANNOT=ERROR|empty tag name");
        return true;
    case AnnotResult::Ok:
        break;
    }

    SavePresetAnnotations();
    g_pipeServer.Send((std::wstring(L"PRESET_ANNOT=OK|") + hash).c_str());
    return true;
}

} // namespace mdrop



//----------------------------------------------------------------------
// SetPresetFpsMaxByFile - per-preset frame-rate ceiling (#222)
//
// The same shape as SetPresetCanvasMaxByFile, one axis over. 0 CLEARS the
// flag rather than storing a zero, because absent and zero are different
// states: absent means "inherit the user's cap", and a stored 0 would read as
// "this preset wants unlimited", which is the one thing a ceiling must never
// be able to say.
//----------------------------------------------------------------------
void mdrop::Engine::SetPresetFpsMaxByFile(const wchar_t* filenameOnly, int fps)
{
    if (!filenameOnly || !*filenameOnly) return;

    // A scratch preset has no identity to hang this on -- same refusal the
    // canvas setter makes, for the same reason.
    const wchar_t* curPath = CurrentPresetLeaf();
    if (_wcsicmp(curPath, filenameOnly) == 0 &&
        IsScratchPreset(m_szCurrentPresetFile))
        return;

    const bool changed = m_annot.with([&](AnnotStore& s) {
        PresetAnnotation* a = FindLocked(s, filenameOnly, /*create=*/true);
        if (!a) return false;
        a->fpsMax = fps;
        a->hasFpsMax = (fps > 0);
        s.dirty = true;
        return true;
    });
    if (changed) SavePresetAnnotations();

    // Only re-resolve when this is the preset actually on screen; setting a
    // ceiling on some other preset in the browser must not change the live cap.
    if (_wcsicmp(CurrentPresetLeaf(), filenameOnly) == 0)
        ResolvePresetFpsCap();
}

//----------------------------------------------------------------------
// ResolvePresetFpsCap - cache the current preset's ceiling for the limiter
//
// Resolved on preset apply, not per frame: EnforceMaxFPS runs on the frame
// path and must not take the annotation lock there. Hash first, filename as
// the fallback, exactly as EffectiveCanvasLimit does -- so a preset that moved
// keeps its ceiling.
//----------------------------------------------------------------------
void mdrop::Engine::ResolvePresetFpsCap()
{
    int cap = 0;
    bool found = false;
    const char* h = m_pState ? m_pState->m_szPresetHash : "";
    if (h && *h) {
        const std::wstring wHash(h, h + strlen(h));
        found = WithAnnotationByHash(wHash, [&](const PresetAnnotation& a) {
            if (a.hasFpsMax) cap = a.fpsMax;
        });
    }
    if (!found) {
        // create=false: resolving a ceiling must not conjure an entry.
        WithAnnotation(CurrentPresetLeaf(), /*create=*/false,
                       [&](PresetAnnotation& a) {
            if (a.hasFpsMax) cap = a.fpsMax;
        });
    }
    if (cap != m_nPresetFpsCap) {
        m_nPresetFpsCap = cap;
        DLOG_INFO("Preset fps ceiling: %d (0 = inherit)", cap);
    }
}
