/*
  PresetEditorWindow — syntax-highlighted editor for the RUNNING preset.

  The window edits CState's code buffers, but never touches them from this
  thread: every tool window runs its own message pump on its own thread, and
  CState, m_shaders and the DX12 PSOs belong to the render thread.  Edits are
  staged locally and pushed across with RenderCmd::ApplyPresetCode, which lands
  in Engine::ApplyPresetCodeSection() and from there in the same OnUserEdited*
  callbacks the legacy in-app menu editor has always used.

  While the window is open Engine::m_bPresetEditorOpen holds preset
  auto-advance down, so nobody loses an unsaved edit to a preset change.
*/

#include "tool_window.h"
#include "engine.h"
#include "engine_helpers.h"
#include "render_commands.h"
#include "state.h"
#include "md_defines.h"   // LINEFEED_CONTROL_CHAR
#include "preset_milk_text.h"
#include "utility.h"
#include <commdlg.h>
#include <commctrl.h>
#include "config_store.h"

namespace mdrop {

extern Engine g_engine;

// Timer used to poll for the render thread's compile result after an Apply.
static const UINT_PTR kApplyPollTimer = 1;
// Watches for the preset changing underneath the editor. Auto-advance is
// suppressed while the window is open, but the user can still load a preset
// from the Presets window -- and applying text staged against the old one would
// write it into the new preset.
static const UINT_PTR kPresetWatchTimer = 2;

namespace {

// CState packs each code section into a single C string, using
// LINEFEED_CONTROL_CHAR (ASCII 1) where the .milk file had a line break.
// Scintilla renders an unknown control character as a boxed mnemonic, so
// untranslated text shows up as one endless line strung together with "SOH"
// boxes. Translate at the editor boundary, both ways.
// A .milk2 is a wrapper holding TWO complete presets between [PRESET1_BEGIN]
// /[PRESET1_END] and [PRESET2_BEGIN]/[PRESET2_END], plus the blend header
// (ParseMilk2File, engine_presets.cpp:2153).  CState holds only one preset --
// on a .milk2 load it is preset 2, the blend-to one -- so CState::Export can
// never reproduce a .milk2 and must not be written over one.
bool IsMilk2Path(const wchar_t* p) {
  if (!p || !*p) return false;
  const wchar_t* dot = wcsrchr(p, L'.');
  return dot && _wcsicmp(dot, L".milk2") == 0;
}

std::string ReadWholeFileA(const wchar_t* path) {
  FILE* f = nullptr;
  if (!path || !*path || _wfopen_s(&f, path, L"rb") != 0 || !f) return std::string();
  fseek(f, 0, SEEK_END);
  const long n = ftell(f);
  fseek(f, 0, SEEK_SET);
  std::string s((size_t)(n > 0 ? n : 0), '\0');
  if (n > 0 && fread(&s[0], 1, (size_t)n, f) != (size_t)n) s.clear();
  fclose(f);
  return s;
}

std::string LfcToLines(const std::string& s) {
  std::string out;
  out.reserve(s.size());
  for (char c : s) {
    if (c == LINEFEED_CONTROL_CHAR) out.push_back('\n');
    else if (c != '\r')             out.push_back(c);
  }
  return out;
}

std::string LinesToLfc(const std::string& s) {
  std::string out;
  out.reserve(s.size());
  for (size_t i = 0; i < s.size(); i++) {
    const char c = s[i];
    if (c == '\r') {
      if (i + 1 < s.size() && s[i + 1] == '\n') i++;   // CRLF -> one break
      out.push_back((char)LINEFEED_CONTROL_CHAR);
    } else if (c == '\n') {
      out.push_back((char)LINEFEED_CONTROL_CHAR);
    } else {
      out.push_back(c);
    }
  }
  return out;
}

// Put each statement on its own line by breaking after every top-level ';'.
// Semicolons inside parentheses (a for-header, a call's arguments), inside a
// string, or inside a comment are left alone -- breaking those would change or
// break the code rather than just re-wrap it.
std::string ExpandStatements(const std::string& s) {
  std::string out;
  out.reserve(s.size() + s.size() / 8);
  int depth = 0;
  bool inLineComment = false, inBlockComment = false, inString = false;

  for (size_t i = 0; i < s.size(); i++) {
    const char c = s[i];
    const char n = (i + 1 < s.size()) ? s[i + 1] : '\0';

    if (inLineComment) {
      out.push_back(c);
      if (c == '\n') inLineComment = false;
      continue;
    }
    if (inBlockComment) {
      out.push_back(c);
      if (c == '*' && n == '/') { out.push_back(n); i++; inBlockComment = false; }
      continue;
    }
    if (inString) {
      out.push_back(c);
      if (c == '\\' && n) { out.push_back(n); i++; }
      else if (c == '"' || c == '\n') inString = false;
      continue;
    }

    if (c == '/' && n == '/') { out.append("//"); i++; inLineComment = true; continue; }
    if (c == '/' && n == '*') { out.append("/*"); i++; inBlockComment = true; continue; }
    if (c == '"')             { out.push_back(c); inString = true; continue; }

    if (c == '(' || c == '[') depth++;
    if ((c == ')' || c == ']') && depth > 0) depth--;

    out.push_back(c);

    if (c == ';' && depth == 0) {
      // Skip trailing spaces/tabs, then break unless a break is already there.
      size_t j = i + 1;
      while (j < s.size() && (s[j] == ' ' || s[j] == '\t')) j++;
      if (j < s.size() && s[j] != '\n' && s[j] != '\r') {
        out.push_back('\n');
        i = j - 1;   // drop the run of spaces we just skipped
      }
    }
  }
  return out;
}

} // namespace

//----------------------------------------------------------------------
// Construction / destruction
//----------------------------------------------------------------------

PresetEditorWindow::PresetEditorWindow(Engine* pEngine)
  : ToolWindow(pEngine, 900, 640) {
  BuildSectionList();
  m_staged.resize(m_sections.size());
  m_dirty.assign(m_sections.size(), false);
  LoadViewOptions();
}

PresetEditorWindow::~PresetEditorWindow() {
  // Belt and braces with DoDestroy(): the unique_ptr on Engine outlives the
  // HWND, so whichever runs first must clear the auto-advance lock.
  if (m_pEngine)
    m_pEngine->m_bPresetEditorOpen.store(false, std::memory_order_relaxed);
}

void PresetEditorWindow::DoDestroy() {
  KillTimer(m_hWnd, kApplyPollTimer);
  KillTimer(m_hWnd, kPresetWatchTimer);
  m_sci.Destroy();
  m_hFallbackEdit = NULL;
  if (m_pEngine)
    m_pEngine->m_bPresetEditorOpen.store(false, std::memory_order_relaxed);
}

//----------------------------------------------------------------------
// Section table
//----------------------------------------------------------------------

void PresetEditorWindow::BuildSectionList() {
  m_sections.clear();
  auto add = [&](const std::wstring& label, int section, int index, CodeLang lang) {
    m_sections.push_back({label, section, index, lang});
  };

  // Rows 0 and 1 are the whole-file views. They must stay first: PopulateTree
  // relies on their indices for the two root items.
  add(L"Whole Preset", PSEUDO_WHOLE_PRESET, 0, CodeLang::EEL);
  add(L"Raw File",     PSEUDO_RAW_FILE,     0, CodeLang::EEL);

  add(L"Init",       PCS_PRESET_INIT, 0, CodeLang::EEL);
  add(L"Per-Frame",  PCS_PER_FRAME,   0, CodeLang::EEL);
  add(L"Per-Pixel",  PCS_PER_PIXEL,   0, CodeLang::EEL);
  add(L"Warp",       PCS_WARP_SHADER, 0, CodeLang::HLSL);
  add(L"Comp",       PCS_COMP_SHADER, 0, CodeLang::HLSL);

  wchar_t buf[64];
  for (int i = 0; i < MAX_CUSTOM_WAVES; i++) {
    swprintf_s(buf, L"Init");          add(buf, PCS_WAVE_INIT,      i, CodeLang::EEL);
    swprintf_s(buf, L"Per-Frame");     add(buf, PCS_WAVE_PER_FRAME, i, CodeLang::EEL);
    swprintf_s(buf, L"Per-Point");     add(buf, PCS_WAVE_PER_POINT, i, CodeLang::EEL);
  }
  // CShape has no per-point buffer (it is commented out in state.h), so shapes
  // contribute two sections each, not three.
  for (int i = 0; i < MAX_CUSTOM_SHAPES; i++) {
    swprintf_s(buf, L"Init");          add(buf, PCS_SHAPE_INIT,      i, CodeLang::EEL);
    swprintf_s(buf, L"Per-Frame");     add(buf, PCS_SHAPE_PER_FRAME, i, CodeLang::EEL);
  }
}

bool PresetEditorWindow::CurrentIsWholeFile() const {
  if (m_nCurRow < 0 || m_nCurRow >= (int)m_sections.size()) return false;
  const int s = m_sections[m_nCurRow].section;
  return s == PSEUDO_WHOLE_PRESET || s == PSEUDO_RAW_FILE;
}

DWORD PresetEditorWindow::GetCommonControlFlags() const {
  return ToolWindow::GetCommonControlFlags() | ICC_TREEVIEW_CLASSES;
}

//----------------------------------------------------------------------
// Tree navigator
//----------------------------------------------------------------------

// 85 flat rows was a wall of text. The tree groups them and starts with the
// waves and shapes collapsed, so what you see is the handful of sections almost
// every preset actually uses.
// A frozen .milk2 has two presets on screen at once, so the editor needs to say
// which one an edit means. Everything else has exactly one preset and no tabs.
void PresetEditorWindow::SyncSideTabs() {
  const bool bTwo = m_pEngine && m_pEngine->HasEditableBlendFromPreset();
  m_bTwoPresets = bTwo;
  if (!bTwo) m_nSide = PSIDE_LIVE;
  if (m_hSideTabs) ShowWindow(m_hSideTabs, bTwo ? SW_SHOW : SW_HIDE);
}

// Switching preset drops every staged edit: they were staged against the other
// preset's buffers and applying them here would write them to the wrong one.
void PresetEditorWindow::OnSideChanged(int nSide) {
  if (nSide == m_nSide) return;
  int nPending = 0;
  for (size_t i = 0; i < m_dirty.size(); i++) if (m_dirty[i]) nPending++;

  m_nSide = nSide;
  m_staged.assign(m_sections.size(), std::string());
  m_dirty.assign(m_sections.size(), false);

  const int keep = m_nCurRow;
  m_nCurRow = -1;                 // do not stash into the preset we just left
  LoadSectionIntoEditor(keep < 0 ? 0 : keep);

  if (nPending > 0)
    SetStatus(L"Switched preset - unapplied edits to the other one were discarded.");
}

void PresetEditorWindow::PopulateTree() {
  if (!m_hTree) return;
  TreeView_DeleteAllItems(m_hTree);

  auto addItem = [&](HTREEITEM parent, const wchar_t* text, int rowIndex) -> HTREEITEM {
    TVINSERTSTRUCTW ins = {};
    ins.hParent = parent ? parent : TVI_ROOT;
    ins.hInsertAfter = TVI_LAST;
    ins.item.mask = TVIF_TEXT | TVIF_PARAM;
    ins.item.pszText = (LPWSTR)text;
    ins.item.lParam = (LPARAM)rowIndex;    // -1 for a pure group node
    return TreeView_InsertItem(m_hTree, &ins);
  };

  // Row indices follow BuildSectionList's order exactly.
  const int kWholeRow = 0, kRawRow = 1, kFirstCode = 2;
  HTREEITEM hWhole = addItem(NULL, m_sections[kWholeRow].label.c_str(), kWholeRow);
  addItem(NULL, m_sections[kRawRow].label.c_str(), kRawRow);

  HTREEITEM hPreset = addItem(NULL, L"Preset", -1);
  addItem(hPreset, m_sections[kFirstCode + 0].label.c_str(), kFirstCode + 0);  // Init
  addItem(hPreset, m_sections[kFirstCode + 1].label.c_str(), kFirstCode + 1);  // Per-Frame
  addItem(hPreset, m_sections[kFirstCode + 2].label.c_str(), kFirstCode + 2);  // Per-Pixel

  HTREEITEM hShaders = addItem(NULL, L"Shaders", -1);
  addItem(hShaders, m_sections[kFirstCode + 3].label.c_str(), kFirstCode + 3); // Warp
  addItem(hShaders, m_sections[kFirstCode + 4].label.c_str(), kFirstCode + 4); // Comp

  int row = kFirstCode + 5;
  HTREEITEM hWaves = addItem(NULL, L"Waves", -1);
  for (int i = 0; i < MAX_CUSTOM_WAVES; i++) {
    wchar_t buf[32];
    swprintf_s(buf, L"Wave %d", i + 1);
    HTREEITEM hW = addItem(hWaves, buf, -1);
    for (int k = 0; k < 3; k++, row++)
      addItem(hW, m_sections[row].label.c_str(), row);
  }

  HTREEITEM hShapes = addItem(NULL, L"Shapes", -1);
  for (int i = 0; i < MAX_CUSTOM_SHAPES; i++) {
    wchar_t buf[32];
    swprintf_s(buf, L"Shape %d", i + 1);
    HTREEITEM hS = addItem(hShapes, buf, -1);
    for (int k = 0; k < 2; k++, row++)
      addItem(hS, m_sections[row].label.c_str(), row);
  }

  // Preset and Shaders open; Waves and Shapes stay shut until asked for.
  TreeView_Expand(m_hTree, hPreset, TVE_EXPAND);
  TreeView_Expand(m_hTree, hShaders, TVE_EXPAND);
  TreeView_SelectItem(m_hTree, hWhole);
}

//----------------------------------------------------------------------
// Reading the live preset
//----------------------------------------------------------------------

// Read-only peek at CState from this thread.  Tolerated race: the render thread
// only rewrites these buffers during a preset load, and while this window is
// open auto-advance is suppressed.  If a torn read is ever seen, add a
// GetPresetCodeSection render command -- do NOT put a mutex around CState.
std::string PresetEditorWindow::ReadSectionFromState(const SectionRow& r) const {
  if (!m_pEngine) return std::string();
  // On a frozen .milk2, preset 1 is m_pOldState and renders alongside preset 2.
  const CState* s = (m_nSide == PSIDE_BLENDFROM && m_pEngine->HasEditableBlendFromPreset())
                      ? m_pEngine->m_pOldState
                      : m_pEngine->m_pState;
  if (!s) return std::string();
  const char* src = nullptr;
  switch (r.section) {
    case PCS_PRESET_INIT: src = s->m_szPerFrameInit; break;
    case PCS_PER_FRAME:   src = s->m_szPerFrameExpr; break;
    case PCS_PER_PIXEL:   src = s->m_szPerPixelExpr; break;
    case PCS_WAVE_INIT:
    case PCS_WAVE_PER_FRAME:
    case PCS_WAVE_PER_POINT:
      if (r.index < 0 || r.index >= MAX_CUSTOM_WAVES) return std::string();
      src = (r.section == PCS_WAVE_INIT)      ? s->m_wave[r.index].m_szInit
          : (r.section == PCS_WAVE_PER_FRAME) ? s->m_wave[r.index].m_szPerFrame
                                              : s->m_wave[r.index].m_szPerPoint;
      break;
    case PCS_SHAPE_INIT:
    case PCS_SHAPE_PER_FRAME:
      if (r.index < 0 || r.index >= MAX_CUSTOM_SHAPES) return std::string();
      src = (r.section == PCS_SHAPE_INIT) ? s->m_shape[r.index].m_szInit
                                          : s->m_shape[r.index].m_szPerFrame;
      break;
    case PCS_WARP_SHADER: src = s->m_szWarpShadersText; break;
    case PCS_COMP_SHADER: src = s->m_szCompShadersText; break;
    default: return std::string();
  }
  if (!src) return std::string();
  return LfcToLines(std::string(src));
}

// The whole preset as text.  Exported to a scratch file rather than assembled
// by hand: CState::Export is the only thing that knows every scalar it writes,
// and keeping one serializer means the editor can never drift from the format.
std::string PresetEditorWindow::BuildWholePresetText(bool bBlocks) const {
  if (!m_pEngine || !m_pEngine->m_pState) return std::string();

  // Raw File on a .milk2 shows the real file: Export could only ever produce
  // preset 2, so calling that "raw" would be a lie. Apply is refused for it in
  // EnqueueApply, since Import cannot consume the wrapper.
  if (!bBlocks && IsMilk2Path(m_pEngine->m_szCurrentPresetFile))
    return ReadWholeFileA(m_pEngine->m_szCurrentPresetFile);

  // Whole Preset for preset 1 of a .milk2 exports m_pOldState instead.
  const bool bOld = (m_nSide == PSIDE_BLENDFROM) && m_pEngine->HasEditableBlendFromPreset();
  CState* pExport = bOld ? m_pEngine->m_pOldState : m_pEngine->m_pState;
  if (!pExport) return std::string();

  // NOT m_szMilkdrop2Path -- that points at resources\, which has no log dir.
  // DebugLogDiagPath resolves into the log/ directory DebugLogInit creates.
  wchar_t szTemp[MAX_PATH];
  DebugLogDiagPath(L"_editor_view.milk", szTemp, MAX_PATH);
  if (!pExport->Export(szTemp)) return std::string();

  std::string text;
  {
    FILE* f = nullptr;
    if (_wfopen_s(&f, szTemp, L"rb") != 0 || !f) return std::string();
    fseek(f, 0, SEEK_END);
    const long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (n > 0) {
      text.resize((size_t)n);
      if (fread(&text[0], 1, (size_t)n, f) != (size_t)n) text.clear();
    }
    fclose(f);
  }
  if (text.empty()) return text;

  // Export drops the embedded [SPRITEn_BEGIN] blocks, which live in the file
  // rather than in CState. Splice them back from the preset on disk so the
  // whole-file views show, and can round-trip, what is actually in the preset.
  {
    const std::string disk = ReadWholeFileA(m_pEngine->m_szCurrentPresetFile);
    const size_t at = disk.find("[SPRITE");
    if (at != std::string::npos) {
      if (!text.empty() && text.back() != '\n') text += '\n';
      text += disk.substr(at);
    }
  }

  return bBlocks ? MilkToBlocks(text) : text;
}

//----------------------------------------------------------------------
// Controls
//----------------------------------------------------------------------

void PresetEditorWindow::DoBuildControls() {
  HWND hw = m_hWnd;
  if (!hw) return;

  // Re-acquire the lock here too: Open() on an already-constructed window
  // rebuilds controls without running the constructor again.
  if (m_pEngine)
    m_pEngine->m_bPresetEditorOpen.store(true, std::memory_order_relaxed);

  auto L = BuildBaseControls();
  m_nTopY = L.y;
  HFONT hFont = GetFont();

  RECT rc;
  GetClientRect(hw, &rc);

  const int x = L.x, rw = L.rw, lineH = L.lineH, gap = L.gap;
  const int listW  = MulDiv(170, lineH, 26);
  const int btnH   = lineH + 4;
  const int statusH = lineH + 2;

  int editX = x + listW + gap;
  int editW = rw - listW - gap;
  int editY = m_nTopY;
  int editH = rc.bottom - editY - (btnH + gap) - (statusH + gap) - gap;
  if (editH < 80) editH = 80;

  // Preset 1 / Preset 2 selector, above the tree. Created always so the layout
  // is stable, shown only when a frozen .milk2 makes two presets meaningful.
  const int tabH = lineH + 8;
  m_hSideTabs = CreateWindowExW(0, WC_TABCONTROLW, L"",
                                WS_CHILD | WS_TABSTOP,
                                x, editY, listW, tabH, hw,
                                (HMENU)(INT_PTR)IDC_MW_PEDIT_SIDETABS, NULL, NULL);
  TrackControl(m_hSideTabs);
  if (m_hSideTabs) {
    SendMessageW(m_hSideTabs, WM_SETFONT, (WPARAM)hFont, TRUE);
    TCITEMW ti = {};
    ti.mask = TCIF_TEXT;
    ti.pszText = (LPWSTR)L"Preset 1";
    TabCtrl_InsertItem(m_hSideTabs, 0, &ti);
    ti.pszText = (LPWSTR)L"Preset 2";
    TabCtrl_InsertItem(m_hSideTabs, 1, &ti);
    TabCtrl_SetCurSel(m_hSideTabs, m_nSide == PSIDE_BLENDFROM ? 0 : 1);
  }
  SyncSideTabs();
  // The tabs sit in the tree column only; the editor keeps the full height.
  int treeY = editY, treeH = editH;
  if (m_bTwoPresets) { treeY += tabH + 2; treeH -= tabH + 2; }

  // Section tree
  m_hTree = CreateWindowExW(WS_EX_CLIENTEDGE, WC_TREEVIEWW, L"",
                            WS_CHILD | WS_VISIBLE | WS_TABSTOP |
                            TVS_HASBUTTONS | TVS_HASLINES | TVS_LINESATROOT |
                            TVS_SHOWSELALWAYS,
                            x, treeY, listW, treeH, hw,
                            (HMENU)(INT_PTR)IDC_MW_PEDIT_SECTIONS, NULL, NULL);
  TrackControl(m_hTree);
  if (m_hTree) {
    SendMessageW(m_hTree, WM_SETFONT, (WPARAM)hFont, TRUE);
    if (m_pEngine && m_pEngine->IsDarkTheme()) {
      TreeView_SetBkColor(m_hTree, m_pEngine->m_colSettingsBg);
      TreeView_SetTextColor(m_hTree, m_pEngine->m_colSettingsText);
      TreeView_SetLineColor(m_hTree, m_pEngine->m_colSettingsText);
    }
    PopulateTree();
  }

  // Editor
  if (m_sci.Create(hw, IDC_MW_PEDIT_SCI, editX, editY, editW, editH)) {
    TrackControl(m_sci.Hwnd());
    m_sci.ApplyTheme(m_pEngine && m_pEngine->IsDarkTheme());
    // m_nSettingsFontSize is a CreateFontW height (negative = character height
    // in pixels); SetFontHeightPx does the pixels-to-points conversion.
    m_sci.SetFontHeightPx(m_pEngine ? m_pEngine->m_nSettingsFontSize : -20);
    // Order matters: folding first, since it re-runs the styling pass that
    // computes fold levels, then line numbers to size margin 0.
    m_sci.SetFoldingEnabled(m_bOptFolding);
    m_sci.SetLineNumbersVisible(m_bOptLineNumbers);
  } else {
    // Degrade rather than refuse to open: a plain edit box still lets the user
    // read and change the code, just without highlighting.
    m_hFallbackEdit = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_MULTILINE | ES_AUTOVSCROLL |
        ES_WANTRETURN | WS_VSCROLL | WS_HSCROLL | ES_NOHIDESEL,
        editX, editY, editW, editH, hw,
        (HMENU)(INT_PTR)IDC_MW_PEDIT_SCI, NULL, NULL);
    TrackControl(m_hFallbackEdit);
    if (m_hFallbackEdit) {
      SendMessageW(m_hFallbackEdit, WM_SETFONT, (WPARAM)hFont, TRUE);
      SendMessageW(m_hFallbackEdit, EM_SETLIMITTEXT, MAX_SHADER_TEXT_LEN, 0);
    }
  }

