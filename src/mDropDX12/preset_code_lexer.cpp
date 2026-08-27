#include "preset_code_lexer.h"
#include <cstring>
#include <cctype>
#include <string>
#include <unordered_set>

namespace mdrop {
namespace {

// ns-eel2 built-in functions, per src/ns-eel2 and MilkDrop's documented set.
const char* const kEelFunctions[] = {
  "abs","acos","asin","atan","atan2","above","below","bnot","band","bor",
  "ceil","cos","cosh","exp","equal","floor","gmegabuf","if","int","invsqrt",
  "log","log10","loop","max","megabuf","min","pow","rand","sigmoid","sign",
  "sin","sinh","sqr","sqrt","tan","tanh","while","exec2","exec3","freembuf",
  "memcpy","memset","assign",
};

// EEL reserved words that are not functions.
const char* const kEelKeywords[] = {
  "else","then","function","local","global","instance",
};

// MilkDrop per-frame / per-pixel magic variables.  q1..q32, t1..t8 and
// reg00..reg99 are matched by pattern in IsNumberedPresetVar() instead.
const char* const kEelPresetVars[] = {
  "bass","bass_att","mid","mid_att","treb","treb_att","vol","vol_att",
  "bass_smooth","mid_smooth","treb_smooth","vol_smooth",
  "vis_intensity","vis_shift","vis_version","colshift_hue",
  "time","frame","fps","progress","meshx","meshy","pixelsx","pixelsy",
  "aspectx","aspecty","zoom","zoomexp","rot","warp","cx","cy","dx","dy","sx","sy",
  "x","y","rad","ang","decay",
  "wave_a","wave_r","wave_g","wave_b","wave_x","wave_y","wave_mystery",
  "wave_mode","wave_usedots","wave_thick","wave_additive","wave_brighten",
  "ob_size","ob_r","ob_g","ob_b","ob_a","ib_size","ib_r","ib_g","ib_b","ib_a",
  "mv_x","mv_y","mv_dx","mv_dy","mv_l","mv_r","mv_g","mv_b","mv_a",
  "echo_zoom","echo_alpha","echo_orient","darken_center","gamma","echo",
  "monitor","rating","brighten","darken","solarize","invert",
  "sample","value1","value2","r","g","b","a","r2","g2","b2","a2",
  "border_r","border_g","border_b","border_a","thick","additive","textured",
  "num_inst","sides","rad2","tex_ang","tex_zoom",
};

// HLSL keywords and types.
const char* const kHlslKeywords[] = {
  "float","float2","float3","float4","half","half2","half3","half4",
  "int","int2","int3","int4","uint","bool","bool2","bool3","bool4",
  "float2x2","float3x3","float4x4","matrix","vector","void","struct",
  "return","if","else","for","while","do","break","continue","discard",
  "switch","case","default",
  "in","out","inout","const","static","uniform","shared","extern","volatile",
  "sampler","sampler1D","sampler2D","sampler3D","samplerCUBE","texture",
  "Texture1D","Texture2D","Texture3D","TextureCube","SamplerState",
  "cbuffer","register","true","false",
};

const char* const kHlslFunctions[] = {
  "abs","acos","all","any","asin","atan","atan2","ceil","clamp","clip","cos",
  "cosh","cross","ddx","ddy","degrees","determinant","distance","dot","exp",
  "exp2","faceforward","floor","fmod","frac","frexp","fwidth","isfinite",
  "isinf","isnan","ldexp","length","lerp","lit","log","log10","log2","max",
  "min","modf","mul","noise","normalize","pow","radians","reflect","refract",
  "round","rsqrt","saturate","sign","sin","sincos","sinh","smoothstep","sqrt",
  "step","tan","tanh","tex1D","tex2D","tex2Dbias","tex2Dlod","tex2Dproj",
  "tex3D","texCUBE","transpose","trunc",
  "get_fft","get_fft_hz","get_fft_peak","get_fft_peak_hz",
};

// MilkDrop shader-side magic names.
const char* const kHlslPresetVars[] = {
  "uv","uv_orig","rad","ang","hue_shader","ret","shader_body",
  "time","fps","frame","progress","bass","mid","treb",
  "bass_att","mid_att","treb_att","vol",
  "texsize","aspect","texsize_noise_lq","texsize_noise_mq","texsize_noise_hq",
  "roam_cos","roam_sin","rand_frame","rand_preset","slow_roam_cos","slow_roam_sin",
  "sampler_main","sampler_fw_main","sampler_pw_main","sampler_fc_main","sampler_pc_main",
  "sampler_blur1","sampler_blur2","sampler_blur3",
  "sampler_blur4","sampler_blur5","sampler_blur6",
  "sampler_noise_lq","sampler_noise_mq","sampler_noise_hq",
  "sampler_noisevol_lq","sampler_noisevol_hq",
  "sampler_pw_noise_lq","sampler_noise_lq_lite","sampler_audio",
};

struct Tables {
  std::unordered_set<std::string> fn, kw, pv;
  Tables(const char* const* f, size_t nf,
         const char* const* k, size_t nk,
         const char* const* p, size_t np) {
    for (size_t i = 0; i < nf; i++) fn.insert(f[i]);
    for (size_t i = 0; i < nk; i++) kw.insert(k[i]);
    for (size_t i = 0; i < np; i++) pv.insert(p[i]);
  }
};

const Tables& EelTables() {
  static const Tables t(kEelFunctions,  sizeof(kEelFunctions)  / sizeof(*kEelFunctions),
                        kEelKeywords,   sizeof(kEelKeywords)   / sizeof(*kEelKeywords),
                        kEelPresetVars, sizeof(kEelPresetVars) / sizeof(*kEelPresetVars));
  return t;
}

const Tables& HlslTables() {
  static const Tables t(kHlslFunctions,  sizeof(kHlslFunctions)  / sizeof(*kHlslFunctions),
                        kHlslKeywords,   sizeof(kHlslKeywords)   / sizeof(*kHlslKeywords),
                        kHlslPresetVars, sizeof(kHlslPresetVars) / sizeof(*kHlslPresetVars));
  return t;
}

inline bool IsWordStart(char c) { return isalpha((unsigned char)c) != 0 || c == '_'; }
inline bool IsWordChar(char c)  { return isalnum((unsigned char)c) != 0 || c == '_'; }

// q1..q32, t1..t8, reg00..reg99 -- MilkDrop's numbered registers.
bool IsNumberedPresetVar(const std::string& w) {
  if (w.size() >= 2 && (w[0] == 'q' || w[0] == 't')) {
    size_t i = 1;
    while (i < w.size() && isdigit((unsigned char)w[i])) i++;
    if (i == w.size()) return true;
  }
  if (w.size() == 5 && w.compare(0, 3, "reg") == 0 &&
      isdigit((unsigned char)w[3]) && isdigit((unsigned char)w[4]))
    return true;
  return false;
}

} // namespace

void LexCode(CodeLang lang, const char* text, size_t len, unsigned char* styleOut) {
  if (!text || !styleOut || len == 0) return;
  const Tables& T = (lang == CodeLang::EEL) ? EelTables() : HlslTables();
  memset(styleOut, SCI_ST_DEFAULT, len);

  size_t i = 0;
  while (i < len) {
    const char c = text[i];

    // Line comment
    if (c == '/' && i + 1 < len && text[i + 1] == '/') {
      while (i < len && text[i] != '\n') styleOut[i++] = SCI_ST_COMMENT;
      continue;
    }
    // Block comment
    if (c == '/' && i + 1 < len && text[i + 1] == '*') {
      styleOut[i++] = SCI_ST_COMMENT;
      styleOut[i++] = SCI_ST_COMMENT;
      while (i < len) {
        if (text[i] == '*' && i + 1 < len && text[i + 1] == '/') {
          styleOut[i++] = SCI_ST_COMMENT;
          styleOut[i++] = SCI_ST_COMMENT;
          break;
        }
        styleOut[i++] = SCI_ST_COMMENT;
      }
      continue;
    }
    // Preprocessor -- HLSL only, and only at the first non-space on a line.
    if (lang == CodeLang::HLSL && c == '#') {
      bool atLineStart = true;
      for (size_t j = i; j > 0; j--) {
        const char p = text[j - 1];
        if (p == '\n') break;
        if (p != ' ' && p != '\t') { atLineStart = false; break; }
      }
      if (atLineStart) {
        while (i < len && text[i] != '\n') styleOut[i++] = SCI_ST_PREPROC;
        continue;
      }
    }
    // String
    if (c == '"') {
      styleOut[i++] = SCI_ST_STRING;
      while (i < len && text[i] != '"' && text[i] != '\n') {
        if (text[i] == '\\' && i + 1 < len) styleOut[i++] = SCI_ST_STRING;
        styleOut[i++] = SCI_ST_STRING;
      }
      if (i < len && text[i] == '"') styleOut[i++] = SCI_ST_STRING;
      continue;
    }
    // Number.  A '.' only starts a number when a digit follows, so "uv.x" keeps
    // the dot as an operator and the swizzle as an identifier.
    if (isdigit((unsigned char)c) ||
        (c == '.' && i + 1 < len && isdigit((unsigned char)text[i + 1]))) {
      while (i < len && (isdigit((unsigned char)text[i]) || text[i] == '.' ||
                         text[i] == 'e' || text[i] == 'E' ||
                         ((text[i] == '+' || text[i] == '-') && i > 0 &&
                          (text[i - 1] == 'e' || text[i - 1] == 'E'))))
        styleOut[i++] = SCI_ST_NUMBER;
      continue;
    }
    // Identifier / keyword
    if (IsWordStart(c)) {
      const size_t start = i;
      while (i < len && IsWordChar(text[i])) i++;
      const std::string w(text + start, i - start);
      unsigned char st = SCI_ST_IDENT;
      if (T.kw.count(w))                                        st = SCI_ST_KEYWORD;
      else if (T.fn.count(w))                                   st = SCI_ST_FUNCTION;
      else if (T.pv.count(w))                                   st = SCI_ST_PRESETVAR;
      else if (lang == CodeLang::EEL && IsNumberedPresetVar(w))  st = SCI_ST_PRESETVAR;
      memset(styleOut + start, st, i - start);
      continue;
    }
    // Operator
    if (c != '\0' && strchr("+-*/%=<>!&|^~?:;,()[]{}.", c) != nullptr) {
      styleOut[i++] = SCI_ST_OPERATOR;
      continue;
    }
    // Whitespace and anything else stays default.
    styleOut[i++] = SCI_ST_DEFAULT;
  }
}

} // namespace mdrop
