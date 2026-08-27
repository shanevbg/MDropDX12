#pragma once
// Tokenizers for the two languages a .milk preset contains: ns-eel2 expressions
// (per-frame / per-pixel / init / wave / shape code) and MilkDrop-flavoured HLSL
// (warp and comp shaders).
//
// Deliberately free of Win32 and Scintilla so it can be built and tested on its
// own -- see tools/lexer-test/.  Output is one style byte per input byte, which
// is exactly what Scintilla's SCI_SETSTYLINGEX consumes, so no run-length
// conversion is needed anywhere.
#include <cstddef>

namespace mdrop {

enum class CodeLang { EEL, HLSL };

enum SciStyle : unsigned char {
  SCI_ST_DEFAULT   = 0,
  SCI_ST_COMMENT   = 1,
  SCI_ST_NUMBER    = 2,
  SCI_ST_STRING    = 3,
  SCI_ST_KEYWORD   = 4,   // language keywords / HLSL types
  SCI_ST_FUNCTION  = 5,   // built-in functions
  SCI_ST_PRESETVAR = 6,   // MilkDrop magic variables (q1..q32, bass, uv, rad, ...)
  SCI_ST_OPERATOR  = 7,
  SCI_ST_PREPROC   = 8,   // #define / #include
  SCI_ST_IDENT     = 9,   // everything else that is a name
  SCI_ST_COUNT     = 10,
};

// Fills styleOut[0..len) with one style byte per byte of text.
// styleOut must have room for len bytes.  Never reads or writes out of range.
void LexCode(CodeLang lang, const char* text, size_t len, unsigned char* styleOut);

} // namespace mdrop
