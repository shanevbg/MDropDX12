#include "preset_milk_text.h"
#include <vector>
#include <cstring>
#include <cctype>
#include <cstdio>

namespace mdrop {
namespace {

// The code-line key prefixes CState::Export writes (state.cpp:1124-1207).
// WriteCode appends the 1-based line number with no separator, so whether the
// prefix ends in '_' is part of the prefix itself -- per_frame_init_1 but
// wave_0_per_frame1.  tick marks the two whose values Export writes with a
// leading backtick (WriteCode's bPrependApostrophe).
struct CodeKind {
  const char* prefix;   // contains %d for the wave/shape index when indexed
  bool        indexed;
  bool        tick;
};

// ORDER MATTERS: per_frame_init_ must be tested before per_frame_, or
// "per_frame_init_1" would match "per_frame_" with the leftover "init_1"
// failing the digit check and the line silently passing through unlifted.
const CodeKind kCodeKinds[] = {
  { "per_frame_init_",     false, false },
  { "per_frame_",          false, false },
  { "per_pixel_",          false, false },
  { "warp_",               false, true  },
  { "comp_",               false, true  },
  { "wave_%d_init",        true,  false },
  { "wave_%d_per_frame",   true,  false },
  { "wave_%d_per_point",   true,  false },
  { "shape_%d_init",       true,  false },
  { "shape_%d_per_frame",  true,  false },
};

const int kMaxIndexed = 16;   // MAX_CUSTOM_WAVES / MAX_CUSTOM_SHAPES

std::vector<std::string> SplitLines(const std::string& s) {
  std::vector<std::string> out;
  std::string cur;
  for (size_t i = 0; i < s.size(); i++) {
    const char c = s[i];
    if (c == '\r') {
      if (i + 1 < s.size() && s[i + 1] == '\n') i++;
      out.push_back(cur);
      cur.clear();
    } else if (c == '\n') {
      out.push_back(cur);
      cur.clear();
    } else {
      cur.push_back(c);
    }
  }
  if (!cur.empty()) out.push_back(cur);
  return out;
}

bool AllDigitsFrom(const char* p, long* out) {
  if (!*p) return false;
  char* end = nullptr;
  const long n = strtol(p, &end, 10);
  if (!end || *end != '\0' || n < 1) return false;
  *out = n;
  return true;
}

// If `key` is <prefix><digits>, return the block name (prefix minus any
// trailing '_'), the line number, and whether its value carries a backtick.
bool MatchCodeKey(const std::string& key, std::string* sectionOut,
                  long* lineOut, bool* tickOut) {
  for (const CodeKind& k : kCodeKinds) {
    if (k.indexed) {
      for (int idx = 0; idx < kMaxIndexed; idx++) {
        char pfx[64];
        snprintf(pfx, sizeof(pfx), k.prefix, idx);
        const size_t plen = strlen(pfx);
        if (key.size() <= plen || key.compare(0, plen, pfx) != 0) continue;
        if (!AllDigitsFrom(key.c_str() + plen, lineOut)) continue;
        *sectionOut = pfx;
        *tickOut = k.tick;
        return true;
      }
    } else {
      const size_t plen = strlen(k.prefix);
      if (key.size() <= plen || key.compare(0, plen, k.prefix) != 0) continue;
      if (!AllDigitsFrom(key.c_str() + plen, lineOut)) continue;
      std::string sec = k.prefix;
      if (!sec.empty() && sec.back() == '_') sec.pop_back();
      *sectionOut = sec;
      *tickOut = k.tick;
      return true;
    }
  }
  return false;
}

// Block name -> the key prefix and backtick flag it came from.
bool SectionToPrefix(const std::string& section, std::string* prefixOut, bool* tickOut) {
  for (const CodeKind& k : kCodeKinds) {
    if (k.indexed) {
      for (int idx = 0; idx < kMaxIndexed; idx++) {
        char pfx[64];
        snprintf(pfx, sizeof(pfx), k.prefix, idx);
        if (section == pfx) { *prefixOut = pfx; *tickOut = k.tick; return true; }
      }
    } else {
      std::string sec = k.prefix;
      if (!sec.empty() && sec.back() == '_') sec.pop_back();
      if (section == sec) { *prefixOut = k.prefix; *tickOut = k.tick; return true; }
    }
  }
  return false;
}

bool IsIniHeader(const std::string& line) {
  return !line.empty() && line[0] == '[' && line.find(']') != std::string::npos;
}

} // namespace

std::string MilkToBlocks(const std::string& milk) {
  const std::vector<std::string> lines = SplitLines(milk);

  std::vector<std::string> passthrough;
  std::vector<std::string> order;                                  // block names
  std::vector<std::vector<std::pair<long, std::string>>> bodies;   // parallel

  // A .milk2 double preset has [preset00] AND [preset01], and the same code
  // keys appear under both. Lifting from more than one would merge two presets'
  // code into one block, so only the first section is lifted; once a second
  // header appears everything passes through verbatim. Blocks are spliced in
  // just before that header so the keys stay inside the section they came from.
  int nHeadersSeen = 0;
  size_t spliceAt = std::string::npos;

  for (const std::string& line : lines) {
    if (IsIniHeader(line)) {
      nHeadersSeen++;
      if (nHeadersSeen == 2) spliceAt = passthrough.size();
    }

    if (nHeadersSeen <= 1) {
      const size_t eq = line.find('=');
      if (eq != std::string::npos && !IsIniHeader(line)) {
        std::string section;
        long num = 0;
        bool tick = false;
        if (MatchCodeKey(line.substr(0, eq), &section, &num, &tick)) {
          std::string val = line.substr(eq + 1);
          if (tick && !val.empty() && val[0] == '`') val.erase(0, 1);
          size_t at = 0;
          for (; at < order.size(); at++) if (order[at] == section) break;
          if (at == order.size()) { order.push_back(section); bodies.emplace_back(); }
          bodies[at].push_back({num, val});
          continue;
        }
      }
    }
    passthrough.push_back(line);
  }

  // Render the lifted blocks.
  std::string blocks;
  for (size_t i = 0; i < order.size(); i++) {
    auto& body = bodies[i];
    // Export emits them in order, but sort so a hand-edited file still works.
    for (size_t a = 1; a < body.size(); a++) {
      auto v = body[a];
      size_t b = a;
      while (b > 0 && body[b - 1].first > v.first) { body[b] = body[b - 1]; b--; }
      body[b] = v;
    }
    blocks += '[';
    blocks += order[i];
    blocks += "]\n";
    for (const auto& pr : body) {
      blocks += pr.second;
      blocks += '\n';
    }
  }

  std::string out;
  for (size_t i = 0; i < passthrough.size(); i++) {
    if (i == spliceAt) out += blocks;
    out += passthrough[i];
    out += '\n';
  }
  if (spliceAt == std::string::npos) out += blocks;
  return out;
}

std::string BlocksToMilk(const std::string& blocks) {
  const std::vector<std::string> lines = SplitLines(blocks);

  std::string out;
  std::string curPrefix;
  bool curTick = false;
  bool inBlock = false;
  long lineNo = 1;

  for (const std::string& line : lines) {
    if (IsIniHeader(line)) {
      const std::string name = line.substr(1, line.find(']') - 1);
      std::string prefix;
      bool tick = false;
      if (SectionToPrefix(name, &prefix, &tick)) {
        curPrefix = prefix;
        curTick = tick;
        inBlock = true;
        lineNo = 1;
        continue;               // our header is synthetic; do not write it out
      }
      inBlock = false;          // a real ini header, e.g. [preset00]
    }

    if (!inBlock) {
      out += line;
      out += '\n';
      continue;
    }

    char key[80];
    snprintf(key, sizeof(key), "%s%ld", curPrefix.c_str(), lineNo++);
    out += key;
    out += '=';
    if (curTick) out += '`';
    out += line;
    out += '\n';
  }
  return out;
}

} // namespace mdrop