  // Button row
  {
    // The four actions that change or persist something. Expand and Templates
    // are editing aids and live in the right-click menu; Apply All is gone
    // because Apply covers it. Save As stays a button: it is a distinct
    // destination, and burying it in a menu that only opens over the code pane
    // made it effectively unreachable.
    int by = editY + editH + gap;
    int bw = MulDiv(92, lineH, 26);
    int bx = x;
    m_hApply  = CreateBtn(hw, L"Apply",      IDC_MW_PEDIT_APPLY,  bx, by, bw, btnH, hFont); bx += bw + 4;
    m_hRevert = CreateBtn(hw, L"Revert",     IDC_MW_PEDIT_REVERT, bx, by, bw, btnH, hFont); bx += bw + 4;
    m_hSave   = CreateBtn(hw, L"Save",       IDC_MW_PEDIT_SAVE,   bx, by, bw, btnH, hFont); bx += bw + 4;
    m_hSaveAs = CreateBtn(hw, L"Save As...", IDC_MW_PEDIT_SAVEAS, bx, by, bw, btnH, hFont);
    TrackControl(m_hApply); TrackControl(m_hRevert);
    TrackControl(m_hSave);  TrackControl(m_hSaveAs);

    HWND hTip = CreateWindowExW(WS_EX_TOPMOST, TOOLTIPS_CLASSW, NULL,
        WS_POPUP | TTS_ALWAYSTIP | TTS_NOPREFIX,
        CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT,
        hw, NULL, GetModuleHandle(NULL), NULL);
    TrackTooltip(hTip);
    if (hTip && m_hApply) {
      TTTOOLINFOW ti = { sizeof(ti) };
      ti.uFlags = TTF_SUBCLASS | TTF_IDISHWND;
      ti.hwnd = hw;
      ti.uId = (UINT_PTR)m_hApply;
      ti.lpszText = (LPWSTR)L"Run the edit now (right-click for more)";
      SendMessageW(hTip, TTM_ADDTOOLW, 0, (LPARAM)&ti);
    }

    // Status strip
    int sy = by + btnH + gap;
    m_hStatus = CreateEdit(hw, L"", IDC_MW_PEDIT_STATUS, x, sy, rw, statusH, hFont,
                           ES_READONLY | ES_AUTOHSCROLL);
    TrackControl(m_hStatus);
  }

