#include "sci_editor.h"
#include "utility.h"
#include <vector>
#include "Scintilla.h"   // external/scintilla/include

// Scintilla's C entry point.  ScintillaDLL.cxx (which holds DllMain) is excluded
// from the build, so nothing registers the window class for us.
extern "C" int Scintilla_RegisterClasses(void* hInstance);

namespace mdrop {
namespace {

// Scintilla colours are 0x00BBGGRR.  Palettes below are authored as 0xRRGGBB
// and converted here, so the byte swap lives in exactly one place.
inline int SciColour(unsigned rgb) {
  return (int)(((rgb & 0xFFu) << 16) | (rgb & 0xFF00u) | ((rgb >> 16) & 0xFFu));
}

struct Palette {
  unsigned back, text, comment, number, string, keyword, function,
           presetvar, oper, preproc, ident,
           caret, selBack, marginBack, marginText, caretLine;
};

const Palette kDark = {
  /*back*/       0x1E1E1E, /*text*/      0xDCDCDC, /*comment*/   0x6A9955,
  /*number*/     0xB5CEA8, /*string*/    0xCE9178, /*keyword*/   0x569CD6,
  /*function*/   0xDCDCAA, /*presetvar*/ 0xC586C0, /*oper*/      0xD4D4D4,
  /*preproc*/    0x9B9B9B, /*ident*/     0x9CDCFE,
  /*caret*/      0xFFFFFF, /*selBack*/   0x264F78, /*marginBack*/0x252526,
  /*marginText*/ 0x858585, /*caretLine*/ 0x2A2A2A,
};

const Palette kLight = {
  /*back*/       0xFFFFFF, /*text*/      0x000000, /*comment*/   0x008000,
  /*number*/     0x098658, /*string*/    0xA31515, /*keyword*/   0x0000FF,
  /*function*/   0x795E26, /*presetvar*/ 0xAF00DB, /*oper*/      0x000000,
  /*preproc*/    0x808080, /*ident*/     0x001080,
  /*caret*/      0x000000, /*selBack*/   0xADD6FF, /*marginBack*/0xF0F0F0,
  /*marginText*/ 0x707070, /*caretLine*/ 0xF5F5F5,
};

} // namespace

bool SciEditor::RegisterScintilla() {
  static bool bTried = false;
  static bool bOK = false;
  if (bTried) return bOK;
  bTried = true;
  bOK = (Scintilla_RegisterClasses(GetModuleHandleW(NULL)) != 0);
  if (!bOK)
    DebugLogA("SciEditor: Scintilla_RegisterClasses failed", LOG_ERROR);
  return bOK;
}

bool SciEditor::Create(HWND hParent, int id, int x, int y, int w, int h) {
  Destroy();
  if (!RegisterScintilla()) return false;

  m_hWnd = CreateWindowExW(0, L"Scintilla", L"",
                           WS_CHILD | WS_VISIBLE | WS_TABSTOP,
                           x, y, w, h, hParent,
                           (HMENU)(INT_PTR)id, GetModuleHandleW(NULL), NULL);
  if (!m_hWnd) {
    DebugLogA("SciEditor: CreateWindowExW(Scintilla) failed", LOG_ERROR);
    return false;
  }

  m_fn  = (SciFn)SendMessageW(m_hWnd, SCI_GETDIRECTFUNCTION, 0, 0);
  m_ptr = (void*)SendMessageW(m_hWnd, SCI_GETDIRECTPOINTER, 0, 0);
  if (!m_fn || !m_ptr) {
    DebugLogA("SciEditor: SCI_GETDIRECTFUNCTION/POINTER returned null", LOG_ERROR);
    Destroy();
    return false;
  }

  // NOTE: no SCI_SETILEXER call.  A document with no lexer attached is exactly
  // Scintilla 5's container-lexing mode, which is what we want.
  Call(SCI_SETCODEPAGE, SC_CP_UTF8);
  Call(SCI_SETTABWIDTH, 2);
  Call(SCI_SETUSETABS, 0);
  Call(SCI_SETEOLMODE, SC_EOL_LF);
  ConfigureMargins();
  // Our own context menu carries the Options submenu, so suppress Scintilla's.
  Call(SCI_USEPOPUP, SC_POPUP_NEVER);
  Call(SCI_SETSCROLLWIDTHTRACKING, 1);
  Call(SCI_SETMULTIPLESELECTION, 1);
  Call(SCI_SETADDITIONALSELECTIONTYPING, 1);
  Call(SCI_SETINDENTATIONGUIDES, SC_IV_LOOKBOTH);

  // Indicator 0 = compile error squiggle.
  Call(SCI_INDICSETSTYLE, 0, INDIC_SQUIGGLE);
  Call(SCI_INDICSETFORE, 0, SciColour(0xFF3B30));

  // m_nFontPt is a point size; feed it back through as pixels at this DPI so the
  // one code path that touches SCI_STYLESETSIZE stays the only one.
  {
    int dpiY = 96;
    if (HDC hdc = GetDC(m_hWnd)) {
      const int d = GetDeviceCaps(hdc, LOGPIXELSY);
      if (d > 0) dpiY = d;
      ReleaseDC(m_hWnd, hdc);
    }
    SetFontHeightPx(MulDiv(m_nFontPt, dpiY, 72));  // applies theme + margin too
  }
  return true;
}

// Margin 0 = line numbers, margin 1 = fold symbols.  Both are sized to zero
// when switched off, which is how Scintilla hides a margin.
void SciEditor::ConfigureMargins() {
  if (!IsValid()) return;

  Call(SCI_SETMARGINS, 2);

  Call(SCI_SETMARGINTYPEN, 0, SC_MARGIN_NUMBER);
  if (m_bLineNumbers) {
    const int w = (int)Call(SCI_TEXTWIDTH, STYLE_LINENUMBER, (intptr_t)"_99999");
    Call(SCI_SETMARGINWIDTHN, 0, w > 0 ? w : 40);
  } else {
    Call(SCI_SETMARGINWIDTHN, 0, 0);
  }

  Call(SCI_SETMARGINTYPEN, 1, SC_MARGIN_SYMBOL);
  Call(SCI_SETMARGINMASKN, 1, SC_MASK_FOLDERS);
  Call(SCI_SETMARGINSENSITIVEN, 1, m_bFolding ? 1 : 0);
  Call(SCI_SETMARGINWIDTHN, 1, m_bFolding ? MulDiv(m_nFontPt, 14, 10) : 0);

  if (m_bFolding) {
    // Box-tree markers, the shape most editors use.
    const int markers[7][2] = {
      { SC_MARKNUM_FOLDEROPEN,    SC_MARK_BOXMINUS },
      { SC_MARKNUM_FOLDER,        SC_MARK_BOXPLUS },
      { SC_MARKNUM_FOLDERSUB,     SC_MARK_VLINE },
      { SC_MARKNUM_FOLDERTAIL,    SC_MARK_LCORNER },
      { SC_MARKNUM_FOLDEREND,     SC_MARK_BOXPLUSCONNECTED },
      { SC_MARKNUM_FOLDEROPENMID, SC_MARK_BOXMINUSCONNECTED },
      { SC_MARKNUM_FOLDERMIDTAIL, SC_MARK_TCORNER },
    };
    const Palette& P = m_bDark ? kDark : kLight;
    for (const auto& m : markers) {
      Call(SCI_MARKERDEFINE, m[0], m[1]);
      // Inverted on purpose: the glyph is drawn in the margin colour on a
      // foreground-coloured box, which is what makes the boxes legible.
      Call(SCI_MARKERSETFORE, m[0], SciColour(P.marginBack));
      Call(SCI_MARKERSETBACK, m[0], SciColour(P.marginText));
    }
    Call(SCI_SETFOLDFLAGS, SC_FOLDFLAG_LINEAFTER_CONTRACTED);
    Call(SCI_SETAUTOMATICFOLD,
         SC_AUTOMATICFOLD_SHOW | SC_AUTOMATICFOLD_CLICK | SC_AUTOMATICFOLD_CHANGE);
  }
}

void SciEditor::SetLineNumbersVisible(bool b) {
  m_bLineNumbers = b;
  ConfigureMargins();
}

void SciEditor::SetFoldingEnabled(bool b) {
  m_bFolding = b;
  if (!b && IsValid()) FoldAll(false);   // never leave text hidden behind a
                                         // margin the user just turned off
  ConfigureMargins();
  StyleWholeBuffer();                    // (re)compute or drop fold levels
}

void SciEditor::FoldAll(bool bFold) {
  if (!IsValid()) return;
  Call(SCI_FOLDALL, bFold ? SC_FOLDACTION_CONTRACT : SC_FOLDACTION_EXPAND);
}

void SciEditor::Destroy() {
  if (m_hWnd && IsWindow(m_hWnd))
    DestroyWindow(m_hWnd);
  m_hWnd = NULL;
  m_fn   = nullptr;
  m_ptr  = nullptr;
}

void SciEditor::Move(int x, int y, int w, int h) {
  if (m_hWnd) MoveWindow(m_hWnd, x, y, w, h, TRUE);
}

void SciEditor::SetLanguage(CodeLang lang) {
  m_lang = lang;
  StyleWholeBuffer();
}

void SciEditor::SetFontHeightPx(int px) {
  if (px < 0) px = -px;          // CreateFontW convention: negative = char height
  if (px < 6) px = 6;
  if (!IsValid()) return;

  // Scintilla sizes styles in POINTS; the app sizes tool-window fonts in pixels.
  int dpiY = 96;
  if (HDC hdc = GetDC(m_hWnd)) {
    const int d = GetDeviceCaps(hdc, LOGPIXELSY);
    if (d > 0) dpiY = d;
    ReleaseDC(m_hWnd, hdc);
  }
  int pt = MulDiv(px, 72, dpiY);
  if (pt < 6)  pt = 6;
  if (pt > 48) pt = 48;
  m_nFontPt = pt;

  Call(SCI_STYLESETFONT, STYLE_DEFAULT, (intptr_t)"Consolas");
  Call(SCI_STYLESETSIZE, STYLE_DEFAULT, pt);
  Call(SCI_STYLECLEARALL);          // propagate the face/size to every style
  ApplyTheme(m_bDark);              // STYLECLEARALL wiped the palette

  ConfigureMargins();
}

void SciEditor::ApplyTheme(bool bDark) {
  m_bDark = bDark;
  if (!IsValid()) return;
  const Palette& P = bDark ? kDark : kLight;

  Call(SCI_STYLESETFORE, STYLE_DEFAULT, SciColour(P.text));
  Call(SCI_STYLESETBACK, STYLE_DEFAULT, SciColour(P.back));
  Call(SCI_STYLECLEARALL);

  Call(SCI_STYLESETFORE, SCI_ST_DEFAULT,   SciColour(P.text));
  Call(SCI_STYLESETFORE, SCI_ST_COMMENT,   SciColour(P.comment));
  Call(SCI_STYLESETFORE, SCI_ST_NUMBER,    SciColour(P.number));
  Call(SCI_STYLESETFORE, SCI_ST_STRING,    SciColour(P.string));
  Call(SCI_STYLESETFORE, SCI_ST_KEYWORD,   SciColour(P.keyword));
  Call(SCI_STYLESETFORE, SCI_ST_FUNCTION,  SciColour(P.function));
  Call(SCI_STYLESETFORE, SCI_ST_PRESETVAR, SciColour(P.presetvar));
  Call(SCI_STYLESETFORE, SCI_ST_OPERATOR,  SciColour(P.oper));
  Call(SCI_STYLESETFORE, SCI_ST_PREPROC,   SciColour(P.preproc));
  Call(SCI_STYLESETFORE, SCI_ST_IDENT,     SciColour(P.ident));

  Call(SCI_STYLESETBACK, STYLE_LINENUMBER, SciColour(P.marginBack));
  Call(SCI_STYLESETFORE, STYLE_LINENUMBER, SciColour(P.marginText));

  Call(SCI_SETCARETFORE, SciColour(P.caret));
  Call(SCI_SETSELBACK, 1, SciColour(P.selBack));
  Call(SCI_SETCARETLINEBACK, SciColour(P.caretLine));
  Call(SCI_SETCARETLINEVISIBLE, 1);

  StyleWholeBuffer();
}

void SciEditor::SetTextUtf8(const std::string& s) {
  if (!IsValid()) return;
  Call(SCI_SETTEXT, 0, (intptr_t)s.c_str());
  // A freshly loaded section is not an edit: no undo past the load, not dirty.
  Call(SCI_EMPTYUNDOBUFFER);
  Call(SCI_SETSAVEPOINT);
  m_bDirty = false;
  StyleWholeBuffer();
}

std::string SciEditor::GetTextUtf8() const {
  if (!IsValid()) return std::string();
  const int n = (int)Call(SCI_GETLENGTH);
  if (n <= 0) return std::string();
  // SCI_GETTEXT writes n bytes plus a terminator; in C++17 s.data() is writable
  // and s[n] is the guaranteed null slot, so n+1 is safe.
  std::string s((size_t)n, '\0');
  Call(SCI_GETTEXT, (uintptr_t)(n + 1), (intptr_t)s.data());
  return s;
}

void SciEditor::ClearModified() {
  m_bDirty = false;
  Call(SCI_SETSAVEPOINT);
}

void SciEditor::SetReadOnly(bool b) {
  Call(SCI_SETREADONLY, b ? 1 : 0);
}

int SciEditor::CurrentLine() const {
  if (!IsValid()) return 1;
  return (int)Call(SCI_LINEFROMPOSITION, (uintptr_t)Call(SCI_GETCURRENTPOS)) + 1;
}

void SciEditor::GotoLine(int line1Based) {
  if (!IsValid()) return;
  const int nLines = (int)Call(SCI_GETLINECOUNT);
  if (line1Based < 1 || line1Based > nLines) return;
  Call(SCI_GOTOLINE, (uintptr_t)(line1Based - 1));
  Call(SCI_SCROLLCARET);
}

void SciEditor::ClearErrorMarks() {
  if (!IsValid()) return;
  Call(SCI_SETINDICATORCURRENT, 0);
  Call(SCI_INDICATORCLEARRANGE, 0, Call(SCI_GETLENGTH));
}

void SciEditor::MarkErrorLine(int line1Based) {
  if (!IsValid()) return;
  const int nLines = (int)Call(SCI_GETLINECOUNT);
  if (line1Based < 1 || line1Based > nLines) return;
  const int pos = (int)Call(SCI_POSITIONFROMLINE, (uintptr_t)(line1Based - 1));
  const int len = (int)Call(SCI_LINELENGTH, (uintptr_t)(line1Based - 1));
  if (pos < 0 || len <= 0) return;
  Call(SCI_SETINDICATORCURRENT, 0);
  Call(SCI_INDICATORFILLRANGE, (uintptr_t)pos, len);
}

void SciEditor::Undo()      { Call(SCI_UNDO); }
void SciEditor::Redo()      { Call(SCI_REDO); }
void SciEditor::Cut()       { Call(SCI_CUT); }
void SciEditor::Copy()      { Call(SCI_COPY); }
void SciEditor::Paste()     { Call(SCI_PASTE); }
void SciEditor::SelectAll() { Call(SCI_SELECTALL); }
void SciEditor::ReplaceSelection(const char* utf8) {
  if (utf8) Call(SCI_REPLACESEL, 0, (intptr_t)utf8);
}
bool SciEditor::CanUndo() const { return Call(SCI_CANUNDO) != 0; }
bool SciEditor::CanRedo() const { return Call(SCI_CANREDO) != 0; }
bool SciEditor::HasSelection() const {
  return Call(SCI_GETSELECTIONSTART) != Call(SCI_GETSELECTIONEND);
}

bool SciEditor::OnNotify(NMHDR* pnm) {
  if (!pnm || !m_hWnd || pnm->hwndFrom != m_hWnd) return false;
  const SCNotification* scn = (const SCNotification*)pnm;
  switch (pnm->code) {
    case SCN_STYLENEEDED:
      StyleWholeBuffer();
      return true;
    case SCN_MODIFIED:
      if (scn->modificationType & (SC_MOD_INSERTTEXT | SC_MOD_DELETETEXT))
        m_bDirty = true;
      return true;
    case SCN_MARGINCLICK:
      // Margin 1 is the fold margin.  SC_AUTOMATICFOLD_CLICK handles the common
      // case, but a click on a header line still needs the explicit toggle.
      if (scn->margin == 1 && m_bFolding) {
        const int ln = (int)Call(SCI_LINEFROMPOSITION, (uintptr_t)scn->position);
        Call(SCI_TOGGLEFOLD, (uintptr_t)ln);
      }
      return true;
    default:
      return false;
  }
}

// Re-lexing the whole section on every request is deliberate: a preset code
// section is at most MAX_SHADER_TEXT_LEN and the tokenizer is one linear pass,
// so the cost is trivial next to the bugs that incremental line-state would buy.
void SciEditor::StyleWholeBuffer() {
  if (!IsValid()) return;
  const int len = (int)Call(SCI_GETLENGTH);
  Call(SCI_STARTSTYLING, 0);
  if (len <= 0) return;
  const std::string text = GetTextUtf8();
  if ((int)text.size() < len) return;   // defensive: never over-read
  std::vector<unsigned char> styles((size_t)len);
  LexCode(m_lang, text.c_str(), (size_t)len, styles.data());
  Call(SCI_SETSTYLINGEX, (uintptr_t)len, (intptr_t)styles.data());
  if (m_bFolding)
    ApplyFoldLevels(text, styles.data(), len);
}

void SciEditor::SetFoldMode(FoldMode m) {
  m_foldMode = m;
  StyleWholeBuffer();
}

// Brace-depth folding.  Only braces the lexer styled as operators count, so a
// '{' inside a comment or a string cannot open a fold -- which is the whole
// reason this runs off the style bytes instead of scanning the raw text.
//
// In IniSections mode a line starting with '[' is a header and everything up to
// the next header hangs off it.
void SciEditor::ApplyFoldLevels(const std::string& text, const unsigned char* styles, int len) {
  if (!IsValid() || !styles) return;

  if (m_foldMode == FoldMode::IniSections) {
    int line = 0;
    int i = 0;
    bool seenHeader = false;
    while (i <= len) {
      const int lineStart = i;
      while (i < len && text[(size_t)i] != '\n') i++;
      // First non-space character of the line.
      int f = lineStart;
      while (f < i && (text[(size_t)f] == ' ' || text[(size_t)f] == '\t')) f++;
      const bool isHeader = (f < i && text[(size_t)f] == '[');

      int level;
      if (isHeader) {
        level = SC_FOLDLEVELBASE | SC_FOLDLEVELHEADERFLAG;
        seenHeader = true;
      } else {
        level = seenHeader ? (SC_FOLDLEVELBASE + 1) : SC_FOLDLEVELBASE;
        if (lineStart == i) level |= SC_FOLDLEVELWHITEFLAG;
      }
      Call(SCI_SETFOLDLEVEL, (uintptr_t)line, level);
      line++;
      if (i >= len) break;
      i++;
    }
    return;
  }

  int line = 0;
  int depth = 0;
  int i = 0;
  while (i <= len) {
    // Accumulate this line's brace movement.
    int open = 0, close = 0;
    const int lineStart = i;
    while (i < len && text[(size_t)i] != '\n') {
      if (styles[i] == SCI_ST_OPERATOR) {
        if (text[(size_t)i] == '{') open++;
        else if (text[(size_t)i] == '}') { if (open > 0) open--; else close++; }
      }
      i++;
    }

    int level = (depth < 0 ? 0 : depth) + SC_FOLDLEVELBASE;
    // A line that closes more than it opens belongs to the outer level.
    if (close > 0) {
      level -= close;
      if (level < SC_FOLDLEVELBASE) level = SC_FOLDLEVELBASE;
    }
    if (open > 0) level |= SC_FOLDLEVELHEADERFLAG;
    if (lineStart == i && open == 0 && close == 0)
      level |= SC_FOLDLEVELWHITEFLAG;   // blank line: fold with its neighbours

    Call(SCI_SETFOLDLEVEL, (uintptr_t)line, level);

    depth += open - close;
    if (depth < 0) depth = 0;
    line++;
    if (i >= len) break;
    i++;   // step over the '\n'
  }
}

} // namespace mdrop
