// config_backend.cpp — the two places settings can live. See config_backend.h
// for the why; this file is the how.
//
//   IniFileBackend   -- the Win32 profile API, the way MilkDrop always did it
//   RegistryBackend  -- HKEY_CURRENT_USER\Software\MDropDX12\<name>\<section>
//
// Both are dumb on purpose. Caching, coalescing and the testing-mode shield all
// live in ConfigStore; a backend only reads, writes, deletes and enumerates.

#include "config_backend.h"

#include <map>
#include <mutex>

#include "utility.h"

namespace mdrop {

namespace {

// Where the registry backend puts everything.
const wchar_t kRegistryRoot[] = L"Software\\MDropDX12";

// The file that decides which backend is used.
const wchar_t kFlagFileName[] = L"useregistry.ini";

std::mutex g_mutex;
ConfigBackendKind g_kind = ConfigBackendKind::IniFile;

// path (lowercased) -> short name, for the app's own configuration files.
std::map<std::wstring, std::wstring>& ManagedFiles() {
  static std::map<std::wstring, std::wstring> m;
  return m;
}

std::wstring Lower(std::wstring s) {
  for (wchar_t& c : s) c = (wchar_t)towlower(c);
  return s;
}

// Registry key names cannot contain a backslash. No section in this app does,
// but a section name is sometimes built from data, so make it impossible for a
// stray one to create a nested key by accident.
std::wstring SafeKeyName(const wchar_t* section) {
  std::wstring out(section ? section : L"");
  for (wchar_t& c : out)
    if (c == L'\\' || c == L'/') c = L'_';
  return out;
}

// ===========================================================================
// The .ini file backend
// ===========================================================================

class IniFileBackend : public ConfigBackend {
 public:
  explicit IniFileBackend(std::wstring path)
      : m_path(std::move(path)), m_describe(m_path) {}

  bool Read(const wchar_t* section, const wchar_t* key,
            std::wstring* out) const override {
    // A value no .ini will ever hold, so "missing" and "present but empty" can
    // be told apart. The profile API offers no other way to ask.
    static const wchar_t kAbsent[] = L"\x01\x02mdrop:absent\x02\x01";
    std::vector<wchar_t> buf(512);
    for (;;) {
      const DWORD n = GetPrivateProfileStringW(section, key, kAbsent, buf.data(),
                                               (DWORD)buf.size(), m_path.c_str());
      // A truncated copy is reported as buffer-size-minus-one. Grow rather than
      // lose the tail of a long value; preset lists get long.
      if (n == (DWORD)buf.size() - 1 && buf.size() < (1u << 20)) {
        buf.resize(buf.size() * 4);
        continue;
      }
      break;
    }
    if (wcscmp(buf.data(), kAbsent) == 0) return false;
    if (out) out->assign(buf.data());
    return true;
  }

  bool Write(const wchar_t* section, const wchar_t* key,
             const wchar_t* value) override {
    return WritePrivateProfileStringW(section, key, value, m_path.c_str()) != FALSE;
  }

  bool DeleteSection(const wchar_t* section) override {
    return WritePrivateProfileStringW(section, nullptr, nullptr,
                                      m_path.c_str()) != FALSE;
  }

  std::vector<std::wstring> SectionNames() const override {
    std::vector<std::wstring> names;
    std::vector<wchar_t> buf(8192);
    DWORD n = GetPrivateProfileSectionNamesW(buf.data(), (DWORD)buf.size(),
                                             m_path.c_str());
    if (n == (DWORD)buf.size() - 2) {
      buf.resize(65536);
      n = GetPrivateProfileSectionNamesW(buf.data(), (DWORD)buf.size(),
                                         m_path.c_str());
    }
    for (const wchar_t* p = buf.data(); p < buf.data() + n && *p; p += wcslen(p) + 1)
      names.emplace_back(p);
    return names;
  }

  void Commit() override {
    // Windows buffers .ini writes; this is the documented way to push them out.
    WritePrivateProfileStringW(nullptr, nullptr, nullptr, m_path.c_str());
  }

