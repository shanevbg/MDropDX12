// preset_hash.cpp — see preset_hash.h for the rule and why it cannot drift.

#include "preset_hash.h"

#include <cstdio>
#include <cstring>
#include <vector>

namespace mdrop {

// Keys excluded from identity.  See the header before adding to this list.
static const char* const kExcludedKeys[] = { "fRating" };

// True when the line's key -- text before the first '=', leading whitespace
// ignored -- matches szKey case-insensitively.
static bool LineKeyIs(const char* line, size_t len, const char* szKey) {
  size_t i = 0;
  while (i < len && (line[i] == ' ' || line[i] == '\t')) i++;

  size_t eq = i;
  while (eq < len && line[eq] != '=') eq++;
  if (eq >= len) return false;  // no '=' on this line: not a key/value pair

  size_t end = eq;
  while (end > i && (line[end - 1] == ' ' || line[end - 1] == '\t')) end--;

  const size_t keyLen = strlen(szKey);
  if (end - i != keyLen) return false;
  return _strnicmp(line + i, szKey, keyLen) == 0;
}

static bool IsExcludedLine(const char* line, size_t len) {
  for (const char* key : kExcludedKeys)
    if (LineKeyIs(line, len, key)) return true;
  return false;
}

static bool IsBlank(const std::string& s) {
  for (char c : s)
    if (c != ' ' && c != '\t') return false;
  return true;
}

std::string ComputePresetHashFromBytes(const char* data, size_t len) {
  if (!data || len == 0) return std::string();

  // Steps 1-3: split, drop excluded keys, strip trailing whitespace.
  std::vector<std::string> lines;
  size_t start = 0;
  for (size_t i = 0; i <= len; i++) {
    if (i != len && data[i] != '\n') continue;

    size_t lineLen = i - start;
    if (lineLen > 0 && data[start + lineLen - 1] == '\r') lineLen--;  // step 1

    if (!IsExcludedLine(data + start, lineLen)) {                     // step 2
      while (lineLen > 0 &&                                           // step 3
             (data[start + lineLen - 1] == ' ' || data[start + lineLen - 1] == '\t'))
        lineLen--;
      lines.emplace_back(data + start, lineLen);
    }
    start = i + 1;
  }

  // Step 4: drop leading and trailing blank lines; keep interior ones.
  size_t first = 0, last = lines.size();
  while (first < last && IsBlank(lines[first])) first++;
  while (last > first && IsBlank(lines[last - 1])) last--;

  // Step 5: join with '\n' and hash.
  std::string body;
  for (size_t i = first; i < last; i++) {
    if (i > first) body.push_back('\n');
    body += lines[i];
  }
  if (body.empty()) return std::string();

  unsigned long long h = 14695981039346656037ULL;  // FNV-1a 64 offset basis
  for (unsigned char c : body) {
    h ^= (unsigned long long)c;
    h *= 1099511628211ULL;                          // FNV-1a 64 prime
  }

  char buf[17];
  snprintf(buf, sizeof(buf), "%016llx", h);
  return std::string(buf);
}

std::string ComputePresetHashFile(const wchar_t* path) {
  if (!path || !path[0]) return std::string();

  FILE* f = _wfopen(path, L"rb");
  if (!f) return std::string();

  fseek(f, 0, SEEK_END);
  long size = ftell(f);
  fseek(f, 0, SEEK_SET);
  if (size <= 0) { fclose(f); return std::string(); }

  std::vector<char> buf((size_t)size);
  size_t got = fread(buf.data(), 1, (size_t)size, f);
  fclose(f);

  return ComputePresetHashFromBytes(buf.data(), got);
}

}  // namespace mdrop
