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
    swprintf_s(when, L"%04d-%02d-%02dT%02d:%02d:%02d",
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
    if (a.hasShaderOverride || a.hasVfxProfile || a.hasAudioProfile || a.hasCanvasMax)
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

    m_presetAnnotations.clear();
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
        if (item.has(L"canvasMax")) {
            a.hasCanvasMax = true;
            a.canvasMax = item[L"canvasMax"].asInt();
        }
        if (item.has(L"feedbackDamp")) {
            a.hasFeedbackDamp = true;
            a.feedbackDamp = item[L"feedbackDamp"].asFloat();
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
        m_presetAnnotations[a.filename] = std::move(a);
    }
    RebuildAnnotationHashIndex();
    m_bAnnotationsDirty = false;
    DLOG_INFO("LoadPresetAnnotations: loaded %d entries (%d with a hash)",
              (int)m_presetAnnotations.size(), (int)m_annotationsByHash.size());
    if (nDropped > 0) {
        // Written back now rather than waiting for the next annotation edit, so
        // the file stops carrying them even if nothing else changes this run.
        DLOG_WARN("LoadPresetAnnotations: dropped %d unreachable auto-error entries "
                  "(full-path key, no hash, nothing user-authored)", nDropped);
        m_bAnnotationsDirty = true;
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
    std::vector<std::wstring> doomed;
    for (auto& kv : m_presetAnnotations)
        if (IsAnnotationKnownMissing(kv.second))
            doomed.push_back(kv.first);

    for (const std::wstring& key : doomed)
        m_presetAnnotations.erase(key);

    if (!doomed.empty()) {
        RebuildAnnotationHashIndex();
        m_bAnnotationsDirty = true;
        SavePresetAnnotations();
    }
    return (int)doomed.size();
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
    const wchar_t* curPath = wcsrchr(m_szCurrentPresetFile, L'\\');
    curPath = curPath ? (curPath + 1) : m_szCurrentPresetFile;
    if (_wcsicmp(curPath, filenameOnly) == 0 &&
        IsScratchPreset(m_szCurrentPresetFile))
        return;

    PresetAnnotation* a = GetAnnotation(filenameOnly, /*create=*/true);
    if (!a) return;
    a->canvasMax = px;
    a->hasCanvasMax = (px > 0);

    // SavePresetAnnotations early-outs unless this is set.
    m_bAnnotationsDirty = true;
    SavePresetAnnotations();

    // Only rebuild when this is the preset actually on screen. Setting a limit
    // on some other preset in the browser must not resize the live canvas.
    const wchar_t* cur = wcsrchr(m_szCurrentPresetFile, L'\\');
    cur = cur ? (cur + 1) : m_szCurrentPresetFile;
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
    const wchar_t* curPath = wcsrchr(m_szCurrentPresetFile, L'\\');
    curPath = curPath ? (curPath + 1) : m_szCurrentPresetFile;
    if (_wcsicmp(curPath, filenameOnly) == 0 &&
        IsScratchPreset(m_szCurrentPresetFile))
        return;

    PresetAnnotation* a = GetAnnotation(filenameOnly, /*create=*/true);
    if (!a) return;
    a->feedbackDamp = strength;
    a->hasFeedbackDamp = (strength > 0.0f);

    m_bAnnotationsDirty = true;
    SavePresetAnnotations();

    // Only when this IS the preset on screen. Setting a damp on some other
    // preset in the browser must not change what is currently rendering --
    // the same rule SetPresetCanvasMaxByFile follows above.
    const wchar_t* cur = wcsrchr(m_szCurrentPresetFile, L'\\');
    cur = cur ? (cur + 1) : m_szCurrentPresetFile;
    if (_wcsicmp(cur, filenameOnly) == 0)
        RefreshCurrentDampStrength();
}

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
        // Emitted only when the flag is set, exactly like the slots above:
        // writing a bare 0 would turn every entry's "inherit" into an
        // explicit "no limit", which is a different state.
        if (a.hasCanvasMax)
            ss << L"      \"canvasMax\": " << a.canvasMax << L",\n";
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
// GetAnnotationForPreset — hash first, filename second
//
// Content identity is the primary key so a preset keeps its settings when it
// is moved or renamed; the filename is the fallback for an entry that has no
// hash yet. The point of having ONE of these is that a writer and a reader
// cannot pick different entries for the same preset.
//----------------------------------------------------------------------

PresetAnnotation* Engine::GetAnnotationForPreset(const wchar_t* filename,
                                                 CState* pState, bool create)
{
    // Scratch files have no identity -- not by hash, not by name. See
    // IsScratchPreset.
    if (IsScratchPreset(filename)) return nullptr;

    if (pState) {
        const char* h = pState->m_szPresetHash;
        std::wstring wHash(h, h + strlen(h));
        if (PresetAnnotation* a = GetAnnotationByHash(wHash)) return a;
    }
    return GetAnnotation(filename, create);
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

    PresetAnnotation* a = GetAnnotationForPreset(
        m_szLoadingPreset[0] ? m_szLoadingPreset : m_szCurrentPresetFile,
        pState, false);

    std::wstring name;
    if (a && a->hasAudioProfile) {
        name = a->audioProfile;              // may be empty: explicitly default
        m_resolvedAudioSource = OverrideSource::Preset;
    } else {
        const ShaderRule* rule = nullptr;
        if (a && !a->tags.empty())
            rule = ShaderOverrides().ResolveRule(a->tags, nullptr);
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

    wcscpy_s(m_szPendingAudioProfile, name.c_str());
    m_bAudioProfilePending.store(true, std::memory_order_release);

    DLOG_INFO("Audio profile: '%ls' (src %d, annot '%ls')", name.c_str(),
              (int)m_resolvedAudioSource, a ? a->filename.c_str() : L"(none)");
}

//----------------------------------------------------------------------
// ApplyPendingAudioProfile — render thread; the only writer of m_audioProfile
//----------------------------------------------------------------------

void Engine::ApplyPendingAudioProfile()
{
    if (!m_bAudioProfilePending.exchange(false, std::memory_order_acq_rel))
        return;

    AudioProfile next = AudioProfileStore::Defaults();
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
    if (!into.hasAudioProfile && from.hasAudioProfile) {
        into.hasAudioProfile = true;
        into.audioProfile = from.audioProfile;
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

PresetAnnotation* Engine::ResolveAnnotation(const wchar_t* filename, const wchar_t* hash,
                                            const wchar_t* fullPath, bool create)
{
    const std::wstring key = BareFilename(filename);
    if (key.empty()) return nullptr;
    const std::wstring wHash = hash ? hash : L"";

    // A scratch copy must not bind to the entry of the preset it was copied
    // from, by hash or by name. See IsScratchPreset.
    if (IsScratchPreset(fullPath)) return nullptr;

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
        auto [iter, ok] = m_presetAnnotations.emplace(key, std::move(fresh));
        a = &iter->second;
        if (!wHash.empty()) m_annotationsByHash[wHash] = a->filename;
        m_bAnnotationsDirty = true;
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

    for (auto& kv : m_presetAnnotations) {
        PresetAnnotation& a = kv.second;
        if (a.hash.empty()) continue;   // nothing to check the aliases against

        std::vector<std::wstring> keep;
        keep.reserve(a.paths.size());
        for (const std::wstring& path : a.paths) {
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
            if (wh == a.hash) {
                keep.push_back(path);
            } else {
                ++removed;
                DLOG_WARN("RepairAnnotationPaths: a recorded path hashes to a "
                          "different preset than the entry holding it -- removed");
            }
        }
        if (keep.size() != a.paths.size()) {
            a.paths.swap(keep);
            m_bAnnotationsDirty = true;
        }
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

    PresetAnnotation* a = GetAnnotation(filename, true);
    if (!a) return;
    a->flags |= PFLAG_ERROR;
    a->errorText = errorMsg;
    a->errorTime = IsoLocalNow();
    a->errorKind = kind;
    m_bAnnotationsDirty = true;
    SavePresetAnnotations();
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
    PresetAnnotation* a = GetAnnotation(filename, false);
    if (!a) return;
    if (a->errorKind != PresetErrorKind::Shader) return;
    if (a->errorText.empty() && !(a->flags & PFLAG_ERROR)) return;

    // The bare flag is retracted along with the text because AutoFlagPresetError
    // is the only thing that sets PFLAG_ERROR -- nothing lets a person mark a
    // preset as broken by hand.  A textless flag therefore came from an import
    // merging another machine's flags, which a local clean compile does answer.
    // If a manual "mark as error" is ever added, this must stop clearing flags
    // it cannot see evidence for.

    a->flags &= ~PFLAG_ERROR;
    a->errorText.clear();
    a->errorTime.clear();
    m_bAnnotationsDirty = true;
    SavePresetAnnotations();
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
        if (item.has(L"canvasMax")) {
            a.hasCanvasMax = true;
            a.canvasMax = item[L"canvasMax"].asInt();
        }
        if (item.has(L"feedbackDamp")) {
            a.hasFeedbackDamp = true;
            a.feedbackDamp = item[L"feedbackDamp"].asFloat();
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

void Engine::SetPresetAudioProfile(const wchar_t* filename,
                                   const std::wstring& name, bool present)
{
    // Through the same lookup the resolver uses. Writing by filename while
    // reading by hash puts the value on one entry and looks for it on another:
    // SOLID_blue.milk's hash resolves to SOLID_blend.milk2, the .milk2 wrapper
    // that contains it, so the setting was stored and then never found.
    PresetAnnotation* a = GetAnnotationForPreset(filename, m_pState, true);
    if (!a) return;
    a->hasAudioProfile = present;
    a->audioProfile = present ? name : std::wstring();
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