  if (m_pEngine) m_wWatchedPreset = m_pEngine->m_szCurrentPresetFile;
  SetTimer(hw, kPresetWatchTimer, 1000, NULL);

  LoadSectionIntoEditor(m_nCurRow);
}

// Reposition rather than rebuild: a rebuild would destroy the Scintilla control
// and with it the undo history and caret position.
void PresetEditorWindow::LayoutControls() {
  HWND hw = m_hWnd;
  if (!hw) return;

  RECT rc;
  GetClientRect(hw, &rc);

  const int lineH = GetLineHeight();
  const int gap = 6, x = 16;
  const int rw = rc.right - x * 2;
  const int listW = MulDiv(170, lineH, 26);
  const int btnH = lineH + 4;
  const int statusH = lineH + 2;

  int editX = x + listW + gap;
  int editW = rw - listW - gap;
  int editY = m_nTopY;
  int editH = rc.bottom - editY - (btnH + gap) - (statusH + gap) - gap;
  if (editH < 80) editH = 80;

  const int tabH = lineH + 8;
  int treeY = editY, treeH = editH;
  if (m_bTwoPresets) { treeY += tabH + 2; treeH -= tabH + 2; }
  if (m_hSideTabs) MoveWindow(m_hSideTabs, x, editY, listW, tabH, TRUE);
  if (m_hTree) MoveWindow(m_hTree, x, treeY, listW, treeH, TRUE);
  if (m_sci.IsValid()) m_sci.Move(editX, editY, editW, editH);
  else if (m_hFallbackEdit) MoveWindow(m_hFallbackEdit, editX, editY, editW, editH, TRUE);

  int by = editY + editH + gap;
  int bw = MulDiv(92, lineH, 26);
  int bx = x;
  HWND btns[4] = { m_hApply, m_hRevert, m_hSave, m_hSaveAs };
  for (HWND b : btns) {
    if (b) MoveWindow(b, bx, by, bw, btnH, TRUE);
    bx += bw + 4;
  }
  if (m_hStatus) MoveWindow(m_hStatus, x, by + btnH + gap, rw, statusH, TRUE);
}

