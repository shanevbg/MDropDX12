// config_store.cpp — implementation of the settings facade. See config_store.h
// for why it exists; this file is about how it works.
//
// The only code in MDropDX12 allowed to call the Win32 profile API directly.
//
// Layout of this file:
//   1. process-wide state (the registry, the shield, the counters)
//   2. the background flusher
//   3. reads   -- served from the in-memory mirror, filled on demand
//   4. writes  -- update the mirror, mark dirty, return
//   5. flushing -- the only place a file is written

#include "config_store.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <locale.h>
#include <memory>
#include <stdio.h>
#include <string.h>
#include <thread>
#include <wchar.h>
#include <wctype.h>

#include "utility.h"

namespace mdrop {

// ===========================================================================
// 1. Process-wide state
// ===========================================================================

namespace {

// How stale the storage-changed check may be. Asking the backend on every
// single read would cost a syscall per setting; a fraction of a second late is
// fine for noticing something outside the process rewrote the file.
constexpr ULONGLONG kRevisionCheckIntervalMs = 500;

std::mutex& RegistryMutex() {
  static std::mutex m;
  return m;
}

std::map<std::wstring, std::unique_ptr<ConfigStore>>& Registry() {
  static std::map<std::wstring, std::unique_ptr<ConfigStore>> r;
  return r;
}

// The path Config() resolves to. Set once by SetConfigPath().
std::wstring& MainPath() {
  static std::wstring p;
  return p;
}

std::atomic<bool> g_shield{false};

// Thread-scoped, deliberately: a tool-window thread that opens an override for
// a Save button must not also let the render thread's incidental writes past.
thread_local int t_overrideDepth = 0;

std::atomic<long long> g_reads{0};
std::atomic<long long> g_writes{0};
std::atomic<long long> g_sets{0};
std::atomic<long long> g_elided{0};
std::atomic<long long> g_shielded{0};
std::atomic<long long> g_flushes{0};

// Registry key for a path. Two spellings of the same file must land on the same
// store, or their mirrors would disagree and each would serve the other's stale
// values.
//
// Only absolute paths are canonicalised. A *relative* path means something
// different to the profile API than it does to GetFullPathNameW -- Win32
// resolves it against the Windows directory, not the working directory -- so
// collapsing one would be wrong. MDropDX12 always uses absolute paths; this is
// belt and braces.
std::wstring RegistryKey(const std::wstring& path) {
  std::wstring key = path;
  const bool absolute =
      (path.size() >= 2 && path[1] == L':') ||
      (path.size() >= 2 && (path[0] == L'\\' || path[0] == L'/') &&
                           (path[1] == L'\\' || path[1] == L'/'));
  if (absolute) {
    wchar_t full[1024];
    const DWORD n = GetFullPathNameW(path.c_str(), 1024, full, nullptr);
    if (n > 0 && n < 1024) key.assign(full);
  }
  for (wchar_t& c : key) c = (wchar_t)towlower(c);
  return key;
}

std::wstring Widen(const char* s) {
  if (!s || !*s) return std::wstring();
  const int n = MultiByteToWideChar(CP_ACP, 0, s, -1, nullptr, 0);
  if (n <= 0) return std::wstring();
  std::wstring out((size_t)n - 1, L'\0');
  MultiByteToWideChar(CP_ACP, 0, s, -1, &out[0], n);
  return out;
}

}  // namespace

// ===========================================================================
// 2. The background flusher
// ===========================================================================
//
// One thread for the whole process. It wakes about once a second, writes out
// whatever has gone dirty since it last looked, and goes back to sleep. That is
// the entire buffering mechanism: no per-store timers, no work queue, and no
// caller ever waits on a file.

namespace {

std::mutex g_flushMutex;
std::condition_variable g_flushWake;
std::atomic<bool> g_flushStop{false};
std::once_flag g_flushOnce;
// Deliberately leaked. Joining it from a static destructor would race the
// runtime's own teardown; ConfigShutdown() joins it at a moment we choose.
std::thread* g_flushThread = nullptr;

void FlusherLoop() {
  std::unique_lock<std::mutex> lock(g_flushMutex);
  while (!g_flushStop.load()) {
    g_flushWake.wait_for(lock, std::chrono::milliseconds(kConfigFlushIntervalMs));
    if (g_flushStop.load()) break;
    lock.unlock();
    ConfigFlushAll();
    lock.lock();
  }
}

void StartFlusher() {
  std::call_once(g_flushOnce, [] {
    g_flushThread = new std::thread(FlusherLoop);
  });
}

}  // namespace

void ConfigFlushAll() {
  // Copy the store pointers out before flushing, so a flush cannot hold the
  // registry lock while it touches the disk -- ConfigFile() is called from
  // every thread in the app.
  std::vector<ConfigStore*> stores;
  {
    std::lock_guard<std::mutex> lock(RegistryMutex());
    stores.reserve(Registry().size());
    for (auto& entry : Registry()) stores.push_back(entry.second.get());
  }
  for (ConfigStore* store : stores) {
    if (store->IsDirty()) store->Flush();
  }
}

void ConfigShutdown() {
  g_flushStop.store(true);
  g_flushWake.notify_all();
  if (g_flushThread) {
    if (g_flushThread->joinable()) g_flushThread->join();
    // The thread object itself is never deleted; see the declaration.
  }
  ConfigFlushAll();
  const ConfigStats s = ConfigDiagnostics();
  DLOG_INFO("ConfigStore: shutdown -- %lld sets became %lld file writes "
            "(%lld elided, %lld held back by testing mode) over %lld flushes",
            s.sets, s.writes, s.elided, s.shielded, s.flushes);
}

// ===========================================================================
// Construction and the registry
// ===========================================================================

ConfigStore::ConfigStore(std::wstring path)
    : m_path(std::move(path)),
      m_backend(m_path.empty() ? nullptr : MakeConfigBackend(m_path)) {}

ConfigStore& ConfigFile(const wchar_t* path) {
  // A store with no path swallows writes and answers every read with the
  // fallback. Better than a null dereference for the handful of call sites that
  // can run before the config path is known.
  static ConfigStore nowhere(L"");
  if (!path || !*path) return nowhere;
  const std::wstring spelling(path);
  const std::wstring key = RegistryKey(spelling);
  std::lock_guard<std::mutex> lock(RegistryMutex());
  auto& registry = Registry();
  auto it = registry.find(key);
  if (it == registry.end())
    it = registry.emplace(key, std::make_unique<ConfigStore>(spelling)).first;
  return *it->second;
}

ConfigStore& ConfigFile(const std::wstring& path) { return ConfigFile(path.c_str()); }

ConfigStore& ConfigFile(const char* path) { return ConfigFile(Widen(path).c_str()); }

ConfigStore& Config() { return ConfigFile(MainPath().c_str()); }

void SetConfigPath(const wchar_t* path) {
  if (!path || !*path) return;
  std::lock_guard<std::mutex> lock(RegistryMutex());
  MainPath().assign(path);
}

// ===========================================================================
// The write shield
// ===========================================================================

void SetConfigWriteShield(bool on) {
  if (on) {
    // Anything already pending belongs to the user, not to the test about to
    // start. Get it onto disk before the shield goes up, or it would sit in the
    // cache for the length of the run and be written afterwards as if the test
    // had asked for it.
    ConfigFlushAll();
  }
  const bool was = g_shield.exchange(on);
  if (was == on) return;

  if (!on) {
    // Coming down: throw the overlay away. Everything a test wrote evaporates
    // and the app is back on the user's real configuration, with nothing to
    // undo and no chance of a half-restored state.
    std::lock_guard<std::mutex> lock(RegistryMutex());
    for (auto& entry : Registry()) {
      ConfigStore& store = *entry.second;
      std::lock_guard<std::recursive_mutex> storeLock(store.m_mutex);
      store.m_shadow.clear();
      store.m_shadowRemovedSections.clear();
    }
  }
  DLOG_INFO("ConfigStore: write shield %s (%lld writes diverted so far)",
            on ? "engaged" : "released", (long long)g_shielded.load());
}

bool IsConfigWriteShielded() { return g_shield.load(); }

ConfigWriteOverride::ConfigWriteOverride() { ++t_overrideDepth; }
ConfigWriteOverride::~ConfigWriteOverride() { --t_overrideDepth; }

bool IsConfigWriteOverridden() { return t_overrideDepth > 0; }

ConfigStats ConfigDiagnostics() {
  ConfigStats s;
  s.reads = g_reads.load();
  s.writes = g_writes.load();
  s.sets = g_sets.load();
  s.elided = g_elided.load();
  s.shielded = g_shielded.load();
  s.flushes = g_flushes.load();
  return s;
}

void ResetConfigDiagnostics() {
  g_reads.store(0);
  g_writes.store(0);
  g_sets.store(0);
  g_elided.store(0);
  g_shielded.store(0);
  g_flushes.store(0);
}

// ===========================================================================
// 3. Reads
// ===========================================================================

void ConfigStore::ForgetIfStorageChanged() const {
  if (!m_backend) return;
  const ULONGLONG now = GetTickCount64();
  if (m_revision.supported && now - m_lastRevisionCheck < kRevisionCheckIntervalMs)
    return;
  m_lastRevisionCheck = now;

  const ConfigRevision current = m_backend->Revision();
  // A backend that cannot tell -- the registry -- never invalidates. Nothing
  // outside this process edits those keys while it runs.
  if (!current.supported || current == m_revision) return;

  // The storage is not what we mirrored. Something else wrote it: an export
  // dialog pointed at the same path, the user edited it, it was deleted. Drop
  // what we think we know, but never drop a pending write -- that would lose a
  // change the app has already told its caller succeeded.
  for (auto sit = m_entries.begin(); sit != m_entries.end(); ) {
    for (auto kit = sit->second.begin(); kit != sit->second.end(); ) {
      if (kit->second.dirty) ++kit;
      else kit = sit->second.erase(kit);
    }
    if (sit->second.empty()) sit = m_entries.erase(sit);
    else ++sit;
  }
  m_revision = current;
}

bool ConfigStore::LookupShadow(const wchar_t* section, const wchar_t* key,
                               Entry* out) const {
  if (!g_shield.load()) return false;
  auto sit = m_shadow.find(section);
  if (sit != m_shadow.end()) {
    auto kit = sit->second.find(key);
    if (kit != sit->second.end()) {
      if (out) *out = kit->second;
      return true;
    }
  }
  // A section deleted under the shield hides everything the file still has.
  if (m_shadowRemovedSections.count(section)) {
    if (out) *out = Entry();
    return true;
  }
  return false;
}

// The heart of the read path: return the mirror's entry for a key, filling it
// from the file the first time it is asked for. Caller holds m_mutex.
const ConfigStore::Entry& ConfigStore::Resolve(const wchar_t* section,
                                               const wchar_t* key) const {
  ForgetIfStorageChanged();
  Entry& entry = m_entries[section][key];
  if (!entry.known) {
    entry.present = m_backend && m_backend->Read(section, key, &entry.value);
    entry.known = true;
    ++g_reads;
    if (!m_revision.supported && m_backend) m_revision = m_backend->Revision();
  }
  return entry;
}

std::wstring ConfigStore::GetString(const wchar_t* section, const wchar_t* key,
                                    const wchar_t* fallback) const {
  if (!section || !key) return std::wstring(fallback ? fallback : L"");
  std::lock_guard<std::recursive_mutex> lock(m_mutex);
  Entry shadowed;
  if (LookupShadow(section, key, &shadowed))
    return shadowed.present ? shadowed.value : std::wstring(fallback ? fallback : L"");
  const Entry& entry = Resolve(section, key);
  return entry.present ? entry.value : std::wstring(fallback ? fallback : L"");
}

DWORD ConfigStore::GetStringTo(const wchar_t* section, const wchar_t* key,
                               const wchar_t* fallback, wchar_t* out,
                               DWORD cchOut) const {
  if (!out || cchOut == 0) return 0;
  // GetString copies before we touch `out`, which matters: 177 call sites pass
  // the destination as its own fallback, so the two alias.
  const std::wstring value = GetString(section, key, fallback);
  DWORD copied = (DWORD)value.size();
  if (copied > cchOut - 1) copied = cchOut - 1;
  if (copied) memcpy(out, value.c_str(), copied * sizeof(wchar_t));
  out[copied] = L'\0';
  return copied;
}

int ConfigStore::GetInt(const wchar_t* section, const wchar_t* key,
                        int fallback) const {
  // GetPrivateProfileIntW is DOCUMENTED to report zero for a negative value in
  // the file. It does not: it returns a UINT whose bit pattern is the negative
  // number, which every caller here assigns straight into an int and gets the
  // number back. Measured, not assumed -- -1680 in a file reads back as
  // 4294965616, which is -1680 as an int.
  //
  // So this returns the number that was written, and there is no second
  // "signed" accessor pretending otherwise. Callers that want a coordinate get
  // one; callers that want a count are not surprised.
  return GetSignedInt(section, key, fallback);
}

// The number that was actually written, negatives included.
//
// Window coordinates need this: a monitor placed left of or above the primary
// one gives windows negative coordinates, and saving x=-1800 only to read back
// 0 is how a window on the left-hand screen jumps to the primary on restart.
int ConfigStore::GetSignedInt(const wchar_t* section, const wchar_t* key,
                              int fallback) const {
  if (!section || !key) return fallback;
  std::lock_guard<std::recursive_mutex> lock(m_mutex);
  Entry shadowed;
  if (LookupShadow(section, key, &shadowed))
    return shadowed.present ? _wtoi(shadowed.value.c_str()) : fallback;
  const Entry& entry = Resolve(section, key);
  return entry.present ? _wtoi(entry.value.c_str()) : fallback;
}

bool ConfigStore::GetBool(const wchar_t* section, const wchar_t* key,
                          bool fallback) const {
  return GetInt(section, key, fallback ? 1 : 0) != 0;
}

float ConfigStore::GetFloat(const wchar_t* section, const wchar_t* key,
                            float fallback) const {
  const std::wstring text = GetString(section, key, nullptr);
  if (text.empty()) return fallback;
  float parsed = fallback;
  // The C locale, always: a machine set to a comma decimal separator would
  // otherwise write "1,500000" and read back 1.
  if (_swscanf_l(text.c_str(), L"%f", g_use_C_locale, &parsed) != 1) return fallback;
  return parsed;
}

bool ConfigStore::Has(const wchar_t* section, const wchar_t* key) const {
  if (!section || !key) return false;
  std::lock_guard<std::recursive_mutex> lock(m_mutex);
  Entry shadowed;
  if (LookupShadow(section, key, &shadowed)) return shadowed.present;
  return Resolve(section, key).present;
}

DWORD ConfigStore::GetSectionNamesTo(wchar_t* out, DWORD cchOut) {
  if (!out || cchOut == 0 || !m_backend) return 0;
  Flush();  // the caller is about to read structure our pending writes change
  std::lock_guard<std::recursive_mutex> lock(m_mutex);
  ++g_reads;
  // The caller wants what GetPrivateProfileSectionNamesW hands back: names run
  // together, each null-terminated, the lot ending in a second null.
  //
  // The shield's overlay is keyed by section and key, so a section that exists
  // only in the overlay is invisible here -- acceptable, because the only
  // caller is the sprite loader, walking a file testing mode has no reason to
  // touch.
  DWORD written = 0;
  for (const std::wstring& name : m_backend->SectionNames()) {
    const DWORD need = (DWORD)name.size() + 1;
    if (written + need + 1 > cchOut) break;
    memcpy(out + written, name.c_str(), need * sizeof(wchar_t));
    written += need;
  }
  out[written] = L'\0';
  return written;
}

DWORD ConfigStore::GetStringToA(const char* section, const char* key,
                                const char* fallback, char* out,
                                DWORD cchOut) const {
  if (!out || cchOut == 0) return 0;
  // Go through the wide path so the narrow calls see the same mirror, the same
  // pending writes and the same overlay as everything else.
  const std::wstring value = GetString(Widen(section).c_str(), Widen(key).c_str(),
                                       Widen(fallback).c_str());
  const int n = WideCharToMultiByte(CP_ACP, 0, value.c_str(), -1, out, (int)cchOut,
                                    nullptr, nullptr);
  if (n <= 0) { out[0] = '\0'; return 0; }
  return (DWORD)(n - 1);
}

// ===========================================================================
// 4. Writes
// ===========================================================================

bool ConfigStore::Store(const wchar_t* section, const wchar_t* key,
                        const wchar_t* value) {
  if (!section || !key || !m_backend) return false;
  ++g_sets;
  std::lock_guard<std::recursive_mutex> lock(m_mutex);

  if (g_shield.load() && t_overrideDepth == 0) {
    Entry& slot = m_shadow[section][key];
    slot.present = (value != nullptr);
    slot.value = value ? value : L"";
    slot.known = true;
    ++g_shielded;
    return true;
  }

  // Resolve first: a write of the value the file already holds is not a write.
  const Entry& current = Resolve(section, key);
  const bool wantPresent = (value != nullptr);
  if (current.present == wantPresent &&
      (!wantPresent || current.value == value)) {
    ++g_elided;
    return true;
  }

  Entry& slot = m_entries[section][key];
  slot.present = wantPresent;
  slot.value = wantPresent ? value : L"";
  if (!slot.dirty) {
    slot.dirty = true;
    ++m_dirtyKeys;
  }
  StartFlusher();
  return true;
}

bool ConfigStore::SetString(const wchar_t* section, const wchar_t* key,
                            const wchar_t* value) {
  // A null value deletes the key, matching the Win32 convention. Several call
  // sites pass a pointer that is legitimately null sometimes.
  return Store(section, key, value);
}

bool ConfigStore::SetInt(const wchar_t* section, const wchar_t* key, int value) {
  wchar_t text[32];
  swprintf(text, 32, L"%d", value);
  return Store(section, key, text);
}

bool ConfigStore::SetBool(const wchar_t* section, const wchar_t* key, bool value) {
  return Store(section, key, value ? L"1" : L"0");
}

bool ConfigStore::SetFloat(const wchar_t* section, const wchar_t* key, float value) {
  wchar_t text[64];
  // "%f" in the C locale, byte for byte what the old WritePrivateProfileFloatW
  // produced, so upgrading does not rewrite every float in the user's file.
  _swprintf_l(text, L"%f", g_use_C_locale, value);
  return Store(section, key, text);
}

bool ConfigStore::Remove(const wchar_t* section, const wchar_t* key) {
  return Store(section, key, nullptr);
}

bool ConfigStore::RemoveSection(const wchar_t* section) {
  if (!section || !m_backend) return false;
  ++g_sets;
  std::lock_guard<std::recursive_mutex> lock(m_mutex);

  if (g_shield.load() && t_overrideDepth == 0) {
    m_shadow.erase(section);
    m_shadowRemovedSections.insert(section);
    ++g_shielded;
    return true;
  }

  // Pending writes into this section would otherwise be flushed after the
  // delete and resurrect it.
  auto sit = m_entries.find(section);
  if (sit != m_entries.end()) {
    for (auto& kv : sit->second)
      if (kv.second.dirty) { kv.second.dirty = false; --m_dirtyKeys; }
    m_entries.erase(sit);
  }

  const bool ok = m_backend->DeleteSection(section);
  ++g_writes;
  m_revision = m_backend->Revision();
  return ok;
}

bool ConfigStore::SetStringA(const char* section, const char* key, const char* value) {
  return Store(Widen(section).c_str(), Widen(key).c_str(),
               value ? Widen(value).c_str() : nullptr);
}

// ===========================================================================
// 5. Flushing — the only place a file is written
// ===========================================================================

bool ConfigStore::IsDirty() const {
  std::lock_guard<std::recursive_mutex> lock(m_mutex);
  return m_dirtyKeys > 0;
}

void ConfigStore::FlushLocked() {
  if (m_dirtyKeys == 0 || !m_backend) return;

  int written = 0;
  for (auto& sectionPair : m_entries) {
    for (auto& keyPair : sectionPair.second) {
      Entry& entry = keyPair.second;
      if (!entry.dirty) continue;
      if (m_backend->Write(sectionPair.first.c_str(), keyPair.first.c_str(),
                           entry.present ? entry.value.c_str() : nullptr)) {
        entry.dirty = false;
        --m_dirtyKeys;
        ++written;
      }
    }
  }
  if (written == 0) return;

  // Windows buffers .ini writes; Commit pushes them out so the storage really
  // is current when we say it is.
  m_backend->Commit();
  g_writes += written;
  ++g_flushes;

  // Re-read the revision so our own write is not mistaken for somebody else's
  // edit on the next read.
  m_revision = m_backend->Revision();
  m_lastRevisionCheck = GetTickCount64();
}

void ConfigStore::Flush() {
  std::lock_guard<std::recursive_mutex> lock(m_mutex);
  FlushLocked();
}

void ConfigStore::Forget() {
  std::lock_guard<std::recursive_mutex> lock(m_mutex);
  FlushLocked();  // never lose a pending change to a cache drop
  m_entries.clear();
  m_dirtyKeys = 0;
  m_revision = ConfigRevision();
}

}  // namespace mdrop