  ConfigRevision Revision() const override {
    ConfigRevision rev;
    WIN32_FILE_ATTRIBUTE_DATA fad = {};
    if (GetFileAttributesExW(m_path.c_str(), GetFileExInfoStandard, &fad)) {
      rev.a = ((unsigned long long)fad.ftLastWriteTime.dwHighDateTime << 32) |
              fad.ftLastWriteTime.dwLowDateTime;
      rev.b = fad.nFileSizeLow;
    }
    rev.supported = true;   // "no file" is a perfectly good revision: 0/0
    return rev;
  }

  const std::wstring& Describe() const override { return m_describe; }

 private:
  std::wstring m_path;
  std::wstring m_describe;
};

// ===========================================================================
// The registry backend
// ===========================================================================
//
// The mapping is the obvious one, which is what makes it readable in regedit:
//
//     settings.ini   [Settings] bShowFPS=1
//     HKCU\Software\MDropDX12\settings\Settings    bShowFPS  (REG_SZ) "1"
//
// Values are REG_SZ throughout, even for numbers, so the two backends store the
// identical text and a value copied either way round survives unchanged.

class RegistryBackend : public ConfigBackend {
 public:
  explicit RegistryBackend(std::wstring name)
      : m_base(std::wstring(kRegistryRoot) + L"\\" + name),
        m_describe(L"HKCU\\" + m_base) {}

  bool Read(const wchar_t* section, const wchar_t* key,
            std::wstring* out) const override {
    HKEY hKey = OpenSection(section, KEY_READ);
    if (!hKey) return false;
    DWORD type = 0, cb = 0;
    LSTATUS st = RegQueryValueExW(hKey, key, nullptr, &type, nullptr, &cb);
    if (st != ERROR_SUCCESS || (type != REG_SZ && type != REG_EXPAND_SZ)) {
      RegCloseKey(hKey);
      return false;
    }
    std::wstring value((cb / sizeof(wchar_t)) + 1, L'\0');
    st = RegQueryValueExW(hKey, key, nullptr, &type, (LPBYTE)&value[0], &cb);
    RegCloseKey(hKey);
    if (st != ERROR_SUCCESS) return false;
    value.resize(wcslen(value.c_str()));
    if (out) *out = value;
    return true;
  }

  bool Write(const wchar_t* section, const wchar_t* key,
             const wchar_t* value) override {
    if (!value) {
      HKEY hKey = OpenSection(section, KEY_SET_VALUE);
      if (!hKey) return true;              // nothing there to delete
      const LSTATUS st = RegDeleteValueW(hKey, key);
      RegCloseKey(hKey);
      return st == ERROR_SUCCESS || st == ERROR_FILE_NOT_FOUND;
    }
    HKEY hKey = nullptr;
    const std::wstring path = m_base + L"\\" + SafeKeyName(section);
    if (RegCreateKeyExW(HKEY_CURRENT_USER, path.c_str(), 0, nullptr,
                        REG_OPTION_NON_VOLATILE, KEY_SET_VALUE, nullptr,
                        &hKey, nullptr) != ERROR_SUCCESS)
      return false;
    const DWORD cb = (DWORD)((wcslen(value) + 1) * sizeof(wchar_t));
    const LSTATUS st = RegSetValueExW(hKey, key, 0, REG_SZ, (const BYTE*)value, cb);
    RegCloseKey(hKey);
    return st == ERROR_SUCCESS;
  }

  bool DeleteSection(const wchar_t* section) override {
    const std::wstring path = m_base + L"\\" + SafeKeyName(section);
    const LSTATUS st = RegDeleteTreeW(HKEY_CURRENT_USER, path.c_str());
    return st == ERROR_SUCCESS || st == ERROR_FILE_NOT_FOUND;
  }

  std::vector<std::wstring> SectionNames() const override {
    std::vector<std::wstring> names;
    HKEY hBase = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, m_base.c_str(), 0, KEY_READ,
                      &hBase) != ERROR_SUCCESS)
      return names;
    for (DWORD i = 0;; ++i) {
      wchar_t name[256];
      DWORD cch = 256;
      if (RegEnumKeyExW(hBase, i, name, &cch, nullptr, nullptr, nullptr,
                        nullptr) != ERROR_SUCCESS)
        break;
      names.emplace_back(name);
    }
    RegCloseKey(hBase);
    return names;
  }