// Reposition only.  The base class's OnResize calls RebuildFonts(), which
// destroys every child window -- including the Scintilla control, taking the
// undo history and caret with it.  Resizing an editor must not do that.
void PresetEditorWindow::OnResize() {
  LayoutControls();
}

// The font +/- buttons DO go through a full rebuild, and there is no way round
// that: the shared font is recreated and every control has to be remade. Stash
// the current section first so an unsaved edit survives the trip -- the rebuild
// ends in DoBuildControls -> LoadSectionIntoEditor, which restores from it.
void PresetEditorWindow::RebuildFonts() {
  StashEditorText();
  ToolWindow::RebuildFonts();
}

//----------------------------------------------------------------------
// Staging / loading
//----------------------------------------------------------------------

void PresetEditorWindow::StashEditorText() {
  if (m_nCurRow < 0 || m_nCurRow >= (int)m_sections.size()) return;
  if (m_sci.IsValid()) {
    if (!m_sci.IsModified()) return;
    m_staged[m_nCurRow] = m_sci.GetTextUtf8();
    m_dirty[m_nCurRow] = true;
    m_sci.ClearModified();
  } else if (m_hFallbackEdit) {
    const int len = GetWindowTextLengthW(m_hFallbackEdit);
    std::wstring w((size_t)len + 1, L'\0');
    GetWindowTextW(m_hFallbackEdit, &w[0], len + 1);
    w.resize((size_t)len);
    std::string narrow;
    narrow.reserve(w.size());
    for (wchar_t c : w) narrow.push_back(c < 128 ? (char)c : '?');
    if (narrow != m_staged[m_nCurRow]) {
      m_staged[m_nCurRow] = narrow;
      m_dirty[m_nCurRow] = true;
    }
  }
}

