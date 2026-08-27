#pragma once
// Thin RAII wrapper around one Scintilla edit control.
//
// Scintilla is linked statically (see the Scintilla ItemGroup in
// engine.vcxproj), so ScintillaDLL.cxx -- and with it DllMain -- is excluded
// from the build and the window class must be registered by hand exactly once
// per process.  RegisterScintilla() does that.
//
// Styling is container-based: Scintilla 5 dropped SCI_SETLEXER/SCLEX_CONTAINER,
// and a document with no ILexer5 attached IS the container case
// (LexInterface::UseContainerLexing() is `return !instance`).  So we simply
// never call SCI_SETILEXER, Scintilla raises SCN_STYLENEEDED, and OnNotify()
// answers it with preset_code_lexer's output.  No Lexilla dependency.
#include <windows.h>
#include <string>
#include "preset_code_lexer.h"

namespace mdrop {

class SciEditor {
public:
  SciEditor() = default;
  ~SciEditor() { Destroy(); }
  SciEditor(const SciEditor&) = delete;
  SciEditor& operator=(const SciEditor&) = delete;

  // Registers the "Scintilla" window class.  Safe to call repeatedly; only the
  // first call does work.  Returns false if registration failed.
  static bool RegisterScintilla();

  bool  Create(HWND hParent, int id, int x, int y, int w, int h);
  void  Destroy();
  HWND  Hwnd() const { return m_hWnd; }
  bool  IsValid() const { return m_hWnd != NULL && m_fn != nullptr && m_ptr != nullptr; }
  void  Move(int x, int y, int w, int h);

  void  SetLanguage(CodeLang lang);
  void  ApplyTheme(bool bDark);
  // Takes a CHARACTER HEIGHT IN PIXELS, matching the app's tool-window font
  // convention (Engine::m_nSettingsFontSize is a negative CreateFontW height).
  // Scintilla wants points, so the DPI conversion happens inside -- passing the
  // raw settings value as points is what made the first cut render at 7pt.
  void  SetFontHeightPx(int px);

  void  SetTextUtf8(const std::string& s);
  std::string GetTextUtf8() const;

  bool  IsModified() const { return m_bDirty; }
  void  ClearModified();
  void  SetReadOnly(bool b);

  int   CurrentLine() const;          // 1-based
  void  GotoLine(int line1Based);

  void  ClearErrorMarks();
  void  MarkErrorLine(int line1Based);

  // View options (persisted by the owning window).
  void  SetLineNumbersVisible(bool b);
  bool  LineNumbersVisible() const { return m_bLineNumbers; }
  // Brace-based folding.  Fold levels come from the '{' / '}' the lexer marked
  // as operators, so braces inside comments and strings do not open a fold.
  // EEL sections rarely contain braces; this mostly earns its keep in shaders.
  void  SetFoldingEnabled(bool b);
  bool  FoldingEnabled() const { return m_bFolding; }
  void  FoldAll(bool bFold);

  // What counts as a fold point.  Braces suit the shaders; IniSections folds on
  // a '[' at the start of a line, which is what the whole-preset and raw views
  // need -- those have no braces outside the shader blocks.
  enum class FoldMode { Braces, IniSections };
  void  SetFoldMode(FoldMode m);

  // Standard edit commands, for the context menu.
  void  Undo();
  void  Redo();
  void  Cut();
  void  Copy();
  void  Paste();
  void  SelectAll();
  void  ReplaceSelection(const char* utf8);
  bool  CanUndo() const;
  bool  CanRedo() const;
  bool  HasSelection() const;

  // Call from the parent's WM_NOTIFY.  Returns true if consumed.
  bool  OnNotify(NMHDR* pnm);

private:
  using SciFn = INT_PTR (*)(void*, unsigned int, uintptr_t, intptr_t);

  INT_PTR Call(unsigned int msg, uintptr_t w = 0, intptr_t l = 0) const {
    return (m_fn && m_ptr) ? m_fn(m_ptr, msg, w, l) : 0;
  }
  void StyleWholeBuffer();
  void ApplyFoldLevels(const std::string& text, const unsigned char* styles, int len);
  void ConfigureMargins();

  HWND     m_hWnd = NULL;
  SciFn    m_fn   = nullptr;
  void*    m_ptr  = nullptr;
  CodeLang m_lang = CodeLang::EEL;
  bool     m_bDirty = false;
  int      m_nFontPt = 10;
  bool     m_bDark = true;
  bool     m_bLineNumbers = true;
  bool     m_bFolding = true;
  FoldMode m_foldMode = FoldMode::Braces;
};

} // namespace mdrop
