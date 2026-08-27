// config_store.h — the one door every setting goes through.
//
// WHY THIS EXISTS
// ---------------
// MDropDX12 keeps its configuration in plain Win32 .ini files, and for a long
// time it talked to them the way the original MilkDrop plugin did: by calling
// GetPrivateProfileString / WritePrivateProfileString directly, from wherever
// the value happened to be needed. That worked, but it grew to roughly 900 call
// sites spread over 30 files, in three different argument orders:
//
//     WritePrivateProfileStringW(section, key, value, file)   // Win32 order
//     WritePrivateProfileIntW   (value, key, file, section)   // our helper -- backwards!
//     WritePrivateProfileFloatW (value, key, file, section)   // ...also backwards
//
// Two of those three read right-to-left. Getting them mixed up compiles cleanly
// and silently writes a key named after the value.
//
// More importantly, 900 doors mean no doorman. There was nowhere to stand to
// answer questions like "is this write allowed right now?", "have we already
// written exactly this?", or "can this wait a moment?".
//
// ConfigStore is that doorman. Everything else in the app now says:
//
//     Config().SetInt(L"Settings", L"bShowFPS", m_bShowFPS);
//     m_bShowFPS = Config().GetBool(L"Settings", L"bShowFPS", false);
//
// -- one argument order, section first, exactly like the file reads.
//
//
// IT IS A WRITE-BACK CACHE, NOT A WRAPPER
// ---------------------------------------
// This is the part that matters most, and the reason a previous attempt at
// unifying .ini access had to be abandoned.
//
// WritePrivateProfileString rewrites the ENTIRE file on every call. Code that
// changes a value repeatedly -- a slider being dragged, a window being moved, a
// loop of IPC commands -- therefore issued one whole-file rewrite per call, and
// the writes started blocking the thread that made them.
//
// So Set* does not touch the file. It updates the value in memory, marks it
// dirty, and returns. A background flusher pushes dirty keys out about once a
// second (kConfigFlushIntervalMs). A key written a hundred times inside that
// window costs exactly one file write, and no caller ever waits on disk.
//
// Reads are served from the same in-memory mirror, so a read after a write sees
// the new value immediately -- buffering is invisible to the rest of the app.
// The mirror is dropped whenever the file's timestamp or size changes underneath
// us, so an .ini edited by something else is still picked up.
//
// The buffer window is also a data-loss window. Keep it short, and flush at any
// boundary where the file has to be right on disk *now*:
//
//     ConfigFlushAll()   -- push every store, synchronously
//     ConfigShutdown()   -- flush and stop the flusher; call once, on the way out
//     store.Flush()      -- push one store, e.g. before another process reads it
//
//
// THE WRITE SHIELD
// ----------------
// Automated tests drive the app over its named pipe. Along the way they change
// settings -- that is usually the point of the test -- and those changes used to
// land in the user's real settings.ini, so a test run quietly rewrote the
// configuration of the machine it ran on.
//
// While the shield is engaged (Engine::SetTestingMode raises it) every write
// goes into a separate in-memory overlay instead of the cache:
//
//     * reads consult the overlay first, so code under test still sees what it
//       just wrote and behaves exactly as it would in normal operation;
//     * nothing is ever marked dirty, so nothing reaches disk;
//     * lowering the shield discards the overlay, so the app snaps back to the
//       user's real configuration without having to undo anything.
//
// Two deliberate ways out, because "no writes at all" is too blunt:
//
//     * ConfigWriteOverride -- an RAII guard for a write the user genuinely
//       asked for (they clicked Save, they typed a rating). Thread-scoped, so
//       one thread opting in never lets another thread's incidental writes
//       through.
//     * [Milkwave] TestingModeWritesSettings=1, or TESTING_MODE=1,persist over
//       the pipe, which tells the engine not to raise the shield at all.

#ifndef MDROP_CONFIG_STORE_H
#define MDROP_CONFIG_STORE_H 1

#include <windows.h>

#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <vector>

#include "config_backend.h"