void PresetEditorWindow::LoadSectionIntoEditor(int nRow) {
  if (nRow < 0 || nRow >= (int)m_sections.size()) return;
  StashEditorText();
  m_nCurRow = nRow;

  const SectionRow& r = m_sections[nRow];
  std::string text;
  if (m_dirty[nRow]) {
    text = m_staged[nRow];
  } else if (r.section == PSEUDO_WHOLE_PRESET) {
    text = BuildWholePresetText(true);
  } else if (r.section == PSEUDO_RAW_FILE) {
    text = BuildWholePresetText(false);
  } else {
    text = ReadSectionFromState(r);
  }

  if (m_sci.IsValid()) {
    m_sci.SetLanguage(r.lang);
    // The whole-file views fold on [section] headers; a code section folds on
    // braces, which only the shaders really have.
    m_sci.SetFoldMode(CurrentIsWholeFile() ? SciEditor::FoldMode::IniSections
                                           : SciEditor::FoldMode::Braces);
    m_sci.SetTextUtf8(text);
    m_sci.ClearErrorMarks();
  } else if (m_hFallbackEdit) {
    std::wstring w(text.begin(), text.end());
    SetWindowTextW(m_hFallbackEdit, w.c_str());
  }

  // A .milk2 holds two presets; CState has only the second, so say so rather
  // than let "Whole Preset" imply the whole file.
  const bool bMilk2 = m_pEngine && IsMilk2Path(m_pEngine->m_szCurrentPresetFile);
  const wchar_t* szWhich = !m_bTwoPresets ? L""
                         : (m_nSide == PSIDE_BLENDFROM) ? L"preset 1 (blend-from)"
                                                        : L"preset 2 (blend-to)";
  wchar_t buf[260];
  if (r.section == PSEUDO_WHOLE_PRESET) {
    if (m_bTwoPresets)
      swprintf_s(buf, L"Whole Preset - %s of this .milk2, code under [section] headers", szWhich);
    else
      swprintf_s(buf, L"Whole Preset - the whole preset, code grouped under [section] headers");
  } else if (r.section == PSEUDO_RAW_FILE) {
    swprintf_s(buf, bMilk2
      ? L"Raw File - the whole .milk2 wrapper, both presets. Read-only: Apply cannot import it."
      : L"Raw File - the .milk file exactly as written");
  } else {
    if (m_bTwoPresets) swprintf_s(buf, L"%s  -  %s", r.label.c_str(), szWhich);
    else               swprintf_s(buf, L"%s", r.label.c_str());
  }
  SetStatus(buf);
}

//----------------------------------------------------------------------
// Apply / revert / save
//----------------------------------------------------------------------

bool PresetEditorWindow::EnqueueApply(int nRow) {
  if (!m_pEngine) return false;
  if (nRow < 0 || nRow >= (int)m_sections.size()) return false;
  const SectionRow& r = m_sections[nRow];

  // Whole-file views replace the entire state, scalars included, so they go
  // through Import rather than the per-section buffer copy.
  if (r.section == PSEUDO_WHOLE_PRESET || r.section == PSEUDO_RAW_FILE) {
    // Raw File on a .milk2 is the two-preset wrapper, which CState::Import
    // cannot read. Applying it would import garbage; edit the sections, or
    // Whole Preset, which is preset 2 on its own.
    if (r.section == PSEUDO_RAW_FILE && m_pEngine &&
        IsMilk2Path(m_pEngine->m_szCurrentPresetFile)) {
      SetStatus(L"Raw File is the whole .milk2 wrapper and cannot be applied - "
                L"use Whole Preset (that is preset 2) or the individual sections.");
      return false;
    }
    const std::string editorText =
        m_dirty[nRow] ? m_staged[nRow]
                      : BuildWholePresetText(r.section == PSEUDO_WHOLE_PRESET);
    const std::string milk = (r.section == PSEUDO_WHOLE_PRESET)
                               ? BlocksToMilk(editorText)
                               : editorText;
    if (milk.empty()) {
      SetStatus(L"Nothing to apply - the preset text is empty.");
      return false;
    }
    RenderCommand cmd;
    cmd.cmd = RenderCmd::ApplyPresetText;
    cmd.iParam1 = m_nSide;                  // preset 1 or 2 of a .milk2
    cmd.sParam.assign(milk.begin(), milk.end());
    m_pEngine->EnqueueRenderCmd(std::move(cmd));
    return true;
  }

  // Bind by value: ReadSectionFromState returns a temporary.
  const std::string editorText = m_dirty[nRow] ? m_staged[nRow] : ReadSectionFromState(r);
  // Back to CState's own encoding: real newlines become LINEFEED_CONTROL_CHAR.
  const std::string code = LinesToLfc(editorText);

  RenderCommand cmd;
  cmd.cmd     = RenderCmd::ApplyPresetCode;
  cmd.iParam1 = r.section;
  cmd.iParam2 = r.index;
  cmd.iParam3 = m_nSide;                         // preset 1 or 2 of a .milk2
  cmd.sParam.assign(code.begin(), code.end());   // preset code is ASCII
  m_pEngine->EnqueueRenderCmd(std::move(cmd));
  return true;
}

// Explicit, never automatic: this rewrites the user's text, so it happens only
// when they ask.  Revert undoes it, and so does Ctrl+Z.
void PresetEditorWindow::ExpandCurrentSection() {
  if (m_nCurRow < 0 || m_nCurRow >= (int)m_sections.size()) return;
  if (!m_sci.IsValid()) {
    SetStatus(L"Expand needs the syntax editor.");
    return;
  }
  const std::string before = m_sci.GetTextUtf8();
  const std::string after  = ExpandStatements(before);
  if (after == before) {
    SetStatus(L"Already one statement per line.");
    return;
  }
  m_sci.SetTextUtf8(after);
  // SetTextUtf8 clears the dirty flag (it is meant for loading a section), but
  // this IS an edit -- stage it so Apply and Save pick it up.
  m_staged[m_nCurRow] = after;
  m_dirty[m_nCurRow] = true;
  SetStatus(L"Expanded. Apply to run it, or Revert to undo.");
}

