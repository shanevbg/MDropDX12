#include "window_diag.h"

#include "format_to.h"
#include "pipe_server.h"

#include <wincodec.h>
#include <algorithm>
#include <vector>

#pragma comment(lib, "windowscodecs.lib")

// Global scope, not inside mdrop -- that is where it is defined, and declaring
// it in the namespace links against a different symbol that does not exist.
extern PipeServer g_pipeServer;

namespace mdrop {

namespace {

std::wstring Lower(std::wstring s) {
  std::transform(s.begin(), s.end(), s.begin(),
                 [](wchar_t c) { return (wchar_t)towlower(c); });
  return s;
}

struct FindCtx {
  std::wstring want;      // already lowercased
  HWND exact = NULL;
  HWND prefix = NULL;
};

BOOL CALLBACK FindProc(HWND hWnd, LPARAM lp) {
  FindCtx* ctx = (FindCtx*)lp;
  DWORD pid = 0;
  GetWindowThreadProcessId(hWnd, &pid);
  if (pid != GetCurrentProcessId() || !IsWindowVisible(hWnd)) return TRUE;

  wchar_t title[256] = {};
  GetWindowTextW(hWnd, title, 256);
  if (!title[0]) return TRUE;
  const std::wstring t = Lower(title);
  if (t == ctx->want) { ctx->exact = hWnd; return FALSE; }
  if (!ctx->prefix && t.rfind(ctx->want, 0) == 0) ctx->prefix = hWnd;
  return TRUE;
}

// Escapes the field separator so a control's text cannot forge a new field.
// A preset name really can contain '|', and a caller splitting on it would
// otherwise read one control as several.
//
// The pipe is escaped as \| and NOT as \p, which is what this used first and
// which is a trap: almost every string in this program is a Windows path, and
// "\presets" contains \p. A decoder that unescapes backslashes and then pipes
// -- the obvious two-pass way, and the way the first consumer of this really
// was written -- turned
//
//     C:\resources\presets\   into   C:\resources|resets\
//
// which looks exactly like the app truncating a buffer, in a dump whose whole
// purpose is finding truncated strings. \| cannot occur in a path, so even a
// naive decoder gets it right.
std::wstring Escape(const std::wstring& s) {
  std::wstring out;
  out.reserve(s.size());
  for (wchar_t c : s) {
    if (c == L'|') out += L"\\|";
    else if (c == L'\\') out += L"\\\\";
    else if (c == L'\r') out += L"\\r";
    else if (c == L'\n') out += L"\\n";
    else out += c;
  }
  return out;
}

// Windows strips a single '&' as a mnemonic prefix before drawing, so measuring
// the raw string measures a character that is never painted. '&&' is one
// literal ampersand and does count.
std::wstring StripMnemonic(const std::wstring& s) {
  std::wstring out;
  out.reserve(s.size());
  for (size_t i = 0; i < s.size(); i++) {
    if (s[i] == L'&') {
      if (i + 1 < s.size() && s[i + 1] == L'&') { out += L'&'; i++; }
      continue;
    }
    out += s[i];
  }
  return out;
}

// How wide the control's own text is, and whether the control is wide enough
// for it (issue 159).
//
// The one thing a rectangle dump cannot infer. GetWindowTextW hands back the
// whole string whatever the control manages to paint, so a label cut in half
// reads here as perfectly correct -- which is how "Preset Name Profile:" drawn
// as "Preset Name" survived a pass whose entire purpose was finding truncated
// strings. It took a screenshot to see, and a screenshot is what this dump
// exists to make unnecessary.
//
// Measured against what THIS app's drawing code actually does, which is the
// reason it has to run in-process. Every button, check box and radio here is
// BS_OWNERDRAW -- the dark theme paints them itself -- so the style bits say
// "owner draw" and nothing more; the type is a window PROPERTY (IsCheckbox,
// IsRadio, IsPinBtn), and the text rectangle each one draws into is reproduced
// below from DrawOwnerCheckbox, DrawOwnerRadio and DrawOwnerButton.
struct TextMetric {
  // The width the CONTROL would need to hold this text on one line -- the text
  // itself plus whatever the control's own chrome takes before the text starts.
  //
  // Reported that way, and not as the bare text width, so the verdict can be
  // rechecked from the dump alone: textfits is 0 exactly when tw exceeds w.
  // A check box's glyph is 5 + max(h/2, 11) wide and lives inside the same
  // rectangle, so a bare text width made "Out" in a 55px radio look like it had
  // 19 pixels to spare while it was in fact being drawn as "Ou".
  int width = 0;
  int fits  = -1;  // 1 yes, 0 no, -1 not judged
};

TextMetric MeasureText(HWND hCtrl, const wchar_t* cls, const wchar_t* text) {
  TextMetric tm;
  if (!text || !text[0]) return tm;

  const LONG style = (LONG)GetWindowLongPtrW(hCtrl, GWL_STYLE);
  const bool isStatic = _wcsicmp(cls, L"Static") == 0;
  const bool isButton = _wcsicmp(cls, L"Button") == 0;

  std::wstring s = text;
  if (isButton || (isStatic && !(style & SS_NOPREFIX))) s = StripMnemonic(s);

  RECT rc = {};
  GetClientRect(hCtrl, &rc);
  const int ctrlW = rc.right, ctrlH = rc.bottom;

  HDC hdc = GetDC(hCtrl);
  if (!hdc) return tm;
  HFONT hf = (HFONT)SendMessageW(hCtrl, WM_GETFONT, 0, 0);
  HFONT hOld = hf ? (HFONT)SelectObject(hdc, hf) : NULL;

  SIZE sz = {};
  GetTextExtentPoint32W(hdc, s.c_str(), (int)s.size(), &sz);
  tm.width = sz.cx;

  TEXTMETRICW met = {};
  GetTextMetricsW(hdc, &met);

  // Judged only where the string is OURS and the box is ours to size. An edit
  // or a combo holds the user's data and scrolls it, so a value too long for
  // the field is not the same kind of fact and would drown this in noise; the
  // width above is reported for those anyway, so a reader can still judge.
  int reserve = -1;
  bool wraps = false;
  if (isStatic) {
    const LONG type = style & SS_TYPEMASK;
    // A static that declares an ellipsis style has SAID it will not fit
    // everything and has a graceful answer for that -- a device name, a
    // path. Judging it would report a design decision as a defect, and
    // those cases are exactly the ones no wording change can fix.
    const bool elides = (style & (SS_ENDELLIPSIS | SS_PATHELLIPSIS |
                                 SS_WORDELLIPSIS)) != 0;
    if (elides) {
      reserve = -1;
    } else if (type == SS_LEFT || type == SS_CENTER || type == SS_RIGHT) {
      reserve = 0;
      // An SS_LEFT static word-wraps, so one tall enough for a second line is
      // not truncated by a string wider than itself -- it is doing what it was
      // made that tall for. Only DrawText knows where the breaks fall.
      wraps = ctrlH >= met.tmHeight * 2;
    } else if (type == SS_SIMPLE || type == SS_LEFTNOWORDWRAP) {
      reserve = 0;
    }
  } else if (isButton && !GetPropW(hCtrl, L"IsPinBtn")) {
    // DrawOwnerCheckbox and DrawOwnerRadio both put the text at
    // rc.left + 1 + max(h/2, 11) + 4; DrawOwnerButton draws into the whole rect.
    reserve = (GetPropW(hCtrl, L"IsCheckbox") || GetPropW(hCtrl, L"IsRadio"))
                  ? 5 + max(ctrlH / 2, 11)
                  : 0;
  }

  // Two pixels of overhang is not a truncation -- the same slack, for the same
  // reason, that FitToContents allows on a control hanging past the client
  // edge. GetTextExtentPoint32W returns the sum of the glyph ADVANCES, which is
  // not exactly the ink DrawText lays down, and a centred button string one
  // pixel too wide loses half a pixel at each end. Reporting those as cut off
  // would bury the ones that really are -- and `tw` is exact either way, so
  // nothing is hidden by the tolerance.
  const int kSlack = 2;

  if (reserve >= 0) {
    tm.width += reserve;
    if (wraps) {
      RECT calc = { 0, 0, ctrlW, 0 };
      DrawTextW(hdc, s.c_str(), (int)s.size(), &calc,
                DT_CALCRECT | DT_WORDBREAK | DT_LEFT);
      // A wrapping static is judged on HEIGHT: tw stays the single-line width,
      // which is what it would need in order NOT to wrap.
      tm.fits = (calc.bottom <= ctrlH + kSlack) ? 1 : 0;
    } else {
      tm.fits = (tm.width <= ctrlW + kSlack) ? 1 : 0;
    }
  }

  if (hOld) SelectObject(hdc, hOld);
  ReleaseDC(hCtrl, hdc);
  return tm;
}

struct DumpCtx {
  HWND parent;
  int  index = 0;
};

BOOL CALLBACK DumpProc(HWND hCtrl, LPARAM lp) {
  DumpCtx* ctx = (DumpCtx*)lp;

  wchar_t cls[64] = {};
  GetClassNameW(hCtrl, cls, 64);

  // 512 rather than a name-sized buffer: an edit control on these windows can
  // hold a whole shader, and a truncated dump would read as a truncated
  // CONTROL, which is the bug this exists to find.
  wchar_t text[512] = {};
  GetWindowTextW(hCtrl, text, 512);

  RECT r = {};
  GetWindowRect(hCtrl, &r);
  POINT tl = { r.left, r.top };
  ScreenToClient(ctx->parent, &tl);

  RECT rcParent = {};
  GetClientRect(ctx->parent, &rcParent);
  const int w = r.right - r.left, h = r.bottom - r.top;
  const bool clipped = (tl.x + w > rcParent.right) || (tl.y + h > rcParent.bottom) ||
                       tl.x < 0 || tl.y < 0;

  // The codepoints, not just the text. A glyph button holding a mangled escape
  // still prints as SOMETHING; only the codepoints show that U+E8E5 arrived as
  // U+00C3 U+00A8 U+0045 U+0035.
  std::wstring codes;
  for (int i = 0; text[i] && i < 8; i++) {
    wchar_t buf[8];
    FormatTo(buf, L"%s%04X", i ? L" " : L"", (unsigned)text[i]);
    codes += buf;
  }

  const TextMetric tm = MeasureText(hCtrl, cls, text);

  wchar_t head[384];
  FormatTo(head,
           L"WINDOW_CTRL|i=%d|class=%s|id=%d|x=%d|y=%d|w=%d|h=%d"
           L"|vis=%d|enab=%d|clip=%d|tw=%d|textfits=%d|u=",
           ctx->index++, cls,
           (int)GetWindowLongPtrW(hCtrl, GWLP_ID),   // GetMenu is for menus
           tl.x, tl.y, w, h,
           IsWindowVisible(hCtrl) ? 1 : 0, IsWindowEnabled(hCtrl) ? 1 : 0,
           clipped ? 1 : 0, tm.width, tm.fits);
  std::wstring line = head;
  line += codes;
  line += L"|text=";
  line += Escape(text);
  g_pipeServer.Send(line.c_str());
  return TRUE;
}

}  // namespace

HWND FindToolWindowByTitle(const std::wstring& title) {
  FindCtx ctx;
  ctx.want = Lower(title);
  EnumWindows(FindProc, (LPARAM)&ctx);
  return ctx.exact ? ctx.exact : ctx.prefix;
}

void SendWindowControlDump(const std::wstring& title) {
  HWND hWnd = FindToolWindowByTitle(title);
  if (!hWnd) {
    g_pipeServer.Send(L"WINDOW_CTRLS=NOTFOUND");
    return;
  }
  wchar_t got[256] = {};
  GetWindowTextW(hWnd, got, 256);
  RECT rc = {};
  GetClientRect(hWnd, &rc);

  wchar_t head[512];
  FormatTo(head, L"WINDOW_CTRLS|title=%s|client=%dx%d", got, rc.right, rc.bottom);
  g_pipeServer.Send(head);

  DumpCtx ctx{ hWnd, 0 };
  EnumChildWindows(hWnd, DumpProc, (LPARAM)&ctx);
  g_pipeServer.Send(L"WINDOW_CTRLS_END");
}

void SendWindowCapture(const std::wstring& title, const std::wstring& baseDir) {
  HWND hWnd = FindToolWindowByTitle(title);
  if (!hWnd) {
    g_pipeServer.Send(L"WINDOW_CAPTURE=NOTFOUND");
    return;
  }

  RECT rc = {};
  GetClientRect(hWnd, &rc);
  const int w = rc.right, h = rc.bottom;
  if (w <= 0 || h <= 0) {
    g_pipeServer.Send(L"WINDOW_CAPTURE=ERROR|reason=empty-client");
    return;
  }

  HDC hdcWin = GetDC(hWnd);
  HDC hdcMem = CreateCompatibleDC(hdcWin);
  BITMAPINFO bi = {};
  bi.bmiHeader.biSize = sizeof(bi.bmiHeader);
  bi.bmiHeader.biWidth = w;
  bi.bmiHeader.biHeight = -h;              // top-down, so WIC needs no flip
  bi.bmiHeader.biPlanes = 1;
  bi.bmiHeader.biBitCount = 32;
  bi.bmiHeader.biCompression = BI_RGB;
  void* bits = nullptr;
  HBITMAP hbm = CreateDIBSection(hdcWin, &bi, DIB_RGB_COLORS, &bits, NULL, 0);
  HGDIOBJ old = hbm ? SelectObject(hdcMem, hbm) : NULL;

  bool ok = false;
  if (hbm) {
    // PW_RENDERFULLCONTENT is the whole reason this runs in-process. Without
    // it, owner-drawn buttons -- the pin, the refresh, every glyph button --
    // are simply not painted, and the capture comes back clean and wrong.
    ok = PrintWindow(hWnd, hdcMem, PW_CLIENTONLY | PW_RENDERFULLCONTENT) != 0;
    if (!ok) ok = PrintWindow(hWnd, hdcMem, PW_CLIENTONLY) != 0;
  }

  std::wstring path;
  if (ok) {
    wchar_t dir[MAX_PATH];
    FormatTo(dir, L"%scapture\\", baseDir.c_str());
    CreateDirectoryW(dir, NULL);

    wchar_t safe[64] = {};
    {
      int j = 0;
      for (size_t i = 0; i < title.size() && j < 63; i++) {
        const wchar_t c = title[i];
        safe[j++] = (iswalnum(c) ? c : L'_');
      }
      safe[j] = 0;
    }
    SYSTEMTIME st;
    GetLocalTime(&st);
    wchar_t full[MAX_PATH];
    FormatTo(full, L"%s%04d%02d%02d-%02d%02d%02d-window-%s.png", dir,
             st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond, safe);
    path = full;

    IWICImagingFactory* factory = nullptr;
    ok = SUCCEEDED(CoCreateInstance(CLSID_WICImagingFactory, nullptr,
                                    CLSCTX_INPROC_SERVER,
                                    IID_PPV_ARGS(&factory))) && factory;
    if (ok) {
      IWICStream* stream = nullptr;
      IWICBitmapEncoder* encoder = nullptr;
      IWICBitmapFrameEncode* frame = nullptr;
      ok = SUCCEEDED(factory->CreateStream(&stream)) &&
           SUCCEEDED(stream->InitializeFromFilename(path.c_str(), GENERIC_WRITE)) &&
           SUCCEEDED(factory->CreateEncoder(GUID_ContainerFormatPng, nullptr, &encoder)) &&
           SUCCEEDED(encoder->Initialize(stream, WICBitmapEncoderNoCache)) &&
           SUCCEEDED(encoder->CreateNewFrame(&frame, nullptr)) &&
           SUCCEEDED(frame->Initialize(nullptr)) &&
           SUCCEEDED(frame->SetSize(w, h));
      if (ok) {
        WICPixelFormatGUID fmt = GUID_WICPixelFormat32bppBGRA;
        ok = SUCCEEDED(frame->SetPixelFormat(&fmt)) &&
             SUCCEEDED(frame->WritePixels(h, w * 4, w * 4 * h, (BYTE*)bits)) &&
             SUCCEEDED(frame->Commit()) &&
             SUCCEEDED(encoder->Commit());
      }
      if (frame) frame->Release();
      if (encoder) encoder->Release();
      if (stream) stream->Release();
      factory->Release();
    }
  }

  if (old) SelectObject(hdcMem, old);
  if (hbm) DeleteObject(hbm);
  DeleteDC(hdcMem);
  ReleaseDC(hWnd, hdcWin);

  if (ok) {
    std::wstring reply = L"WINDOW_CAPTURE_PATH=" + path;
    g_pipeServer.Send(reply.c_str());
  } else {
    g_pipeServer.Send(L"WINDOW_CAPTURE=ERROR|reason=capture-failed");
  }
}

// One window's geometry, as a field run. Screen coordinates, because that is
// the frame a layout has to reason in -- a client placing its own window
// beside ours cannot use our client coordinates.
//
// client= is reported separately from w/h and is not redundant: the render
// cost, the swap-chain size and every capture are all client-sized, while a
// layout has to reserve the FRAME. On a borderless window they are equal; on a
// tool window they differ by the caption and border, which is exactly the
// margin a layout gets wrong when it only has one of the two.
static std::wstring GeometryFields(HWND hWnd) {
  RECT wr = {}, cr = {};
  GetWindowRect(hWnd, &wr);
  GetClientRect(hWnd, &cr);

  // Which monitor it is ON, by the same device name the display commands use,
  // so a caller can join this against GET_DISPLAY_MODE without guessing from
  // coordinates.
  wchar_t device[36] = L"";
  MONITORINFOEXW mi = {};
  mi.cbSize = sizeof(mi);
  HMONITOR hm = MonitorFromWindow(hWnd, MONITOR_DEFAULTTONEAREST);
  if (hm && GetMonitorInfoW(hm, &mi))
    wcsncpy_s(device, mi.szDevice, _TRUNCATE);

  const LONG_PTR ex = GetWindowLongPtrW(hWnd, GWL_EXSTYLE);
  wchar_t buf[512];
  FormatTo(buf,
           // monitor=, not display=: display= is the 1-based INDEX in
           // RoleFields, and two different keys spelled the same way in one
           // row means a caller building a dictionary silently keeps whichever
           // it parsed last. Device names go out RAW here, unescaped, because
           // that is how GET_DISPLAY_MODE and every other display verb emits
           // them and clients already parse that form.
           L"x=%d|y=%d|w=%d|h=%d|client=%dx%d|monitor=%ls"
           L"|visible=%d|minimized=%d|maximized=%d|topmost=%d|dpi=%u",
           (int)wr.left, (int)wr.top,
           (int)(wr.right - wr.left), (int)(wr.bottom - wr.top),
           (int)cr.right, (int)cr.bottom,
           device[0] ? device : L"?",
           IsWindowVisible(hWnd) ? 1 : 0,
           IsIconic(hWnd) ? 1 : 0,
           IsZoomed(hWnd) ? 1 : 0,
           (ex & WS_EX_TOPMOST) ? 1 : 0,
           GetDpiForWindow(hWnd));
  return buf;
}

// role=, plus the display it belongs to when it belongs to one.
//
// Kept separate from GeometryFields because these are the facts the engine
// supplied and those are the ones Win32 measured -- and when a row is wrong it
// matters a great deal which half produced it.
static std::wstring RoleFields(const KnownWindow* k) {
  if (!k) return L"role=tool";
  std::wstring s = L"role=" + k->role;
  if (k->display > 0) s += L"|display=" + std::to_wstring(k->display);
  // Raw, not Escape()d, to match GET_DISPLAY_MODE. device= is the monitor the
  // engine ASSIGNED this window to; monitor= in GeometryFields is where Windows
  // says it actually is. They are usually the same, and the case where they are
  // not is worth being able to see.
  if (!k->device.empty()) s += L"|device=" + k->device;
  if (k->pid) s += L"|pid=" + std::to_wstring(k->pid);
  return s;
}

namespace {

struct ListCtx {
  std::wstring out;
  int count = 0;
  const std::vector<KnownWindow>* known = nullptr;
};

const KnownWindow* Lookup(const std::vector<KnownWindow>& known, HWND h) {
  for (const KnownWindow& k : known)
    if (k.hWnd == h) return &k;
  return nullptr;
}

BOOL CALLBACK ListProc(HWND hWnd, LPARAM lp) {
  ListCtx* ctx = (ListCtx*)lp;
  DWORD pid = 0;
  GetWindowThreadProcessId(hWnd, &pid);
  if (pid != GetCurrentProcessId()) return TRUE;

  // Titled top-level windows only. Untitled ones are tooltips, IME helpers and
  // message-only plumbing -- nothing a layout can address or would want to.
  wchar_t title[256] = {};
  GetWindowTextW(hWnd, title, 256);
  if (!title[0]) return TRUE;

  // Zero-area windows are plumbing, not layout. Measured: "MSCTFIME UI" and
  // two "Default IME" windows come back from EnumWindows with a title, so
  // filtering on the title alone leaves three rows of noise that no caller can
  // place anything relative to. Every real window of ours has an area, even
  // when hidden.
  RECT wr = {};
  GetWindowRect(hWnd, &wr);
  if (wr.right <= wr.left || wr.bottom <= wr.top) return TRUE;

  wchar_t cls[128] = {};
  GetClassNameW(hWnd, cls, 128);

  // Hidden windows ARE listed, with visible=0 -- a child in hidden mode is a
  // real window with a real rect. Note that a CLOSED tool window is not here
  // at all: ToolWindow::Close destroys the window rather than hiding it, so
  // its remembered position lives in the ini, not in an HWND.
  ctx->out += L"\nWINDOW|" + RoleFields(Lookup(*ctx->known, hWnd)) +
              L"|title=" + Escape(title) +
              L"|class=" + Escape(cls) + L"|" + GeometryFields(hWnd);
  ctx->count++;
  return TRUE;
}

}  // namespace

// Every top-level window this process owns, with where it is (#259).
//
// The render window, every tool window, and any per-display window, in one
// terminated batch. Nothing could read a window's position back before this:
// SET_WINDOW could move the render window and UI_MOVE could move a tool
// window, but neither had a reader, so a client arranging itself around us --
// which is what Milkwave Remote does -- had to guess.
void SendWindowList(const std::vector<KnownWindow>& known) {
  ListCtx ctx;
  ctx.known = &known;
  EnumWindows(ListProc, (LPARAM)&ctx);

  // A child renders in its OWN PROCESS, so the enumeration above -- filtered
  // to our pid, as it must be -- cannot see it. Without this a three-display
  // setup with two children reports one window and looks correct.
  for (const KnownWindow& k : known) {
    if (!k.pid || k.pid == GetCurrentProcessId()) continue;
    ctx.out += L"\nWINDOW|" + RoleFields(&k);
    if (!k.hWnd) {
      // Spawned but its window is not up yet, or it died. Reported rather than
      // omitted: "display 1 is a child whose window has not appeared" and
      // "display 1 is not a child" are different states and a caller waiting
      // for the first must not read it as the second.
      ctx.out += L"|title=|class=|state=nowindow";
    } else {
      wchar_t t[256] = {};
      GetWindowTextW(k.hWnd, t, 256);
      wchar_t cls[128] = {};
      GetClassNameW(k.hWnd, cls, 128);
      ctx.out += L"|title=" + Escape(t) + L"|class=" + Escape(cls) +
                 L"|" + GeometryFields(k.hWnd);
    }
    ctx.count++;
  }

  // Terminated, deliberately. UI_LIST is not, so a client has no way to know
  // it has them all and waits out its timeout instead -- which is how a probe
  // reading UI_LIST looked like a hang in this very session.
  wchar_t head[64];
  FormatTo(head, L"WINDOWS|count=%d", ctx.count);
  g_pipeServer.Send((std::wstring(head) + ctx.out + L"\nWINDOWS_END").c_str());
}

// One window: the render window when no title is given, else a match.
//
// A title is matched against our own windows first and then against the known
// table, so "the child on display 2" is reachable by its caption even though
// it is another process and FindToolWindowByTitle cannot see it.
void SendWindowGeometry(const std::wstring& title,
                        const std::vector<KnownWindow>& known) {
  const KnownWindow* k = nullptr;
  HWND hWnd = nullptr;

  if (title.empty()) {
    for (const KnownWindow& e : known)
      if (e.role == L"render") { k = &e; hWnd = e.hWnd; break; }
  } else {
    hWnd = FindToolWindowByTitle(title);
    if (hWnd) {
      k = Lookup(known, hWnd);
    } else {
      const std::wstring want = Lower(title);
      for (const KnownWindow& e : known) {
        if (!e.hWnd) continue;
        wchar_t t[256] = {};
        GetWindowTextW(e.hWnd, t, 256);
        if (Lower(t) == want) { k = &e; hWnd = e.hWnd; break; }
      }
    }
  }

  if (!hWnd) {
    g_pipeServer.Send(L"WINDOW=NOTFOUND");
    return;
  }
  wchar_t t[256] = {};
  GetWindowTextW(hWnd, t, 256);
  g_pipeServer.Send((L"WINDOW|" + RoleFields(k) + L"|title=" + Escape(t) +
                     L"|" + GeometryFields(hWnd)).c_str());
}

bool HandleWindowDiagIPC(const wchar_t* sMessage, const std::wstring& baseDir,
                         const std::vector<KnownWindow>& known) {
  if (!sMessage) return false;

  // GET_WINDOWS before GET_WINDOW: the shorter verb is a PREFIX of the longer
  // one, so checking it first would swallow the plural and report a single
  // window to a caller that asked for every one of them. These are exact
  // compares rather than prefix ones, which makes the ordering belt and
  // braces -- but the ordering is kept, because the next verb added here will
  // be written by copying one of these.
  if (wcscmp(sMessage, L"GET_WINDOWS") == 0) {
    SendWindowList(known);
    return true;
  }
  if (wcscmp(sMessage, L"GET_WINDOW") == 0) {
    SendWindowGeometry(L"", known);
    return true;
  }
  if (wcsncmp(sMessage, L"GET_WINDOW=", 11) == 0) {
    SendWindowGeometry(sMessage + 11, known);
    return true;
  }

  // Every control on one of our own tool windows: class, id, rect, visibility,
  // whether the client edge clips it, its text AND that text's codepoints.
  // The codepoints are the point -- a button whose glyph escape was mangled
  // into the system codepage still prints as something, and only the
  // codepoints show U+E8E5 arriving as U+00C3 U+00A8 U+0045 U+0035.
  if (wcsncmp(sMessage, L"DIAG_WINDOW=", 12) == 0) {
    SendWindowControlDump(sMessage + 12);
    return true;
  }
  if (wcsncmp(sMessage, L"CAPTURE_WINDOW=", 15) == 0) {
    SendWindowCapture(sMessage + 15, baseDir);
    return true;
  }
  return false;
}

}  // namespace mdrop