namespace mdrop {

// How long a changed value may sit in memory before it is written out. Long
// enough that a dragged slider costs one write; short enough that killing the
// process loses at most a second of settings.
static constexpr unsigned kConfigFlushIntervalMs = 1000;

// Case-insensitive comparison for section and key names, because that is how
// the Win32 profile API treats them: [Settings] and [settings] are one section.
struct IniNameLess {
  bool operator()(const std::wstring& a, const std::wstring& b) const {
    return _wcsicmp(a.c_str(), b.c_str()) < 0;
  }
};

// Running totals, for diagnostics. Not per-store: the interesting question is
// always "what did the app do", not "what did this file do".
struct ConfigStats {
  long long reads = 0;      // values fetched from a file (a cache miss)
  long long writes = 0;     // keys actually written to a file by the flusher
  long long sets = 0;       // Set*/Remove calls made by the app
  long long elided = 0;     // sets that changed nothing and were dropped
  long long shielded = 0;   // sets diverted to the overlay by testing mode
  long long flushes = 0;    // flush passes that had something to do
};

// One .ini file. Get a reference from Config() or ConfigFile(); never construct
// one directly, so that two spellings of the same path cannot end up with two
// caches that disagree.
class ConfigStore {
 public:
  explicit ConfigStore(std::wstring path);

  const std::wstring& Path() const { return m_path; }

  // ---------------------------------------------------------------- reads --
  // Every read takes the value's fallback last, so the call reads like the
  // sentence it stands for: "the Settings section's bShowFPS, or false".

  // Matches GetPrivateProfileIntW exactly, including its quirk of reporting
  // zero for a negative value in the file. Use GetSignedInt when the number can
  // legitimately be negative.
  int GetInt(const wchar_t* section, const wchar_t* key, int fallback) const;

  // The number that was written, negatives included. Window coordinates.
  int GetSignedInt(const wchar_t* section, const wchar_t* key, int fallback) const;
  bool GetBool(const wchar_t* section, const wchar_t* key, bool fallback) const;
  float GetFloat(const wchar_t* section, const wchar_t* key, float fallback) const;
  std::wstring GetString(const wchar_t* section, const wchar_t* key,
                         const wchar_t* fallback = L"") const;

  // Fixed-buffer form, for the call sites that read straight into a member
  // array. Returns the character count written, excluding the terminator --
  // same as GetPrivateProfileStringW.
  DWORD GetStringTo(const wchar_t* section, const wchar_t* key,
                    const wchar_t* fallback, wchar_t* out, DWORD cchOut) const;

  // Same, taking the array itself so the count cannot be got wrong. Nine call
  // sites used to pass sizeof(buf), and sizeof a wchar_t array is twice its
  // length in characters -- an overflow waiting for a long enough value.
  template <size_t N>
  DWORD GetStringTo(const wchar_t* section, const wchar_t* key,
                    const wchar_t* fallback, wchar_t (&out)[N]) const {
    return GetStringTo(section, key, fallback, out, (DWORD)N);
  }

  // Is the key present in the file (or in the testing-mode overlay)?
  bool Has(const wchar_t* section, const wchar_t* key) const;

  // Section names, as a double-null-terminated block, exactly like
  // GetPrivateProfileSectionNamesW. Flushes first, because the caller is about
  // to read structure that pending writes may have changed.
  DWORD GetSectionNamesTo(wchar_t* out, DWORD cchOut);

  // --------------------------------------------------------------- writes --
  // These update memory and return; the flusher writes the file. A true means
  // the value is now what you asked for, not that it has reached disk.

  bool SetInt(const wchar_t* section, const wchar_t* key, int value);
  bool SetBool(const wchar_t* section, const wchar_t* key, bool value);
  bool SetFloat(const wchar_t* section, const wchar_t* key, float value);
  bool SetString(const wchar_t* section, const wchar_t* key, const wchar_t* value);
  bool SetString(const wchar_t* section, const wchar_t* key, const std::wstring& value) {
    return SetString(section, key, value.c_str());
  }

  // Delete a single key. (Passing a null value to SetString does the same
  // thing, matching the Win32 convention, because plenty of call sites pass a
  // pointer that is legitimately null sometimes.)
  bool Remove(const wchar_t* section, const wchar_t* key);

  // Delete a whole section. Synchronous -- it is rare, and routing it through
  // the dirty-key queue would mean ordering a delete against writes into the
  // same section.
  bool RemoveSection(const wchar_t* section);