// Apply pushes the current view. From a whole-file view that is the entire
// preset; from a code section it is just that section, plus any other section
// left modified -- so one button covers what Apply and Apply All used to.
void PresetEditorWindow::ApplyCurrentSection() {
  StashEditorText();

  int nQueued = 0;
  if (CurrentIsWholeFile()) {
    if (EnqueueApply(m_nCurRow)) nQueued++;
  } else {
    if (EnqueueApply(m_nCurRow)) nQueued++;
    for (int i = 0; i < (int)m_sections.size(); i++) {
      if (i == m_nCurRow || !m_dirty[i]) continue;
      const int s = m_sections[i].section;
      if (s == PSEUDO_WHOLE_PRESET || s == PSEUDO_RAW_FILE) continue;  // never mix
      if (EnqueueApply(i)) nQueued++;
    }
  }

  // If nothing went to the render thread, EnqueueApply has already put the
  // reason on the status strip. Saying "Applying..." over the top of it, and
  // then "Applied." when the poll finds no error, would report a success that
  // never happened.
  if (nQueued == 0) return;

  SetStatus(L"Applying...");
  // The recompile runs on the render thread a frame or two from now, so poll
  // for its result rather than reading m_wLastShaderError straight away.
  SetTimer(m_hWnd, kApplyPollTimer, 250, NULL);
}

void PresetEditorWindow::RevertCurrentSection() {
  if (m_nCurRow < 0 || m_nCurRow >= (int)m_sections.size()) return;
  m_dirty[m_nCurRow] = false;
  m_staged[m_nCurRow].clear();
  const int keep = m_nCurRow;
  m_nCurRow = -1;              // stop LoadSectionIntoEditor stashing what we just dropped
  LoadSectionIntoEditor(keep);
  SetStatus(L"Reverted to the running preset.");
}

//----------------------------------------------------------------------
// Templates
//----------------------------------------------------------------------

namespace {

struct Template { int id; const wchar_t* label; const char* body; };

// Starter snippets, offered per section kind. Deliberately short: they are a
// reminder of the idiom, not a preset.
const Template kTplPerFrame[] = {
  { IDC_MW_PEDIT_TPL_BASE + 0, L"Beat-reactive zoom",
    "// pulse the zoom on bass\n"
    "vol = (bass + mid + treb) * 0.333;\n"
    "zoom = 1.0 + 0.02 * bass_att;\n" },
  { IDC_MW_PEDIT_TPL_BASE + 1, L"Slow rotation",
    "rot = rot + 0.003 * sin(time * 0.31);\n" },
  { IDC_MW_PEDIT_TPL_BASE + 2, L"Smoothed q-vars for the shader",
    "q1 = bass_att;\n"
    "q2 = mid_att;\n"
    "q3 = treb_att;\n" },
};

const Template kTplPerPixel[] = {
  { IDC_MW_PEDIT_TPL_BASE + 10, L"Radial zoom falloff",
    "zoom = zoom + 0.02 * (1.0 - rad);\n" },
  { IDC_MW_PEDIT_TPL_BASE + 11, L"Angular swirl",
    "rot = rot + 0.02 * sin(ang * 3 + time);\n" },
};

const Template kTplInit[] = {
  { IDC_MW_PEDIT_TPL_BASE + 20, L"Zero a megabuf range",
    "n = 0;\n"
    "loop (4096, megabuf(n) = 0; n = n + 1);\n" },
};

const Template kTplWave[] = {
  { IDC_MW_PEDIT_TPL_BASE + 30, L"Circle",
    "t = sample * 6.2831853;\n"
    "x = 0.5 + 0.3 * cos(t);\n"
    "y = 0.5 + 0.3 * sin(t);\n" },
  { IDC_MW_PEDIT_TPL_BASE + 31, L"Colour by volume",
    "r = bass_att * 0.5;\n"
    "g = mid_att * 0.5;\n"
    "b = treb_att * 0.5;\n"
    "a = 1;\n" },
};

const Template kTplShape[] = {
  { IDC_MW_PEDIT_TPL_BASE + 40, L"Pulse with the beat",
    "rad = 0.2 + 0.1 * bass_att;\n"
    "ang = ang + 0.01;\n"
    "a = 0.6;\n" },
};

const Template kTplWarp[] = {
  { IDC_MW_PEDIT_TPL_BASE + 50, L"Empty warp shader",
    "shader_body\n"
    "{\n"
    "    ret = GetPixel(uv).xyz;\n"
    "}\n" },
  { IDC_MW_PEDIT_TPL_BASE + 51, L"Blur feedback",
    "shader_body\n"
    "{\n"
    "    float2 d = (uv - 0.5) * 0.995 + 0.5;\n"
    "    ret = lerp(GetPixel(d).xyz, GetBlur1(uv).xyz, 0.25);\n"
    "}\n" },
};

const Template kTplComp[] = {
  { IDC_MW_PEDIT_TPL_BASE + 60, L"Passthrough",
    "shader_body\n"
    "{\n"
    "    ret = GetPixel(uv).xyz;\n"
    "}\n" },
  { IDC_MW_PEDIT_TPL_BASE + 61, L"Hue shift by treble",
    "shader_body\n"
    "{\n"
    "    float3 c = GetPixel(uv).xyz;\n"
    "    ret = c * hue_shader * (1.0 + 0.5 * treb_att);\n"
    "}\n" },
};

// Which set applies to a section.
void TemplatesFor(int section, const Template** out, int* count) {
  switch (section) {
    case PCS_PER_FRAME:       *out = kTplPerFrame; *count = _countof(kTplPerFrame); return;
    case PCS_PER_PIXEL:       *out = kTplPerPixel; *count = _countof(kTplPerPixel); return;
    case PCS_PRESET_INIT:
    case PCS_WAVE_INIT:
    case PCS_SHAPE_INIT:      *out = kTplInit;     *count = _countof(kTplInit);     return;
    case PCS_WAVE_PER_FRAME:
    case PCS_WAVE_PER_POINT:  *out = kTplWave;     *count = _countof(kTplWave);     return;
    case PCS_SHAPE_PER_FRAME: *out = kTplShape;    *count = _countof(kTplShape);    return;
    case PCS_WARP_SHADER:     *out = kTplWarp;     *count = _countof(kTplWarp);     return;
    case PCS_COMP_SHADER:     *out = kTplComp;     *count = _countof(kTplComp);     return;
    default:                  *out = nullptr;      *count = 0;                      return;
  }
}

} // namespace

// Insert at the caret, replacing any selection.  Never wipes the section: a
// template is a starting point you paste into what you already have.
void PresetEditorWindow::InsertTemplate(int nTemplateId) {
  if (!m_sci.IsValid()) { SetStatus(L"Templates need the syntax editor."); return; }
  if (m_nCurRow < 0 || m_nCurRow >= (int)m_sections.size()) return;

  const Template* list = nullptr;
  int count = 0;
  TemplatesFor(m_sections[m_nCurRow].section, &list, &count);
  for (int i = 0; i < count; i++) {
    if (list[i].id != nTemplateId) continue;
    m_sci.ReplaceSelection(list[i].body);
    m_staged[m_nCurRow] = m_sci.GetTextUtf8();
    m_dirty[m_nCurRow] = true;
    m_sci.ClearModified();
    SetStatus(L"Template inserted. Apply to run it.");
    return;
  }
}