  // Deliberately no Revision(): nothing outside this process edits these keys
  // while the app runs, and the registry offers no per-value change token worth
  // a syscall on every read. Reporting "cannot tell" leaves ConfigStore's
  // mirror in place, which is what we want.

  const std::wstring& Describe() const override { return m_describe; }

  bool Exists() const {
    HKEY hKey = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, m_base.c_str(), 0, KEY_READ,
                      &hKey) != ERROR_SUCCESS)
      return false;
    RegCloseKey(hKey);
    return true;
  }

  const std::wstring& BasePath() const { return m_base; }

 private:
  HKEY OpenSection(const wchar_t* section, REGSAM access) const {
    HKEY hKey = nullptr;
    const std::wstring path = m_base + L"\\" + SafeKeyName(section);
    if (RegOpenKeyExW(HKEY_CURRENT_USER, path.c_str(), 0, access,
                      &hKey) != ERROR_SUCCESS)
      return nullptr;
    return hKey;
  }

  std::wstring m_base;
  std::wstring m_describe;
};

// ===========================================================================
// Moving settings between the two
// ===========================================================================

// Every key=value pair in one .ini section, as the profile API hands them over:
// a run of null-terminated "key=value" strings ending in an extra null.
std::vector<std::pair<std::wstring, std::wstring>> ReadIniSection(
    const std::wstring& path, const std::wstring& section) {
  std::vector<std::pair<std::wstring, std::wstring>> pairs;
  std::vector<wchar_t> buf(16384);
  DWORD n = GetPrivateProfileSectionW(section.c_str(), buf.data(),
                                      (DWORD)buf.size(), path.c_str());
  if (n == (DWORD)buf.size() - 2) {
    buf.resize(262144);
    n = GetPrivateProfileSectionW(section.c_str(), buf.data(), (DWORD)buf.size(),
                                  path.c_str());
  }
  for (const wchar_t* p = buf.data(); p < buf.data() + n && *p; p += wcslen(p) + 1) {
    const wchar_t* eq = wcschr(p, L'=');
    if (!eq) continue;
    pairs.emplace_back(std::wstring(p, eq), std::wstring(eq + 1));
  }
  return pairs;
}

// First run on the registry: bring the .ini across so the user's configuration
// is not silently reset by flipping a flag.
void SeedRegistryFromIni(RegistryBackend& reg, const std::wstring& iniPath) {
  IniFileBackend ini(iniPath);
  int sections = 0, keys = 0;
  for (const std::wstring& section : ini.SectionNames()) {
    ++sections;
    for (const auto& kv : ReadIniSection(iniPath, section)) {
      reg.Write(section.c_str(), kv.first.c_str(), kv.second.c_str());
      ++keys;
    }
  }
  DLOG_WARN("ConfigBackend: seeded %ls from %ls (%d sections, %d keys)",
            reg.BasePath().c_str(), iniPath.c_str(), sections, keys);
}

}  // namespace

// ===========================================================================
// Selection
// ===========================================================================

void SetConfigBackend(ConfigBackendKind kind) {
  std::lock_guard<std::mutex> lock(g_mutex);
  g_kind = kind;
}

ConfigBackendKind GetConfigBackend() {
  std::lock_guard<std::mutex> lock(g_mutex);
  return g_kind;
}

void RegisterManagedConfigFile(const wchar_t* path, const wchar_t* name) {
  if (!path || !*path || !name || !*name) return;
  std::lock_guard<std::mutex> lock(g_mutex);
  ManagedFiles()[Lower(path)] = name;
}

