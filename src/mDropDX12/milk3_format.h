#pragma once
/*
  milk3_format.h — the .milk3 key vocabulary, in one place.

  A .milk3 holds HLSL. The GLSL->HLSL conversion happens once, at import, so
  nothing downstream ever sees GLSL and the shader text is plain HLSL written
  against MilkDrop's own preamble (time, texsize, float2, frac, ...).

  The KEY NAMES used to be Shadertoy's ("image", "bufferA".."bufferD",
  "channels", "iChannel0", plus a "shadertoy": true flag). That was wrong on
  two counts: the format is not Shadertoy's and never was after conversion, and
  a third of the shipped library came from glslsandbox yet was still stamped
  shadertoy:true. Shadertoy's layout is also not a standard -- other sites use
  different pass models -- so naming the format after one of them dates it.

  The names are now neutral:

      image        ->  main
      bufferA..D   ->  pass0..pass3
      channels     ->  inputs
      iChannel0    ->  channel0
      "shadertoy": true -> dropped; "version" already identifies the format

  READING ACCEPTS BOTH SPELLINGS, WRITING EMITS ONLY THE NEW ONES. Every .milk3
  written by an earlier build keeps loading untouched, so there is no migration
  step and no file to rewrite. The pass names appear as channel VALUES as well
  as keys ("channel0": "pass0"), so Milk3PassValueIs() has to alias those too.

  Do not add a key here without adding its legacy spelling beside it: the whole
  point of this header is that the loader, the writers and the import UI cannot
  disagree about what a key is called.
*/

#include "json_utils.h"
#include <string>

namespace mdrop {

// Canonical names, written by this build.
inline const wchar_t* kMilk3KeyMain    = L"main";
inline const wchar_t* kMilk3KeyInputs  = L"inputs";
inline const wchar_t* kMilk3KeyChannel0 = L"channel0";
// Index 0..3 -> "pass0".."pass3".
inline const wchar_t* Milk3PassKey(int i) {
  static const wchar_t* k[4] = { L"pass0", L"pass1", L"pass2", L"pass3" };
  return (i >= 0 && i < 4) ? k[i] : L"";
}

// Legacy (Shadertoy) spellings, accepted on read only.
inline const wchar_t* kMilk3LegacyKeyMain     = L"image";
inline const wchar_t* kMilk3LegacyKeyInputs   = L"channels";
inline const wchar_t* kMilk3LegacyKeyChannel0 = L"iChannel0";
inline const wchar_t* Milk3LegacyPassKey(int i) {
  static const wchar_t* k[4] = { L"bufferA", L"bufferB", L"bufferC", L"bufferD" };
  return (i >= 0 && i < 4) ? k[i] : L"";
}

// Read a member under its canonical name, falling back to the legacy one.
// The canonical name wins if a file somehow carries both.
inline const JsonValue& Milk3Member(const JsonValue& obj,
                                    const wchar_t* name, const wchar_t* legacy) {
  return obj.has(name) ? obj[name] : obj[legacy];
}

inline std::wstring Milk3String(const JsonValue& obj,
                                const wchar_t* name, const wchar_t* legacy,
                                const wchar_t* def = L"") {
  return Milk3Member(obj, name, legacy).asString(def);
}

// True when a channel value names pass `i`, in either spelling.
inline bool Milk3PassValueIs(const std::wstring& v, int i) {
  return v == Milk3PassKey(i) || v == Milk3LegacyPassKey(i);
}

// True when a channel value names the main pass, in either spelling.
inline bool Milk3MainValueIs(const std::wstring& v) {
  return v == kMilk3KeyMain || v == kMilk3LegacyKeyMain;
}

}  // namespace mdrop