void PresetEditorWindow::SavePreset(bool bSaveAs) {
  if (!m_pEngine) return;

  // A .milk2 is saved through Engine::SaveMilk2ToPath, which writes BOTH
  // presets plus the blend header. That only works while the frozen blend is
  // live; without it CState holds one preset and a save would destroy the other.
  if (IsMilk2Path(m_pEngine->m_szCurrentPresetFile) &&
      !m_pEngine->HasEditableBlendFromPreset()) {
    SetStatus(L"This .milk2's second preset is not loaded, so saving would "
              L"discard it - use Save As with a .milk name instead.");
    return;
  }

  // Apply first so the file matches what is on screen.  ProcessPendingCommands
  // preserves order and only coalesces LoadPresetPath, so the save lands after
  // every apply in the same drained batch.
  StashEditorText();
  if (CurrentIsWholeFile()) {
    EnqueueApply(m_nCurRow);
  } else {
    for (int i = 0; i < (int)m_sections.size(); i++) {
      const int s = m_sections[i].section;
      if (s == PSEUDO_WHOLE_PRESET || s == PSEUDO_RAW_FILE) continue;
      if (m_dirty[i]) EnqueueApply(i);
    }
  }

  std::wstring path;
  if (bSaveAs) {
    wchar_t szFile[MAX_PATH] = {0};
    if (m_pEngine->m_szCurrentPresetFile[0])
      wcscpy_s(szFile, m_pEngine->m_szCurrentPresetFile);

    OPENFILENAMEW ofn = {0};
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner   = m_hWnd;
    // The extension picks the format: .milk2 writes both presets plus the blend
    // header, anything else writes the live preset on its own.
    const bool bTwo = m_pEngine->HasEditableBlendFromPreset();
    ofn.lpstrFilter = bTwo
      ? L"MilkDrop double presets (*.milk2)\0*.milk2\0MilkDrop presets (*.milk)\0*.milk\0All files (*.*)\0*.*\0"
      : L"MilkDrop presets (*.milk)\0*.milk\0All files (*.*)\0*.*\0";
    ofn.lpstrFile   = szFile;
    ofn.nMaxFile    = MAX_PATH;
    ofn.lpstrDefExt = bTwo ? L"milk2" : L"milk";
    ofn.Flags       = OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
    if (!GetSaveFileNameW(&ofn)) return;      // user cancelled
    path = szFile;
  } else {
    if (!m_pEngine->m_szCurrentPresetFile[0]) {
      SetStatus(L"No current preset file -- use Save As.");
      return;
    }
    path = m_pEngine->m_szCurrentPresetFile;
  }

  RenderCommand cmd;
  cmd.cmd    = RenderCmd::SavePresetFile;
  cmd.sParam = path;
  m_pEngine->EnqueueRenderCmd(std::move(cmd));

  m_bPollingSave = true;
  SetStatus(L"Saving...");
  SetTimer(m_hWnd, kApplyPollTimer, 250, NULL);
}

//----------------------------------------------------------------------
// Status / error reporting
//----------------------------------------------------------------------

void PresetEditorWindow::SetStatus(const wchar_t* text) {
  if (m_hStatus) SetWindowTextW(m_hStatus, text ? text : L"");
}

void PresetEditorWindow::RefreshStatusFromLastError() {
  if (!m_pEngine) return;
  const std::wstring err = m_pEngine->m_wLastShaderError;

  if (err.empty()) {
    SetStatus(m_bPollingSave ? L"Saved." : L"Applied.");
    m_bPollingSave = false;
    if (m_sci.IsValid()) m_sci.ClearErrorMarks();
    return;
  }
  m_bPollingSave = false;

  SetStatus(err.c_str());

  // Only shader sections have compiler line numbers worth mapping.
  if (m_nCurRow < 0 || m_nCurRow >= (int)m_sections.size()) return;
  const int sec = m_sections[m_nCurRow].section;
  if (sec != PCS_WARP_SHADER && sec != PCS_COMP_SHADER) return;
  if (!m_sci.IsValid()) return;

  // D3DCompile prefixes a source name, so find the first '(' rather than
  // assuming the line number sits at offset 0.
  const wchar_t* paren = wcschr(err.c_str(), L'(');
  if (!paren) return;
  int compiledLine = 0;
  if (swscanf_s(paren, L"(%d,", &compiledLine) != 1) return;

  const int userLine = m_pEngine->MapCompiledLineToUserLine(compiledLine);
  if (userLine > 0) {
    m_sci.ClearErrorMarks();
    m_sci.MarkErrorLine(userLine);
    m_sci.GotoLine(userLine);
  }
}

//----------------------------------------------------------------------
// Message handling
//----------------------------------------------------------------------

LRESULT PresetEditorWindow::DoCommand(HWND hWnd, int id, int code, LPARAM lParam) {
  UNREFERENCED_PARAMETER(hWnd);
  UNREFERENCED_PARAMETER(lParam);

  // Templates are a contiguous id block, so they cannot be a switch label.
  if (id >= IDC_MW_PEDIT_TPL_BASE && id < IDC_MW_PEDIT_TPL_BASE + 100) {
    InsertTemplate(id);
    return 0;
  }

  switch (id) {
    case IDC_MW_PEDIT_APPLY:
      if (code == BN_CLICKED) { ApplyCurrentSection(); return 0; }
      return -1;
    case IDC_MW_PEDIT_EXPAND:
      if (code == BN_CLICKED) { ExpandCurrentSection(); return 0; }
      return -1;
    case IDC_MW_PEDIT_REVERT:
      if (code == BN_CLICKED) { RevertCurrentSection(); return 0; }
      return -1;
    case IDC_MW_PEDIT_SAVE:
      if (code == BN_CLICKED) { SavePreset(false); return 0; }
      return -1;
    case IDC_MW_PEDIT_SAVEAS:
      if (code == BN_CLICKED) { SavePreset(true); return 0; }
      return -1;

    // ── Context menu ──
    case IDC_MW_PEDIT_OPT_LINENUMS:
      m_bOptLineNumbers = !m_bOptLineNumbers;
      m_sci.SetLineNumbersVisible(m_bOptLineNumbers);
      SaveViewOptions();
      return 0;
    case IDC_MW_PEDIT_OPT_FOLDING:
      m_bOptFolding = !m_bOptFolding;
      m_sci.SetFoldingEnabled(m_bOptFolding);
      SaveViewOptions();
      return 0;
    case IDC_MW_PEDIT_FOLDALL:    m_sci.FoldAll(true);  return 0;
    case IDC_MW_PEDIT_UNFOLDALL:  m_sci.FoldAll(false); return 0;
    case IDC_MW_PEDIT_UNDO:       m_sci.Undo();         return 0;
    case IDC_MW_PEDIT_REDO:       m_sci.Redo();         return 0;
    case IDC_MW_PEDIT_CUT:        m_sci.Cut();          return 0;
    case IDC_MW_PEDIT_COPY:       m_sci.Copy();         return 0;
    case IDC_MW_PEDIT_PASTE:      m_sci.Paste();        return 0;
    case IDC_MW_PEDIT_SELECTALL:  m_sci.SelectAll();    return 0;

    default:
      return -1;
  }
}

LRESULT PresetEditorWindow::DoNotify(HWND hWnd, NMHDR* pnm) {
  UNREFERENCED_PARAMETER(hWnd);
  if (!pnm) return -1;

  if (pnm->hwndFrom == m_hSideTabs && pnm->code == TCN_SELCHANGE) {
    // Tab 0 is Preset 1 (the blend-from state), tab 1 is Preset 2 (live).
    const int sel = TabCtrl_GetCurSel(m_hSideTabs);
    OnSideChanged(sel == 0 ? PSIDE_BLENDFROM : PSIDE_LIVE);
    return 0;
  }

  if (pnm->hwndFrom == m_hTree && pnm->code == TVN_SELCHANGEDW) {
    const NMTREEVIEWW* tv = (const NMTREEVIEWW*)pnm;
    const int row = (int)tv->itemNew.lParam;
    if (row >= 0) LoadSectionIntoEditor(row);   // group nodes carry -1
    return 0;
  }
  return m_sci.OnNotify(pnm) ? 0 : -1;
}