void ConfigureBackendFromFlagFile(const wchar_t* dir) {
  if (!dir || !*dir) return;
  std::wstring flagFile = dir;
  if (!flagFile.empty() && flagFile.back() != L'\\' && flagFile.back() != L'/')
    flagFile += L'\\';
  flagFile += kFlagFileName;

  // The one direct call to the profile API left outside config_backend.cpp's
  // own backend: this file decides which backend exists, so it cannot be read
  // through one.
  const bool useRegistry =
      GetPrivateProfileIntW(L"Settings", L"UseRegistry", 0, flagFile.c_str()) != 0;

  if (GetFileAttributesW(flagFile.c_str()) == INVALID_FILE_ATTRIBUTES) {
    // Write the flag file out, commented, so the option is discoverable without
    // reading the manual. Best effort -- a read-only install just goes without.
    FILE* f = nullptr;
    if (_wfopen_s(&f, flagFile.c_str(), L"wt, ccs=UTF-8") == 0 && f) {
      fputws(L"; Where MDropDX12 keeps its settings.\n"
             L";\n"
             L";   UseRegistry=0  settings.ini, messages.ini and sprites.ini in\n"
             L";                  this folder. Portable: copy the folder and the\n"
             L";                  settings come with it. This is the default.\n"
             L";\n"
             L";   UseRegistry=1  HKEY_CURRENT_USER\\Software\\MDropDX12. Settings\n"
             L";                  follow the user instead of the folder, so they\n"
             L";                  survive replacing this directory with a new\n"
             L";                  build.\n"
             L";\n"
             L"; Switching to the registry copies the existing .ini files across\n"
             L"; the first time, so nothing is lost. The .ini files are left as\n"
             L"; they were; to bring the registry back into them afterwards,\n"
             L"; send CONFIG_EXPORT_INI over the named pipe.\n"
             L";\n"
             L"; Read once, at start-up. Restart MDropDX12 after changing it.\n"
             L"\n"
             L"[Settings]\n"
             L"UseRegistry=0\n", f);
      fclose(f);
    }
  }

  SetConfigBackend(useRegistry ? ConfigBackendKind::Registry
                               : ConfigBackendKind::IniFile);
  DLOG_WARN("ConfigBackend: settings will be kept in %ls",
            useRegistry ? L"the registry (HKCU\\Software\\MDropDX12)"
                        : L"the .ini files beside the executable");
}

std::unique_ptr<ConfigBackend> MakeConfigBackend(const std::wstring& path) {
  std::wstring name;
  {
    std::lock_guard<std::mutex> lock(g_mutex);
    if (g_kind == ConfigBackendKind::Registry) {
      auto it = ManagedFiles().find(Lower(path));
      if (it != ManagedFiles().end()) name = it->second;
    }
  }
  if (name.empty())
    return std::unique_ptr<ConfigBackend>(new IniFileBackend(path));

  auto reg = std::unique_ptr<RegistryBackend>(new RegistryBackend(name));
  if (!reg->Exists() && GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES)
    SeedRegistryFromIni(*reg, path);
  return reg;
}

bool ExportRegistryToIniFiles(std::wstring* summaryOut) {
  std::map<std::wstring, std::wstring> managed;
  {
    std::lock_guard<std::mutex> lock(g_mutex);
    managed = ManagedFiles();
  }
  int files = 0, keys = 0;
  for (const auto& entry : managed) {
    RegistryBackend reg(entry.second);
    if (!reg.Exists()) continue;
    IniFileBackend ini(entry.first);
    ++files;
    for (const std::wstring& section : reg.SectionNames()) {
      // Enumerate the section's values through the registry API directly; the
      // backend interface deliberately has no "list keys", because nothing in
      // the app needs it apart from this.
      HKEY hKey = nullptr;
      const std::wstring path =
          std::wstring(kRegistryRoot) + L"\\" + entry.second + L"\\" + section;
      if (RegOpenKeyExW(HKEY_CURRENT_USER, path.c_str(), 0, KEY_READ,
                        &hKey) != ERROR_SUCCESS)
        continue;
      for (DWORD i = 0;; ++i) {
        wchar_t valueName[512];
        DWORD cchName = 512;
        if (RegEnumValueW(hKey, i, valueName, &cchName, nullptr, nullptr, nullptr,
                          nullptr) != ERROR_SUCCESS)
          break;
        std::wstring value;
        if (reg.Read(section.c_str(), valueName, &value)) {
          ini.Write(section.c_str(), valueName, value.c_str());
          ++keys;
        }
      }
      RegCloseKey(hKey);
    }
    ini.Commit();
  }
  if (summaryOut) {
    wchar_t buf[256];
    swprintf_s(buf, L"%d file(s), %d key(s)", files, keys);
    summaryOut->assign(buf);
  }
  DLOG_WARN("ConfigBackend: exported the registry into %d .ini file(s), %d keys",
            files, keys);
  return files > 0;
}

}  // namespace mdrop