  // Write everything pending for this file, now, and push the Win32 profile
  // cache out behind it. Use before handing the file to something else.
  void Flush();

  // ----------------------------------------------------------- ANSI bridge --
  // Two corners of the app still speak ANSI: the DXGI adapter records copied
  // from the original plugin, and the sprite .ini, whose values are arbitrary
  // bytes. Rather than convert those in place, they get narrow overloads that
  // land in the same store.

  DWORD GetStringToA(const char* section, const char* key, const char* fallback,
                     char* out, DWORD cchOut) const;
  bool SetStringA(const char* section, const char* key, const char* value);

  // ---------------------------------------------------------- maintenance --

  // Drop the in-memory mirror. Done for you when the file changes underneath
  // the store; call it by hand only if something edited the file in a way the
  // filesystem cannot show.
  void Forget();

  // Does this store have unwritten changes?
  bool IsDirty() const;

 private:
  // One key's state. `known` says the mirror has an answer for this key at all;
  // `present` says the answer is "the file has it"; `dirty` says the value here
  // has not reached the file yet.
  struct Entry {
    std::wstring value;
    bool present = false;
    bool known = false;
    bool dirty = false;
  };
  using Section = std::map<std::wstring, Entry, IniNameLess>;
  using Sections = std::map<std::wstring, Section, IniNameLess>;

  // All of these expect m_mutex to be held.
  bool LookupShadow(const wchar_t* section, const wchar_t* key, Entry* out) const;
  const Entry& Resolve(const wchar_t* section, const wchar_t* key) const;
  bool Store(const wchar_t* section, const wchar_t* key, const wchar_t* value);
  void FlushLocked();
  void ForgetIfStorageChanged() const;

  std::wstring m_path;
  // Where the values really are: an .ini file, or the registry. Chosen once, at
  // construction, by MakeConfigBackend().
  std::unique_ptr<ConfigBackend> m_backend;
  mutable std::recursive_mutex m_mutex;

  // The in-memory mirror of the storage, plus the pending changes to it.
  mutable Sections m_entries;
  mutable ConfigRevision m_revision;
  mutable ULONGLONG m_lastRevisionCheck = 0;
  int m_dirtyKeys = 0;

  // Where writes go while the shield is up. Both cleared when it comes down.
  Sections m_shadow;
  std::set<std::wstring, IniNameLess> m_shadowRemovedSections;

  friend void SetConfigWriteShield(bool on);
};

// --------------------------------------------------------------------------
// Getting at the stores
// --------------------------------------------------------------------------

// The application's own settings.ini.
ConfigStore& Config();

// Any other .ini file -- the message profiles, the sprite file, a profile the
// user chose in a file dialog. Paths are normalised, so two spellings of one
// file share one store.
ConfigStore& ConfigFile(const wchar_t* path);
ConfigStore& ConfigFile(const std::wstring& path);
ConfigStore& ConfigFile(const char* path);

// Point Config() at the real settings.ini. Call once, at start-up, before the
// first setting is read.
void SetConfigPath(const wchar_t* path);

// --------------------------------------------------------------------------
// Flushing
// --------------------------------------------------------------------------

// Write out every store's pending changes, synchronously.
void ConfigFlushAll();

// Final flush; stops the background flusher. Call once, on the way out.
void ConfigShutdown();

// --------------------------------------------------------------------------
// The write shield
// --------------------------------------------------------------------------

// Raise or lower the shield across every store. Lowering it discards
// everything written while it was up.
void SetConfigWriteShield(bool on);
bool IsConfigWriteShielded();

// Let one thread's writes through the shield for as long as the guard lives.
// Use it for a write the user explicitly asked for; do not use it to make an
// incidental write "important".
class ConfigWriteOverride {
 public:
  ConfigWriteOverride();
  ~ConfigWriteOverride();
  ConfigWriteOverride(const ConfigWriteOverride&) = delete;
  ConfigWriteOverride& operator=(const ConfigWriteOverride&) = delete;
};

// True when the calling thread currently holds a ConfigWriteOverride.
bool IsConfigWriteOverridden();

// Running totals since process start.
ConfigStats ConfigDiagnostics();
void ResetConfigDiagnostics();

}  // namespace mdrop

#endif  // MDROP_CONFIG_STORE_H
