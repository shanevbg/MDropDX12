// shader_overrides.cpp — see shader_overrides.h.

#include "shader_overrides.h"
#include "json_utils.h"
#include "utility.h"

#include <windows.h>
#include <shlwapi.h>
#include <cstdio>
#include <cstring>

namespace mdrop {

// ─── small helpers ──────────────────────────────────────────────────────

static bool IEquals(const std::wstring& a, const std::wstring& b) {
  return _wcsicmp(a.c_str(), b.c_str()) == 0;
}

// Turns a display name into something safe to use as a filename.
static std::wstring SafeFileStem(const std::wstring& name) {
  std::wstring out;
  for (wchar_t c : name) {
    if (wcschr(L"\\/:*?\"<>|", c) || c < 32) out.push_back(L'_');
    else out.push_back(c);
  }
  while (!out.empty() && (out.back() == L' ' || out.back() == L'.')) out.pop_back();
  if (out.empty()) out = L"override";
  return out;
}

static std::string ReadTextFile(const wchar_t* path) {
  FILE* f = _wfopen(path, L"rb");
  if (!f) return std::string();
  fseek(f, 0, SEEK_END);
  long size = ftell(f);
  fseek(f, 0, SEEK_SET);
  if (size <= 0) { fclose(f); return std::string(); }
  std::string s((size_t)size, '\0');
  size_t got = fread(&s[0], 1, (size_t)size, f);
  fclose(f);
  s.resize(got);
  return s;
}

static bool WriteTextFile(const wchar_t* path, const std::string& text) {
  FILE* f = _wfopen(path, L"wb");
  if (!f) return false;
  if (!text.empty()) fwrite(text.data(), 1, text.size(), f);
  fclose(f);
  return true;
}

// ─── paths ──────────────────────────────────────────────────────────────

std::wstring ShaderOverrideStore::StorePath() const {
  return m_resourceDir + L"shaderoverrides.json";
}

std::wstring ShaderOverrideStore::ShaderDir() const {
  return m_resourceDir + L"shaders\\";
}

std::wstring ShaderOverrideStore::FilePath(const std::wstring& name, bool warp) const {
  for (const auto& o : m_overrides) {
    if (!IEquals(o.name, name)) continue;
    const std::wstring& file = warp ? o.warpFile : o.compFile;
    if (file.empty()) return std::wstring();
    return ShaderDir() + file;
  }
  return std::wstring();
}

// ─── load / save ────────────────────────────────────────────────────────

bool ShaderOverrideStore::LoadTextFor(ShaderOverride& o) const {
  o.warpText.clear();
  o.compText.clear();
  if (!o.warpFile.empty()) o.warpText = ReadTextFile((ShaderDir() + o.warpFile).c_str());
  if (!o.compFile.empty()) o.compText = ReadTextFile((ShaderDir() + o.compFile).c_str());
  return true;
}

bool ShaderOverrideStore::Load(const wchar_t* resourceDir) {
  m_resourceDir = resourceDir ? resourceDir : L"";
  m_overrides.clear();
  m_rules.clear();
  m_unknownRoot.clear();
  m_bEnabled = true;
  m_bLoaded = true;

  JsonValue root = JsonLoadFile(StorePath().c_str());
  if (root.isNull()) {
    DLOG_INFO("ShaderOverrideStore: no shaderoverrides.json; starting empty");
    return true;   // absence is normal, not an error
  }

  if (root.has(L"enabled")) m_bEnabled = root[L"enabled"].asBool(true);

  const JsonValue& overrides = root[L"overrides"];
  if (overrides.isObject()) {
    for (const auto& [key, val] : overrides.members) {
      ShaderOverride o;
      o.name     = key;
      o.warpFile = val[L"warp"].asString();
      o.compFile = val[L"comp"].asString();
      o.notes    = val[L"notes"].asString();
      LoadTextFor(o);
      m_overrides.push_back(std::move(o));
    }
  }

  const JsonValue& rules = root[L"rules"];
  if (rules.isArray()) {
    for (size_t i = 0; i < rules.size(); i++) {
      const JsonValue& r = rules.at(i);
      ShaderRule rule;
      rule.overrideName = r[L"override"].asString();
      rule.vfxProfile = r[L"vfx"].asString();
      rule.audioProfile = r[L"audio"].asString();
      rule.enabled = r[L"enabled"].asBool(true);
      const JsonValue& tags = r[L"tags"];
      for (size_t t = 0; t < tags.size(); t++) {
        std::wstring tag = tags.at(t).asString();
        if (!tag.empty()) rule.tags.push_back(tag);
      }
      // A rule needs a tag to match on and SOMETHING to contribute.  Requiring
      // an override here is what made a profile-only rule impossible: it was
      // dropped at load, so resolution never saw it and the rule looked
      // ignored rather than rejected.
      if (!rule.tags.empty() &&
          (!rule.overrideName.empty() || !rule.vfxProfile.empty() ||
           !rule.audioProfile.empty()))
        m_rules.push_back(std::move(rule));
    }
  }

  // Keep anything this build does not understand, so a newer format survives a
  // rewrite here.
  for (const auto& [key, val] : root.members) {
    if (key == L"version" || key == L"enabled" ||
        key == L"overrides" || key == L"rules") continue;
    m_unknownRoot.emplace_back(key, val);
  }

  DLOG_INFO("ShaderOverrideStore: %d overrides, %d rules, enabled=%d",
            (int)m_overrides.size(), (int)m_rules.size(), m_bEnabled ? 1 : 0);
  return true;
}

bool ShaderOverrideStore::Save() const {
  if (m_resourceDir.empty()) return false;

  JsonWriter w;
  w.BeginObject();
  w.Int(L"version", 1);
  w.Bool(L"enabled", m_bEnabled);

  w.BeginObject(L"overrides");
  for (const auto& o : m_overrides) {
    w.BeginObject(o.name.c_str());
    w.String(L"warp", o.warpFile);
    w.String(L"comp", o.compFile);
    w.String(L"notes", o.notes);
    w.EndObject();
  }
  w.EndObject();

  w.BeginArray(L"rules");
  for (const auto& r : m_rules) {
    w.BeginObject();
    JsonValue tags;
    tags.type = JsonValue::Array;
    for (const auto& t : r.tags) tags.elements.push_back(JsonValue(t));
    w.Value(L"tags", tags);
    w.String(L"override", r.overrideName);
    w.String(L"vfx", r.vfxProfile);
    w.String(L"audio", r.audioProfile);
    w.Bool(L"enabled", r.enabled);
    w.EndObject();
  }
  w.EndArray();

  for (const auto& [key, val] : m_unknownRoot)
    w.Value(key.c_str(), val);

  w.EndObject();
  return w.SaveToFile(StorePath().c_str());
}

// ─── lookup ─────────────────────────────────────────────────────────────

const ShaderOverride* ShaderOverrideStore::Find(const std::wstring& name) const {
  for (const auto& o : m_overrides)
    if (IEquals(o.name, name)) return &o;
  return nullptr;
}

ShaderOverride* ShaderOverrideStore::FindMutable(const std::wstring& name) {
  for (auto& o : m_overrides)
    if (IEquals(o.name, name)) return &o;
  return nullptr;
}

// ANY tag within a rule, FIRST matching rule across rules.  Array order is the
// priority, so one preset resolves to exactly one override and two overrides
// can never fight over the same shader slot.
const ShaderRule* ShaderOverrideStore::ResolveRule(const std::vector<std::wstring>& tags,
                                                   std::wstring* matchedRuleTag) const {
  if (!m_bEnabled || tags.empty()) return nullptr;

  for (const auto& rule : m_rules) {
    if (!rule.enabled) continue;
    for (const auto& ruleTag : rule.tags) {
      for (const auto& presetTag : tags) {
        if (!IEquals(ruleTag, presetTag)) continue;
        if (matchedRuleTag) *matchedRuleTag = ruleTag;
        return &rule;
      }
    }
  }
  return nullptr;
}

const ShaderOverride* ShaderOverrideStore::Resolve(const std::vector<std::wstring>& tags,
                                                   std::wstring* matchedRuleTag) const {
  // First match wins, which is the documented rule and what the Up/Down
  // buttons exist to control.  A matched rule whose override is missing
  // therefore yields nothing rather than falling through to a later rule --
  // the old code kept scanning, which quietly made rule order not mean what
  // the window says it means.
  const ShaderRule* r = ResolveRule(tags, matchedRuleTag);
  return r ? Find(r->overrideName) : nullptr;
}

std::vector<std::wstring> ShaderOverrideStore::Names() const {
  std::vector<std::wstring> out;
  for (const auto& o : m_overrides) out.push_back(o.name);
  return out;
}

// ─── mutation ───────────────────────────────────────────────────────────

bool ShaderOverrideStore::Add(const std::wstring& name) {
  if (name.empty() || Find(name)) return false;
  ShaderOverride o;
  o.name = name;
  m_overrides.push_back(std::move(o));
  return Save();
}

bool ShaderOverrideStore::AddFromFile(const std::wstring& name, const wchar_t* srcPath,
                                      bool isWarp) {
  if (name.empty() || !srcPath || !srcPath[0]) return false;

  std::string text = ReadTextFile(srcPath);
  if (text.empty()) return false;

  CreateDirectoryW(ShaderDir().c_str(), NULL);

  const std::wstring file = SafeFileStem(name) + (isWarp ? L".warp.hlsl" : L".comp.hlsl");
  if (!WriteTextFile((ShaderDir() + file).c_str(), text)) return false;

  ShaderOverride* o = FindMutable(name);
  if (!o) {
    ShaderOverride fresh;
    fresh.name = name;
    m_overrides.push_back(std::move(fresh));
    o = &m_overrides.back();
  }
  if (isWarp) { o->warpFile = file; o->warpText = text; }
  else        { o->compFile = file; o->compText = text; }
  o->lastError.clear();
  return Save();
}

// Lifts `warp_N=` / `comp_N=` blocks out of a preset.  Each line carries an
// optional leading backtick, which is MilkDrop's marker for "this line is
// shader source" and is not part of the code.
bool ShaderOverrideStore::AddFromMilk(const std::wstring& name, const wchar_t* milkPath) {
  if (name.empty() || !milkPath || !milkPath[0]) return false;

  std::string raw = ReadTextFile(milkPath);
  if (raw.empty()) return false;

  std::string warp, comp;
  size_t start = 0;
  while (start <= raw.size()) {
    size_t end = raw.find('\n', start);
    if (end == std::string::npos) end = raw.size();
    std::string line = raw.substr(start, end - start);
    if (!line.empty() && line.back() == '\r') line.pop_back();
    start = end + 1;

    const bool isWarp = line.compare(0, 5, "warp_") == 0;
    const bool isComp = line.compare(0, 5, "comp_") == 0;
    if (!isWarp && !isComp) continue;

    size_t eq = line.find('=');
    if (eq == std::string::npos) continue;
    std::string body = line.substr(eq + 1);
    if (!body.empty() && body[0] == '`') body.erase(0, 1);

    (isWarp ? warp : comp) += body;
    (isWarp ? warp : comp) += "\n";
  }

  if (warp.empty() && comp.empty()) return false;

  CreateDirectoryW(ShaderDir().c_str(), NULL);

  ShaderOverride* o = FindMutable(name);
  if (!o) {
    ShaderOverride fresh;
    fresh.name = name;
    m_overrides.push_back(std::move(fresh));
    o = &m_overrides.back();
  }

  if (!warp.empty()) {
    o->warpFile = SafeFileStem(name) + L".warp.hlsl";
    o->warpText = warp;
    WriteTextFile((ShaderDir() + o->warpFile).c_str(), warp);
  }
  if (!comp.empty()) {
    o->compFile = SafeFileStem(name) + L".comp.hlsl";
    o->compText = comp;
    WriteTextFile((ShaderDir() + o->compFile).c_str(), comp);
  }
  o->lastError.clear();
  return Save();
}

bool ShaderOverrideStore::Rename(const std::wstring& from, const std::wstring& to) {
  if (to.empty() || Find(to)) return false;
  ShaderOverride* o = FindMutable(from);
  if (!o) return false;

  o->name = to;
  // Rules point at the name, so they follow it.
  for (auto& r : m_rules)
    if (IEquals(r.overrideName, from)) r.overrideName = to;
  return Save();
}

bool ShaderOverrideStore::Remove(const std::wstring& name, bool deleteFiles) {
  for (size_t i = 0; i < m_overrides.size(); i++) {
    if (!IEquals(m_overrides[i].name, name)) continue;

    if (deleteFiles) {
      if (!m_overrides[i].warpFile.empty())
        DeleteFileW((ShaderDir() + m_overrides[i].warpFile).c_str());
      if (!m_overrides[i].compFile.empty())
        DeleteFileW((ShaderDir() + m_overrides[i].compFile).c_str());
    }
    m_overrides.erase(m_overrides.begin() + i);

    // Rules referring to it would silently never match; drop them so the list
    // does not accumulate rules that cannot fire.
    for (size_t r = 0; r < m_rules.size(); ) {
      if (IEquals(m_rules[r].overrideName, name)) m_rules.erase(m_rules.begin() + r);
      else r++;
    }
    return Save();
  }
  return false;
}

bool ShaderOverrideStore::ReloadText(const std::wstring& name) {
  ShaderOverride* o = FindMutable(name);
  if (!o) return false;
  o->lastError.clear();
  return LoadTextFor(*o);
}

void ShaderOverrideStore::SetLastError(const std::wstring& name, const std::wstring& err) {
  ShaderOverride* o = FindMutable(name);
  if (o) o->lastError = err;
}

ShaderOverrideStore& ShaderOverrides() {
  static ShaderOverrideStore s_store;
  return s_store;
}

}  // namespace mdrop
