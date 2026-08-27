// shader_overrides.h — user shader overrides, selected by preset tag.
//
// A "shader override" is a warp and/or comp pixel shader the user wrote or
// imported, substituted for a preset's own shader at load.  Which preset gets
// which override is decided by an ordered list of rules matching the tags in
// presets.json.
//
// This mirrors what MilkDrop 3 PRO does with its MD31/MD32 header keys -- a key
// in the file selects a shader from a side store and the preset's own shader
// text is never compiled -- except that the store is the user's, the shaders
// are editable text on disk, and nothing is written into preset files.
//
// Layout:
//
//   resources/shaderoverrides.json   names, rules, master enable
//   resources/shaders/<name>.warp.hlsl
//   resources/shaders/<name>.comp.hlsl
//
// Shader text lives in real files rather than inside the JSON so it can be
// edited with syntax highlighting, diffed, and kept in version control.
//
// This is deliberately NOT part of Engine.  Engine is already ~1,900 lines of
// header with ~1,000 members spread over 23 translation units; a store with no
// existing gravity pulling it in does not go there.

#pragma once

#include <string>
#include <vector>

namespace mdrop {

// Defined in json_utils.h, which this header deliberately does not include:
// the tag must match that definition exactly (struct, not class) or every TU
// seeing both warns C4099.
struct JsonValue;

struct ShaderOverride {
  std::wstring name;        // key, and the display name
  std::wstring warpFile;    // relative to resources/shaders, may be empty
  std::wstring compFile;    // relative to resources/shaders, may be empty
  std::wstring notes;

  std::string  warpText;    // loaded from disk; empty means "keep the preset's"
  std::string  compText;

  // Set when a compile of this override failed, so the window can say which
  // override is broken rather than blaming the preset.
  std::wstring lastError;
};

struct ShaderRule {
  std::vector<std::wstring> tags;   // matches a preset carrying ANY of these
  std::wstring overrideName;        // optional -- a rule may carry only a profile
  std::wstring vfxProfile;          // optional; empty means "says nothing about
                                    // video effects"
  std::wstring audioProfile;        // optional; empty means "says nothing about
                                    // audio". One rule on the md31 tag is how a
                                    // whole converted collection gets MD3 audio
                                    // without editing 54 presets.
  bool enabled = true;
};

class ShaderOverrideStore {
public:
  // resourceDir is the directory holding shaderoverrides.json (m_szMilkdrop2Path).
  bool Load(const wchar_t* resourceDir);
  bool Save() const;

  bool IsEnabled() const { return m_bEnabled; }
  void SetEnabled(bool b) { m_bEnabled = b; }
  bool IsLoaded() const { return m_bLoaded; }

  // First rule, in list order, that is enabled and shares at least one tag with
  // the preset.  Null when the store is disabled, or nothing matches.
  //
  // Returns the RULE, not its override: a rule may name a VFX profile and no
  // override at all, and such a rule is invisible to anything that can only
  // hand back an override.
  const ShaderRule* ResolveRule(const std::vector<std::wstring>& tags,
                                std::wstring* matchedRuleTag = nullptr) const;

  // The matched rule's override, for callers that only care about shaders.
  const ShaderOverride* Resolve(const std::vector<std::wstring>& tags,
                                std::wstring* matchedRuleTag = nullptr) const;
  const ShaderOverride* Find(const std::wstring& name) const;

  std::vector<std::wstring> Names() const;
  const std::vector<ShaderOverride>& Overrides() const { return m_overrides; }
  std::vector<ShaderRule>& Rules() { return m_rules; }
  const std::vector<ShaderRule>& Rules() const { return m_rules; }

  // Creates an empty override; fails when the name is taken or empty.
  bool Add(const std::wstring& name);
  // Copies srcPath into resources/shaders and points the named override's warp
  // or comp slot at it, creating the override if needed.
  bool AddFromFile(const std::wstring& name, const wchar_t* srcPath, bool isWarp);
  // Lifts the warp_N=/comp_N= blocks out of a .milk and writes them as files.
  // Returns false when the preset carries neither.
  bool AddFromMilk(const std::wstring& name, const wchar_t* milkPath);
  bool Rename(const std::wstring& from, const std::wstring& to);
  bool Remove(const std::wstring& name, bool deleteFiles);
  bool ReloadText(const std::wstring& name);

  // Full path of an override's warp/comp file, for opening it in an editor.
  std::wstring FilePath(const std::wstring& name, bool warp) const;
  void SetLastError(const std::wstring& name, const std::wstring& err);

private:
  ShaderOverride* FindMutable(const std::wstring& name);
  std::wstring StorePath() const;
  std::wstring ShaderDir() const;
  bool LoadTextFor(ShaderOverride& o) const;

  std::wstring m_resourceDir;
  std::vector<ShaderOverride> m_overrides;   // ordered for stable display
  std::vector<ShaderRule> m_rules;           // ORDER IS PRIORITY
  bool m_bEnabled = true;
  bool m_bLoaded = false;

  // Members this build did not recognise, re-emitted on save so a newer format
  // is not erased by an older build.  Same preservation vfxprofiles.json uses.
  std::vector<std::pair<std::wstring, JsonValue>> m_unknownRoot;
};

// The process-wide store.
//
// Reached through a function rather than a member on Engine so that engine.h
// needs neither this header nor any JSON type: engine.h is already ~1,900
// lines, and keeping JsonValue out of it was an explicit requirement.
ShaderOverrideStore& ShaderOverrides();

}  // namespace mdrop
