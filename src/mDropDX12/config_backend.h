// config_backend.h — where settings actually live.
//
// ConfigStore (config_store.h) is the API the rest of the app uses: typed
// getters and setters, a write-back cache, a testing-mode shield. This file is
// the other half of that split -- the storage underneath it.
//
// There are two places a setting can be kept:
//
//   * an .ini FILE, which is what MilkDrop always used and what MDropDX12 ships
//     with. It is portable: the whole app is a folder you can copy, settings
//     included.
//
//   * the WINDOWS REGISTRY, under HKEY_CURRENT_USER. Settings then live with
//     the user rather than with the build directory, which is the point: a
//     rebuild replaces the output folder, and with it the .ini that was in it.
//
// The choice is made once, at start-up, by a single flag in `useregistry.ini`
// beside the executable:
//
//     [Settings]
//     UseRegistry=1
//
// A separate file on purpose. It has to be read before the settings system
// exists, so it cannot be a setting itself, and keeping it apart means the
// switch survives anything that happens to settings.ini.
//
// Only the app's OWN configuration files follow the flag -- settings.ini,
// messages.ini, sprites.ini, registered through RegisterManagedConfigFile().
// A file the user picked in a save dialog (an exported message profile, say) is
// always a real file, because the whole point of exporting one is to have a
// file to hand to somebody.
//
// Switching to the registry the first time COPIES the existing .ini across, so
// nothing is lost. The .ini is left alone afterwards, as a snapshot of the
// moment you switched; changes made while on the registry do not flow back into
// it unless you ask (CONFIG_EXPORT_INI over the pipe).

#ifndef MDROP_CONFIG_BACKEND_H
#define MDROP_CONFIG_BACKEND_H 1

#include <windows.h>

#include <memory>
#include <string>
#include <vector>

namespace mdrop {

// A cheap token for "has this storage changed since I last looked". The .ini
// backend uses the file's write time and size; the registry backend has no
// equivalent worth the syscall and reports that it cannot tell.
struct ConfigRevision {
  unsigned long long a = 0;
  unsigned long long b = 0;
  bool supported = false;
  bool operator==(const ConfigRevision& o) const {
    return supported == o.supported && a == o.a && b == o.b;
  }
  bool operator!=(const ConfigRevision& o) const { return !(*this == o); }
};

// One place settings can be stored. Deliberately small: everything clever --
// caching, coalescing, the testing-mode shield -- lives one level up in
// ConfigStore, so a new backend only has to answer five questions.
class ConfigBackend {
 public:
  virtual ~ConfigBackend() = default;

  // Fetch one value. False means "not there"; an empty string is a value.
  virtual bool Read(const wchar_t* section, const wchar_t* key,
                    std::wstring* out) const = 0;

  // Store one value, or delete it when `value` is null.
  virtual bool Write(const wchar_t* section, const wchar_t* key,
                     const wchar_t* value) = 0;

  virtual bool DeleteSection(const wchar_t* section) = 0;

  // Every section name, for the callers that treat sections as data.
  virtual std::vector<std::wstring> SectionNames() const = 0;

  // Push any host-side buffering out. Windows caches .ini writes; the registry
  // does not need it.
  virtual void Commit() {}

  virtual ConfigRevision Revision() const { return ConfigRevision(); }

  // For log lines and diagnostics: "settings.ini" or "HKCU:...\settings".
  virtual const std::wstring& Describe() const = 0;
};

// --------------------------------------------------------------------------
// Choosing a backend
// --------------------------------------------------------------------------

enum class ConfigBackendKind {
  IniFile,   // the .ini beside the executable (the default, and portable)
  Registry,  // HKEY_CURRENT_USER\Software\<root>
};

// Read `useregistry.ini` from `dir` and act on it. Call once, at start-up,
// before the first setting is touched. Creates the flag file, commented, if it
// is not there, so the option is discoverable without reading the manual.
void ConfigureBackendFromFlagFile(const wchar_t* dir);

// Set the backend directly. ConfigureBackendFromFlagFile() calls this; tests
// and the IPC export command use it too.
void SetConfigBackend(ConfigBackendKind kind);
ConfigBackendKind GetConfigBackend();

// Declare a path as one of the app's own configuration files, and give it the
// short name its registry key will use ("settings", "messages", "sprites").
// Unregistered paths always stay real files.
void RegisterManagedConfigFile(const wchar_t* path, const wchar_t* name);

// Build the backend for a path: registry if the path is managed and the
// registry is selected, otherwise the .ini file itself.
std::unique_ptr<ConfigBackend> MakeConfigBackend(const std::wstring& path);

// Copy everything the registry holds for the managed files back into their
// .ini files. For a user who wants to go back to a portable install, and for
// taking a look at what is stored.
bool ExportRegistryToIniFiles(std::wstring* summaryOut);

}  // namespace mdrop

#endif  // MDROP_CONFIG_BACKEND_H