//----------------------------------------------------------------------
// View options (line numbers / folding), persisted per window
//----------------------------------------------------------------------

void PresetEditorWindow::LoadViewOptions() {
  if (!m_pEngine) return;
  const wchar_t* sec = GetINISection();
  m_bOptLineNumbers = Config().GetInt(sec, L"LineNumbers", 1) != 0;
  m_bOptFolding     = Config().GetInt(sec, L"Folding", 1) != 0;
}

void PresetEditorWindow::SaveViewOptions() {
  if (!m_pEngine) return;
  const wchar_t* sec = GetINISection();
  Config().SetString(sec, L"LineNumbers", m_bOptLineNumbers ? L"1" : L"0");
  Config().SetString(sec, L"Folding", m_bOptFolding     ? L"1" : L"0");
}

//----------------------------------------------------------------------
// Context menu
//----------------------------------------------------------------------

LRESULT PresetEditorWindow::DoContextMenu(HWND hWnd, int x, int y) {
  if (!m_sci.IsValid()) return -1;

  // The menu opens anywhere in the window, not only over the code pane.
  // Restricting it to the editor is how Save As became unreachable when it
  // lived here: a menu you have to already know about is not a menu.
  POINT pt = { x, y };
  if (pt.x == -1 && pt.y == -1) {          // keyboard (Shift+F10 / menu key)
    // Put it at the caret when the editor has focus, otherwise beside whatever
    // control does -- a menu that appears somewhere unrelated is disorienting.
    HWND hFocus = GetFocus();
    RECT rc;
    if (hFocus && IsChild(m_hWnd, hFocus) && hFocus != m_sci.Hwnd()) {
      GetWindowRect(hFocus, &rc);
      pt.x = rc.left + 24;
      pt.y = rc.top + 24;
    } else {
      GetWindowRect(m_sci.Hwnd(), &rc);
      pt.x = rc.left + 24;
      pt.y = rc.top + 24;
    }
  }

  HMENU hOptions = CreatePopupMenu();
  AppendMenuW(hOptions, MF_STRING | (m_bOptLineNumbers ? MF_CHECKED : 0),
              IDC_MW_PEDIT_OPT_LINENUMS, L"Line Numbers");
  AppendMenuW(hOptions, MF_STRING | (m_bOptFolding ? MF_CHECKED : 0),
              IDC_MW_PEDIT_OPT_FOLDING, L"Code Folding");
  AppendMenuW(hOptions, MF_SEPARATOR, 0, NULL);
  AppendMenuW(hOptions, MF_STRING | (m_bOptFolding ? 0 : MF_GRAYED),
              IDC_MW_PEDIT_FOLDALL, L"Fold All");
  AppendMenuW(hOptions, MF_STRING | (m_bOptFolding ? 0 : MF_GRAYED),
              IDC_MW_PEDIT_UNFOLDALL, L"Unfold All");

  HMENU hMenu = CreatePopupMenu();
  AppendMenuW(hMenu, MF_STRING | (m_sci.CanUndo() ? 0 : MF_GRAYED), IDC_MW_PEDIT_UNDO, L"Undo");
  AppendMenuW(hMenu, MF_STRING | (m_sci.CanRedo() ? 0 : MF_GRAYED), IDC_MW_PEDIT_REDO, L"Redo");
  AppendMenuW(hMenu, MF_SEPARATOR, 0, NULL);
  const bool bSel = m_sci.HasSelection();
  AppendMenuW(hMenu, MF_STRING | (bSel ? 0 : MF_GRAYED), IDC_MW_PEDIT_CUT,  L"Cut");
  AppendMenuW(hMenu, MF_STRING | (bSel ? 0 : MF_GRAYED), IDC_MW_PEDIT_COPY, L"Copy");
  AppendMenuW(hMenu, MF_STRING, IDC_MW_PEDIT_PASTE, L"Paste");
  AppendMenuW(hMenu, MF_STRING, IDC_MW_PEDIT_SELECTALL, L"Select All");
  AppendMenuW(hMenu, MF_SEPARATOR, 0, NULL);
  AppendMenuW(hMenu, MF_STRING, IDC_MW_PEDIT_EXPAND, L"Expand Statements");

  // Templates for whatever section is open. Whole-file views get none: a
  // snippet has no single right place to land in a whole preset.
  {
    const Template* list = nullptr;
    int count = 0;
    if (m_nCurRow >= 0 && m_nCurRow < (int)m_sections.size())
      TemplatesFor(m_sections[m_nCurRow].section, &list, &count);
    HMENU hTpl = CreatePopupMenu();
    for (int i = 0; i < count; i++)
      AppendMenuW(hTpl, MF_STRING, list[i].id, list[i].label);
    if (count == 0)
      AppendMenuW(hTpl, MF_STRING | MF_GRAYED, 0, L"(none for this section)");
    AppendMenuW(hMenu, MF_POPUP, (UINT_PTR)hTpl, L"Templates");
  }

  AppendMenuW(hMenu, MF_SEPARATOR, 0, NULL);
  AppendMenuW(hMenu, MF_STRING, IDC_MW_PEDIT_SAVEAS, L"Save As...");
  AppendMenuW(hMenu, MF_SEPARATOR, 0, NULL);
  AppendMenuW(hMenu, MF_POPUP, (UINT_PTR)hOptions, L"Options");

  TrackPopupMenu(hMenu, TPM_LEFTALIGN | TPM_TOPALIGN, pt.x, pt.y, 0, hWnd, NULL);
  DestroyMenu(hMenu);   // also destroys the submenu
  return 0;
}

LRESULT PresetEditorWindow::DoMessage(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam) {
  UNREFERENCED_PARAMETER(lParam);
  if (msg == WM_TIMER && wParam == kApplyPollTimer) {
    KillTimer(hWnd, kApplyPollTimer);
    RefreshStatusFromLastError();
    return 0;
  }

  if (msg == WM_TIMER && wParam == kPresetWatchTimer) {
    if (m_pEngine && m_wWatchedPreset != m_pEngine->m_szCurrentPresetFile) {
      m_wWatchedPreset = m_pEngine->m_szCurrentPresetFile;
      // Everything staged belongs to the preset that just went away.
      m_staged.assign(m_sections.size(), std::string());
      m_dirty.assign(m_sections.size(), false);
      const bool bWasTwo = m_bTwoPresets;
      SyncSideTabs();
      if (bWasTwo != m_bTwoPresets) {
        LayoutControls();          // the tab strip appeared or went away
        if (m_hSideTabs)
          TabCtrl_SetCurSel(m_hSideTabs, m_nSide == PSIDE_BLENDFROM ? 0 : 1);
      }
      const int keep = m_nCurRow;
      m_nCurRow = -1;              // do not stash into the preset we just left
      LoadSectionIntoEditor(keep < 0 ? 0 : keep);
    }
    return 0;
  }
  return -1;
}

} // namespace mdrop
