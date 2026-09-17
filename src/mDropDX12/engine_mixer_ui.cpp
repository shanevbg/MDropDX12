/*
  Plugin module: Audio Mixer ToolWindow
  Drives the mixer solely through MixerStateLines()/MixerHandleCommand().
  Contains: State-line parsing helpers (ParseRecords, allow-list row
            cells, SortAllowRowsForDisplay), MixerWindow open/close,
            DoBuildControls (fader strips + failover allowlist ListView),
            RefreshValues, DoCommand/DoNotify/DoHScroll/DoContextMenu,
            active-state tracking (LooksActive, SetActive)
*/

// engine_mixer_ui.cpp — the Audio Mixer ToolWindow.
//
// Deliberately talks to the mixer only through the string facade that
// engine_mixer.cpp already exposes to IPC: MixerStateLines() to read, and
// MixerHandleCommand() to write. Nothing here includes audio_mixer.h, so the
// mixer module stays free of engine types and the window and the Android
// client drive exactly the same verbs -- a difference between what a button
// does and what a remote does cannot creep in.
#include "engine.h"
#include "tool_window.h"
#include "seen_format.h"
#include "order_group.h"
#include "config_store.h"
#include "mixer_settings.h"
#include "utility.h"
#include "format_to.h"

#include <algorithm>
#include <commctrl.h>
#include <map>
#include <set>
#include <string>
#include <vector>

namespace mdrop {

#define PAGE_CTRL(page, expr) TrackPageControl(page, (expr))

namespace {

// One record from the state facade: "MIXER_FADER|ch=…|vol=…" becomes its kind
// and its fields.
struct Rec {
  std::wstring kind;
  std::map<std::wstring, std::wstring> f;

  const std::wstring& Get(const wchar_t* key) const {
    static const std::wstring empty;
    auto it = f.find(key);
    return it == f.end() ? empty : it->second;
  }
  int GetInt(const wchar_t* key) const { return _wtoi(Get(key).c_str()); }
  // For a field whose ABSENCE means something different from zero. Battery is
  // one: -1 is "no figure to show" and 0 would be a flat battery, and an
  // absent field parses to 0 either way.
  int GetIntOr(const wchar_t* key, int fallback) const {
    const std::wstring& v = Get(key);
    return v.empty() ? fallback : _wtoi(v.c_str());
  }
  float GetFloat(const wchar_t* key) const {
    return (float)_wtof(Get(key).c_str());
  }
};

std::vector<Rec> ParseRecords(const std::vector<std::wstring>& lines) {
  std::vector<Rec> out;
  for (const std::wstring& line : lines) {
    if (line.empty()) continue;
    Rec rec;
    size_t start = 0;
    bool first = true;
    for (size_t i = 0; i <= line.size(); i++) {
      if (i != line.size() && line[i] != L'|') continue;
      const std::wstring part = line.substr(start, i - start);
      start = i + 1;
      if (first) {
        rec.kind = part;
        first = false;
        continue;
      }
      const size_t eq = part.find(L'=');
      // Ids carry '=' inside them, so only the FIRST one separates.
      if (eq != std::wstring::npos)
        rec.f[part.substr(0, eq)] = part.substr(eq + 1);
    }
    if (!rec.kind.empty()) out.push_back(rec);
  }
  return out;
}

// Trims an endpoint name down to the part that identifies it.
//
// Windows names a virtual endpoint at length and with repetition:
// "SteelSeries Sonar - Chat (SteelSeries Sonar Virtual Audio Device)". In a
// label column two fifths of the row wide, six of those truncated to
// "SteelSeries Sonar -" and could not be told apart at all -- the half that
// says WHICH channel it is, is the half that gets cut.
//
// So: keep what follows the last " - ", then drop a trailing parenthetical
// from it. A name with no " - " keeps its brackets, because that is where the
// model number lives -- "Headphones (WF-1000XM4)" means nothing without them.
// The full name is on the tooltip either way.
std::wstring ShortNameTail(const std::wstring& name);

std::wstring ShortName(const std::wstring& name) {
  // "Microphone" is Windows' word and it is three times longer than it needs
  // to be. Shane asked for it directly, looking at a fader column where
  // "Microphone (EMEET SmartCam C960 4K)" ran off the end: "for fader
  // channels, can you mic instead of Microphone".
  //
  // Only this one word. "Headphones" and "Headset" are the A2DP and hands-free
  // endpoints of the same physical device and have to stay tellable apart --
  // abbreviating both is how they would stop being -- and the model number in
  // the bracket is the part that identifies the device, so none of that moves.
  // The tooltip carries the unabbreviated name either way.
  std::wstring in = name;
  if (in.rfind(L"Microphone", 0) == 0 &&
      (in.size() == 10 || in[10] == L' ' || in[10] == L'('))
    in = L"Mic" + in.substr(10);
  return ShortNameTail(in);
}

// The rest of the trimming, on a name whose prefix has already been dealt with.
std::wstring ShortNameTail(const std::wstring& name) {
  auto dropTrailingBracket = [](const std::wstring& in) -> std::wstring {
    const size_t open = in.rfind(L" (");
    if (open == std::wstring::npos || in.empty() || in.back() != L')')
      return in;
    const std::wstring head = in.substr(0, open);
    return head.empty() ? in : head;
  };

  const size_t dash = name.rfind(L" - ");
  if (dash != std::wstring::npos && dash + 3 < name.size())
    return dropTrailingBracket(name.substr(dash + 3));

  // No leading qualifier: only remove a bracket that merely repeats the name.
  const size_t open = name.rfind(L" (");
  if (open == std::wstring::npos || name.empty() || name.back() != L')')
    return name;
  const std::wstring head = name.substr(0, open);
  const std::wstring tail = name.substr(open + 2, name.size() - open - 3);
  return head.find(tail) != std::wstring::npos ? head : name;
}

// The bracket tag on a multi-fader row: WHICH of that channel's faders it is.
//
// [P] is the personal level, what you hear; [S] is the streaming one, what
// goes out. Sonar publishes exactly that pair per channel in streamer mode
// ("monitoring" and "streaming"), so every multi-fader channel carries both
// and the tag means the same thing the whole way down the column -- Aux [P],
// Aux [S], Chat [P], Chat [S], and so on.
//
// These letters used to mean something else entirely: the two hotkey slots,
// one pair in the whole list. That was the wrong meaning and it caused real
// confusion; the slots themselves are gone now.
//
// A fader that is neither returns nothing and the row falls back to its own
// label, so a provider publishing three faders never has two of them read as
// the same row.
const wchar_t* RoleTag(const std::wstring& faderId) {
  if (faderId == L"monitoring") return L" [P]";
  if (faderId == L"streaming")  return L" [S]";
  return nullptr;
}

// Marks a device Windows knows about but that is not connected, and says when
// it last was. Decoration only: everything from this marker on is stripped
// before the name is stored, because the name is what the failover matches on
// when the device comes back.
//
// It used to read "(not connected), last seen <date>", which Shane could not
// read in the dropdown -- the interesting half was past the right-hand edge.
// The date alone says "not connected"; saying it twice cost the room. "seen"
// went the same way for the same reason: "Last:" carries it, and the word was
// paying no rent.
//
// The time is kept, not just the day: three of his four Sony pairings were
// last seen on the SAME date and are told apart only by the hour.
//
// The trailing space is part of the marker, so every caller appends the value
// straight on and none of them has to remember to add one.
const wchar_t* const kOfflineSuffix = L"   Last: ";

// Marks a headset's hands-free endpoint in any list that offers it.
//
// Windows publishes one physical headset twice: "Headphones (X)" is A2DP,
// stereo and high quality, and "Headset (X)" turns the microphone on and drops
// to mono narrowband. Shane: "the headset is terrible and once that channel
// gets enabled I have to switch headsets." Two rows that differ by one word
// and cost a change of headsets to confuse are worth spelling out.
const wchar_t* const kHandsFreeSuffix = L"   [mic - low quality]";

// How a preferred-device row reads.
//
// Shane asked to see which of them are actually there: "sometimes I put away
// my headsets and they don't disconnect when I think they should", and a list
// that looks the same either way cannot answer that. The marker leads, so the
// column scans down the left rather than needing each line read to the end.
const wchar_t* const kHere = L"[+] ";
const wchar_t* const kGone = L"[  ] ";

// Segoe MDL2 Assets: a speaker with waves, and a speaker with a cross.
const wchar_t* const kGlyphLive  = L"\xE767";
const wchar_t* const kGlyphMuted = L"\xE74F";
// Red and green rather than the theme colour, because the whole point is that
// a muted row differs from its neighbours without being read.
const COLORREF kColMuted = RGB(226, 76, 76);
const COLORREF kColLive  = RGB(86, 200, 96);

std::wstring Fmt(const wchar_t* fmt, ...) {
  wchar_t buf[512];
  va_list args;
  va_start(args, fmt);
  _vsnwprintf_s(buf, _countof(buf), _TRUNCATE, fmt, args);
  va_end(args);
  return buf;
}

}  // namespace

// ─────────────────────────────────────────────────────
// Construction and engine bridge
// ─────────────────────────────────────────────────────

// Bigger than it was. At 520x720 every tab had to be trimmed to fit -- the
// order list lost a row, the failover blurb lost a line, and the fader list
// stopped after ten of twenty-five. Shane: "make the window bigger, I don't
// see anyway that's less confusing." Logical pixels, so on a scaled display
// this is about two thirds of that in device pixels.
// True when the device an allowlist entry names is connected right now.
//
// By id when the entry has one, and otherwise by NAME -- an entry for a device
// that was switched off when it was added has no id at all. Either name
// answers, because a device renamed since the entry was made still carries its
// Windows name underneath.
// File-local rather than a member: Rec is defined in this translation unit,
// so it cannot appear in a signature declared in the header.
// Returns the device's record rather than a bool, because everything else the
// row shows comes from the same record and finding it twice would be the only
// alternative.
static const Rec* FindDevice(const std::vector<Rec>& state,
                            const std::wstring& id,
                            const std::wstring& name,
                            bool mustBeActive) {
  for (const Rec& r : state) {
    if (r.kind != L"MIXER_DEVICE") continue;
    if (mustBeActive && r.GetInt(L"active") == 0) continue;
    if (!id.empty() && r.Get(L"id") == id) return &r;
    if (!name.empty() && (r.Get(L"name") == name ||
                          r.Get(L"windowsName") == name)) return &r;
  }
  return nullptr;
}

static const Rec* FindActiveDevice(const std::vector<Rec>& state,
                                   const std::wstring& id,
                                   const std::wstring& name) {
  return FindDevice(state, id, name, true);
}

struct AllowCells {
  std::wstring device;    // presence marker + the name we show it under
  std::wstring battery;   // "50%", or empty when there is none to show
  std::wstring seen;      // when it last was here, or empty when it is here now
};

// A battery percentage as it is shown, anywhere in this window.
//
// Empty for a negative reading, which is the ordinary case rather than an
// error: plenty of devices report nothing at all, and nothing wired -- every
// speaker, every Sonar channel -- can ever report anything. Shared so the
// Mixer tab and the Failover tab cannot drift into two spellings of the same
// number.
static std::wstring BatteryCell(int percent) {
  if (percent < 0) return std::wstring();
  wchar_t buf[16];
  FormatTo(buf, L"%d%%", percent);
  return buf;
}

// One allowlist row, as three columns.
//
// It was one padded string in a LISTBOX, which is why it looked out of place
// beside every other list in this window. The marker still LEADS the device
// column rather than getting a column of its own, because the point of it is
// that presence scans down the left edge without reading each line to the end.
static AllowCells AllowRowCells(const std::vector<Rec>& state,
                                const std::wstring& id,
                                const std::wstring& name) {
  const Rec* here = FindActiveDevice(state, id, name);
  // Our own short name if there is one, since that is what it was given for.
  const std::wstring alias = MixerCfg().AliasFor(id, name);
  const std::wstring label = !alias.empty() ? alias
                           : !name.empty()  ? name
                                            : id;
  AllowCells c;
  c.device = (here ? kHere : kGone) + label;

  if (here) {
    // Battery only for a device that is connected AND reports one. Windows
    // keeps the last figure it was told after a disconnect -- one of Shane's
    // earbud sets has read 1% for months sitting in its case -- so a figure
    // beside a device that is not here would be a number from the past
    // presented as the present. Plenty of devices report nothing at all, which
    // is ordinary rather than an error, so the cell is simply empty.
    c.battery = BatteryCell(here->GetIntOr(L"battery", -1));
    // Last Seen stays empty for something that is here now: the answer would
    // be "now", which is what the marker already says.
    return c;
  }

  // A second lookup, without the active requirement, because the row that
  // needs a date is by definition one FindActiveDevice will not return. An
  // entry added by name for hardware this machine has never seen has no record
  // at all and gets an empty cell rather than a fabricated date.
  const Rec* known = FindDevice(state, id, name, false);
  if (known) c.seen = FriendlySeen(known->Get(L"seen"));
  return c;
}

// Device / Battery / Last Seen, as percentages of the list's width.
//
// Defined once because they are needed twice: at creation, and again on every
// resize. Battery gets more than it needs for "82%" because the HEADER is the
// wider string and a truncated "Batte..." is worse than a little slack.
const wchar_t* const kAllowColTitles[] = { L"Device", L"Battery", L"Last Seen" };
const int kAllowColPct[] = { 54, 18, 28 };

// What the View combo offers. Index 0 is the rule's own order and the default;
// the rest name a column. Kept next to the titles so the two cannot drift.
const wchar_t* const kAllowSortNames[] = { L"Preferred order", L"Device",
                                           L"Battery", L"Last Seen" };

// Order the allowlist FOR DISPLAY. The rule is never touched (forgejo#69).
//
// Every case here sorts the underlying fact, never the cell text, and each is
// a trap that a naive string sort falls into:
//
//   Device     the cell is prefixed with a presence marker, so sorting it
//              sorts by connected-ness first and name second. Ordering by the
//              name alone is what someone picking a device out of a list of
//              near-identical pairings actually wants.
//
//   Battery    the cell is "50%" or empty, and "100%" sorts before "9%" as
//              text. Empty is ORDINARY -- plenty of devices report nothing,
//              and a disconnected one deliberately reports nothing -- so those
//              go together at the END rather than scattered through the list
//              or claiming to be flat.
//
//   Last Seen  the cell is display-formatted ("Today 13:04", "Ystrdy 15:46")
//              and sorting THAT puts Today below Ystrdy and both above every
//              real date. The absolute value behind it is sorted instead --
//              the same rule the device dropdown follows with `a.seen > b.seen`
//              and the same reason MIXER_DEVICE keeps the absolute form on the
//              wire. It is also EMPTY for a device that is here now, because
//              the presence marker already says so; those are the most
//              recently seen of all and sort FIRST, where a string sort would
//              file them last.
//
// Most-useful-first in every case: newest sighting, fullest battery, A to Z.
// Stable, so within a group the preferred order still shows through.
template <typename RowT>
void SortAllowRowsForDisplay(std::vector<RowT>& rows,
                             const std::vector<Rec>& state, int mode) {
  if (mode <= 0 || mode > 3) return;   // 0 is the rule's own order

  // The absolute "YYYY-MM-DD HH:MM" behind the formatted cell, and the battery
  // as a number. Read from the state records rather than from the cells, which
  // is the whole point.
  const auto factsFor = [&state](const MixerAllowEntry& e,
                                 std::wstring* seen, int* battery,
                                 bool* here) {
    *seen = L"";
    *battery = -1;
    *here = false;
    for (const Rec& d : state) {
      if (d.kind != L"MIXER_DEVICE") continue;
      const bool byId   = !e.id.empty() && d.Get(L"id") == e.id;
      const bool byName = !e.name.empty() && d.Get(L"windowsName") == e.name;
      if (!byId && !byName) continue;
      *seen = d.Get(L"seen");
      *battery = d.GetIntOr(L"battery", -1);
      *here = d.GetInt(L"active") != 0;
      if (byId) break;      // an id match is the better one; keep looking only
    }                       // while all we have is a name
  };

  std::stable_sort(rows.begin(), rows.end(),
                   [&](const RowT& a, const RowT& b) {
    std::wstring sa, sb;
    int ba = -1, bb = -1;
    bool ha = false, hb = false;
    factsFor(a.entry, &sa, &ba, &ha);
    factsFor(b.entry, &sb, &bb, &hb);

    if (mode == 1) {          // Device, by name and not by the marker
      const std::wstring na = MixerCfg().AliasFor(a.entry.id, a.entry.name);
      const std::wstring nb = MixerCfg().AliasFor(b.entry.id, b.entry.name);
      const std::wstring la = !na.empty() ? na
                            : !a.entry.name.empty() ? a.entry.name : a.entry.id;
      const std::wstring lb = !nb.empty() ? nb
                            : !b.entry.name.empty() ? b.entry.name : b.entry.id;
      const int cmp = _wcsicmp(la.c_str(), lb.c_str());
      return cmp != 0 ? cmp < 0 : false;
    }

    if (mode == 2) {          // Battery, fullest first, "no figure" last
      if ((ba < 0) != (bb < 0)) return bb < 0;
      if (ba != bb) return ba > bb;
      return false;
    }

    // Last Seen, most recent first. A device that is HERE is more recent than
    // any date, and its cell is deliberately empty.
    if (ha != hb) return ha;
    if (sa.empty() != sb.empty()) return sb.empty() ? false : true;
    if (sa != sb) return sa > sb;
    return false;
  });
}

// The selected row, or -1. A ListView has no LB_GETCURSEL.
static int LvSelection(HWND hList) {
  return (int)SendMessageW(hList, LVM_GETNEXTITEM, (WPARAM)-1, LVNI_SELECTED);
}

static void LvSelect(HWND hList, int row) {
  LVITEMW it = {};
  it.mask = LVIF_STATE;
  it.state = LVIS_SELECTED | LVIS_FOCUSED;
  it.stateMask = LVIS_SELECTED | LVIS_FOCUSED;
  SendMessageW(hList, LVM_SETITEMSTATE, (WPARAM)row, (LPARAM)&it);
  SendMessageW(hList, LVM_ENSUREVISIBLE, (WPARAM)row, FALSE);
}

MixerWindow::MixerWindow(Engine* pEngine) : ToolWindow(pEngine, 640, 900) {}

void Engine::OpenAudioMixerWindow() {
  // A child instance owns no external interfaces, and the mixer is one.
  if (m_bChildMode) return;
  OpenToolWindow(m_mixerWindow);
}

void Engine::CloseAudioMixerWindow() {
  if (m_mixerWindow) {
    m_mixerWindow->Close();
    m_mixerWindow.reset();
  }
}

bool Engine::IsAudioMixerWindowOpen() const {
  return m_mixerWindow && m_mixerWindow->IsOpen();
}

DWORD MixerWindow::GetCommonControlFlags() const {
  return ICC_TAB_CLASSES | ICC_BAR_CLASSES | ICC_UPDOWN_CLASS;
}

void MixerWindow::SendMixer(const std::wstring& command) {
  std::wstring reply;
  m_pEngine->MixerHandleCommand(command.c_str(), reply);
}

// ─────────────────────────────────────────────────────
// Build Controls
// ─────────────────────────────────────────────────────

void MixerWindow::DoBuildControls() {
  HWND hw = m_hWnd;
  if (!hw) return;

  Engine* p = m_pEngine;
  auto L = BuildBaseControls();
  int y = L.y, lineH = L.lineH, gap = L.gap, x = L.x, rw = L.rw, clientW = L.clientW;
  HFONT hFont = GetFont();
  HFONT hFontBold = GetFontBold();

  RECT rcWnd;
  GetClientRect(hw, &rcWnd);
  const int clientH = rcWnd.bottom;

  m_faders.clear();
  m_routeIds.clear();
  m_deviceIds.clear();
  m_allowIds.clear();

  const std::vector<Rec> state = ParseRecords(p->MixerStateLines());
  const bool enabled = p->MixerIsEnabled();

  // Three tabs, not two. The fader list is as long as the machine has audio
  // endpoints -- twelve here -- and with the slot, step and confirmation
  // controls under it on the same page they were laid out past the bottom of
  // the client area and could not be reached at all. Measured: the last of
  // them sat at y=538 in a client 442 tall.
  const wchar_t* tabNames[] = { L"Mixer", L"Failover", L"Options", L"Profiles" };
  // A margin at the foot, not flush to it.
  //
  // `clientH - y` put the tab control's bottom edge exactly on the client
  // edge -- measured one pixel PAST it -- so it sat on the inside of the
  // sizing border and left almost nothing to grab. Combined with the Options
  // page running past the bottom, the window read as un-resizable from below.
  RECT rcTab = BuildTabControl(IDC_MW_MIXER_TAB, tabNames, 4,
                               0, y, clientW, clientH - y - gap);
  const int tabX = rcTab.left + x;
  const int tabY = rcTab.top + 4;
  const int tabRW = rcTab.right - rcTab.left - x;
  // The page's own bottom, not the window's. Laying out against clientH ran
  // rows past the padded edge and clipped the last one.
  const int pageBottom = rcTab.bottom;

  // ════════════════════════════════════════════════════════════════════
  // Page 0: Mixer
  // ════════════════════════════════════════════════════════════════════
  {
    int py = tabY;

    // Refresh shares the top row with the switch that turns the subsystem on.
    // It used to have a row to itself, which cost one fader out of the ten
    // that fit -- and on the tab whose whole job is showing faders, a row is
    // expensive. Right-aligned and anchored there, so it stays put when the
    // window is widened.
    {
      const int bw = MulDiv(70, lineH, 26);
      PAGE_CTRL(0, CreateCheck(hw, L"Enable audio mixer", IDC_MW_MIXER_ENABLE,
                               tabX, py, tabRW - bw - gap, lineH, hFont, enabled));
      PAGE_CTRL(0, TrackAnchored(
          CreateBtn(hw, L"Refresh", IDC_MW_MIXER_REFRESH,
                    tabX + tabRW - bw, py, bw, lineH, hFont),
          kAnchorTopRight));
    }
    py += lineH + gap;

    // ── Health strip ──
    //
    // A provider that is unavailable is the interesting case, and it is the
    // only one that offers the restart: SteelSeries wedges, and Shane's
    // original reason for wanting a Sonar provider at all was that it "gets
    // hung all the time and I can't kill it easily".
    std::wstring health;
    {
      std::map<std::wstring, std::wstring> seen;
      for (const Rec& r : state) {
        if (r.kind != L"MIXER_FADER" && r.kind != L"MIXER_HEALTH") continue;
        const std::wstring& prov = r.Get(L"provider");
        if (prov.empty()) continue;
        seen[prov] = r.Get(L"health");
      }
      for (const auto& kv : seen) {
        if (!health.empty()) health += L"   ";
        health += kv.first + L": " + (kv.second.empty() ? L"?" : kv.second);
      }
      if (health.empty()) health = enabled ? L"no providers" : L"mixer is off";
    }
    PAGE_CTRL(0, CreateLabel(hw, health.c_str(), tabX, py, tabRW, lineH, hFont));
    py += lineH + gap;


    // ── One row per fader ──
    //
    // The widths are capped as a fraction of the row, not left as MulDiv of a
    // fixed pixel count. Shane runs the tool windows at a large font, and at
    // that size a 150px label column scaled to roughly three quarters of the
    // width -- the slider was a sliver at the right-hand edge and the mute box
    // was off the end of the window entirely.
    {
      const bool spinStyle = MixerCfg().Get().spinBoxes;
      // The word "Mute" is gone: the speaker says it, in red or green, and
      // the box beside it is what you click. That reads down the column at a
      // glance instead of one label at a time, and it gives the row back the
      // width the word used to cost.
      const int muteW = lineH;                    // just the tick box
      const int rightPad = gap * 2;               // and it is not flush to the edge
      const int iconW = lineH;          // one glyph, no label

      // Rebuilt with the controls: a font outlives nothing here, and the old
      // one would leak on every font-size change.
      if (m_hMuteFont) DeleteObject(m_hMuteFont);
      m_hMuteFont = CreateFontW(-(lineH * 5 / 9), 0, 0, 0, FW_NORMAL, FALSE,
                                FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                                CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                                DEFAULT_PITCH | FF_DONTCARE, L"Segoe MDL2 Assets");
      // Half the row. Endpoint names are long and the distinguishing part is
      // at the front, so at two fifths "Speakers (NVIDIA Broadcast)" and
      // "Microphone (NVIDIA Broadcast)" still both ran out mid-word. A slider
      // does not need the space as much as the name does.
      const int labelW = (std::min)(MulDiv(240, lineH, 26), tabRW / 2);
      int index = 0;

      // A channel with one fader gets one row labelled by the CHANNEL. Every
      // Windows endpoint is such a channel, so heading each of them with its
      // own name and then writing "Volume" underneath doubled the length of
      // the list and pushed the controls below it off the bottom.
      std::map<std::wstring, int> faderCount;
      for (const Rec& r : state)
        if (r.kind == L"MIXER_FADER") faderCount[r.Get(L"ch")]++;

      // ── The user's order first, then anything new ──
      //
      // A device that appears after the order was saved goes to the BOTTOM
      // rather than wherever the provider happened to put it: a new endpoint
      // must not push the faders someone uses every day off the visible part
      // of the list.
      std::vector<const Rec*> faders;
      {
        std::vector<const Rec*> remaining;
        for (const Rec& r : state)
          if (r.kind == L"MIXER_FADER") remaining.push_back(&r);

        for (const std::wstring& key : p->MixerFaderOrder()) {
          for (size_t i = 0; i < remaining.size(); i++) {
            if (remaining[i]->Get(L"ch") + L"|" + remaining[i]->Get(L"id") != key)
              continue;
            faders.push_back(remaining[i]);
            remaining.erase(remaining.begin() + i);
            break;
          }
        }
        for (const Rec* r : remaining) faders.push_back(r);
      }

      // The MANUAL order, before any view rule touches it.
      //
      // This is what the Options list edits and what gets SAVED, and it is
      // taken here for the same reason hiding is applied later than it looks
      // like it should be: a view rule must never rewrite the stored order.
      // It did. m_orderKeys was built from the list AFTER the unmuted-first
      // partition, so every move wrote the sort back into mixer.json, and a
      // channel that happened to be unmuted climbed a little each time anyone
      // reordered anything -- which is how a device "moves to the top on its
      // own". Observed in his mixer.json, whose stored order came back from a
      // session reshuffled against the order he had arranged.
      const std::vector<const Rec*> manualOrder = faders;

      // ── The view rules, applied by the ENGINE and not repeated here ──
      //
      // Floating unmuted channels, pinning failover devices and hiding
      // provider-owned endpoints used to be worked out in this function, which
      // meant they existed only while the window was open and only in this
      // copy. A remote asking what the list looked like got the stored order
      // instead and rendered something that disagreed with the screen on its
      // first and last rows (forgejo#65).
      //
      // MixerViewRows() is now the single implementation and MIXER_VIEW puts
      // it on the wire, so the window and a phone cannot come to different
      // conclusions -- the failure mode this replaces. Rows are placed by KEY:
      // that reply is computed from its own snapshot, so a fader may appear in
      // one and not the other for a moment, and anything unplaced simply keeps
      // the position the block above gave it.
      {
        std::map<std::wstring, size_t> at;
        for (size_t i = 0; i < faders.size(); i++)
          at[faders[i]->Get(L"ch") + L"|" + faders[i]->Get(L"id")] = i;

        std::vector<const Rec*> viewed;
        viewed.reserve(faders.size());
        std::vector<bool> taken(faders.size(), false);
        for (const Engine::MixerViewRow& vr : p->MixerViewRows()) {
          const auto it = at.find(vr.key);
          if (it == at.end() || taken[it->second]) continue;
          taken[it->second] = true;
          viewed.push_back(faders[it->second]);
        }
        for (size_t i = 0; i < faders.size(); i++)
          if (!taken[i]) viewed.push_back(faders[i]);
        faders.swap(viewed);

        // The arrangement these rows were built for. The poll tick compares
        // against it to decide whether the layout is still the right one.
        m_viewRev = p->MixerOrderRev();
      }
      // Drop the rows a provider owns, unless asked for them.
      //
      // Eight of the twenty-five here are Windows endpoints that SteelSeries
      // Sonar owns. They truncate to exactly the Sonar channel names they
      // shadow -- "Aux", "Media", "Chat" -- sit at 1.000, and ignore a write,
      // so the row that LOOKS like the one you want is a decoy at full while
      // the real sonar:aux sits at 0.030 below the fold (forgejo#51).
      //
      // Hidden rather than marked, because the list being twice as long as it
      // needs to be is half of what makes this unusable: the fader you want is
      // pushed off the bottom by its own decoys. Counted, never silently
      // dropped, and still addressable over IPC -- a view rule, exactly as
      // forgejo#50 requires of hiding.
      const bool showVirtual = MixerCfg().Get().showVirtualEndpoints;

      // The ORDER is taken before anything is hidden, and hiding never touches
      // it.
      //
      // m_orderKeys is what the Options list edits and what MixerSetFaderOrder
      // writes out, so building it from the filtered list meant a hidden fader
      // lost its place: one group move saved 15 keys where 25 had been stored,
      // silently dropping every endpoint a provider owns. forgejo#50 states the
      // rule for the flag it proposes -- "hiding must not reorder" -- and it
      // applies just as much to hiding that is a default rather than a choice.
      m_orderKeys.clear();
      m_orderKeys.reserve(manualOrder.size());
      m_orderVisible.clear();
      for (const Rec* r : manualOrder) {
        const bool hide = !showVirtual && r->GetInt(L"virtual") != 0;
        // Every fader is in the ORDER; only the unhidden ones are in the VIEW.
        //
        // The two were one array and could not be: built from the filtered
        // list the save dropped hidden faders, and built from the full list
        // the Options list filled with the decoys the Mixer tab hides -- five
        // rows named Aux, Media, Chat, Gaming, Stream that are not the
        // channels of those names, so reordering appeared to do nothing
        // (forgejo#53).
        if (!hide) m_orderVisible.push_back((int)m_orderKeys.size());
        m_orderKeys.push_back(r->Get(L"ch") + L"|" + r->Get(L"id"));
      }

      // Two different reasons a fader is not drawn, counted apart because the
      // footer explains them differently and the remedies are different.
      //
      // `virtual` is ours: a device another program owns. `hidden` is the
      // user's, and until now the Mixer tab ignored it entirely -- the flag was
      // stored, the phone honoured it, and the strip it was set from kept
      // drawing the row. Marking a channel hidden appeared to do nothing.
      //
      // The flag rides on the record already (engine_mixer.cpp emits
      // `hidden=`), so this is a filter, not new plumbing.
      int hiddenVirtual = 0;
      int hiddenByUser = 0;
      const bool showHiddenFaders = MixerCfg().Get().showHiddenFaders;
      {
        std::vector<const Rec*> keep;
        keep.reserve(faders.size());
        for (const Rec* r : faders) {
          if (!showVirtual && r->GetInt(L"virtual") != 0) { hiddenVirtual++; continue; }
          if (!showHiddenFaders && r->GetInt(L"hidden") != 0) { hiddenByUser++; continue; }
          keep.push_back(r);
        }
        faders.swap(keep);
      }
      const int total = (int)faders.size();
      // One line for "N more not shown", one more if any are hidden.
      const int footerLines = 2 + (hiddenVirtual > 0 ? 1 : 0) +
                                  (hiddenByUser > 0 ? 1 : 0);

      // TTM_ADDTOOLW keeps the POINTER, not a copy, so the strings have to
      // outlive this function. They are members for that reason, cleared on
      // every rebuild alongside the control list they describe.
      m_faderNames.clear();
      m_faderNames.reserve((size_t)total);
      std::vector<std::wstring>& tipText = m_faderNames;

      HWND hTip = CreateWindowExW(WS_EX_TOPMOST, TOOLTIPS_CLASSW, NULL,
          WS_POPUP | TTS_ALWAYSTIP | TTS_NOPREFIX,
          CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT,
          hw, NULL, GetModuleHandle(NULL), NULL);
      TrackTooltip(hTip);

      std::wstring lastHeader;
      for (const Rec* pr : faders) {
        const Rec& r = *pr;
        if (index >= IDC_MW_MIXER_MAX_FADERS) break;
        // Reserve for the footer, including the hidden-count line when there
        // is one. Without this the explanation of what is missing was itself
        // pushed off the bottom -- on a full list, which is exactly when it
        // is needed.
        if (py + lineH * footerLines > pageBottom) break;   // out of room

        const std::wstring& ch = r.Get(L"ch");
        const bool multi = faderCount[ch] > 1;

        // A multi-fader channel has NO header. Its name moves into the row
        // label instead, so the two lines that used to read
        //
        //     Aux
        //       Monitoring   [====]
        //       Streaming    [====]
        //
        // now read "Aux [P]" and "Aux [S]" on two lines with nothing above
        // them. Five channels on this machine -- Aux, Chat, Media, Game, Mic
        // -- so five lines come back for faders, which is the whole point:
        // the Mixer tab shows as many rows as fit and the rest are unreachable.
        //
        // "Devices" STAYS. It heads the entire single-fader endpoint run
        // rather than one channel, so it costs one line for the whole list and
        // folding it would leave those rows with no heading at all. It was
        // added for exactly that reason (forgejo#51): endpoint rows used to
        // trail the last channel header and read as belonging to it.
        if (!multi && lastHeader != L"Devices") {
          PAGE_CTRL(0, CreateLabel(hw, L"Devices", tabX, py,
                                   tabRW, lineH, hFontBold));
          py += lineH + 2;
          lastHeader = L"Devices";
        }

        std::wstring full = multi ? r.Get(L"label") : r.Get(L"chname");
        // A name the USER chose is shown exactly as they typed it.
        //
        // ShortName exists to make Windows' own naming fit a column two fifths
        // of a row wide -- it drops everything before " - " and a trailing
        // parenthetical, which is right for "SteelSeries Sonar - Chat (…)" and
        // wrong for a short name someone picked precisely so it would fit.
        // "XM5 - White" would come back as "White", quietly discarding half of
        // a deliberate choice.
        //
        // chwindowsName is present only when a short name has been given, so
        // its presence IS the test -- no comparison against the device list,
        // which a remote could not cheaply do either.
        const std::wstring windowsName = r.Get(L"chwindowsName");
        const bool renamed = !multi && !windowsName.empty();
        std::wstring shown = renamed ? full : ShortName(full);

        // A per-fader short name outranks all of it (forgejo#52).
        //
        // It is the most SPECIFIC thing anyone has said about this row: a
        // device rename covers every fader on the device, and ShortName is a
        // guess at what Windows meant. This names one fader, which is the
        // whole point -- a Sonar channel carries two, and "Aux P" and "Aux S"
        // is the distinction worth spending characters on in a narrow list.
        //
        // Shown verbatim, and it deliberately skips the multi-fader tag block
        // below: appending "— Monitoring" to a name someone chose precisely so
        // it would fit is how you get back the width you were trying to save.
        // The tooltip still carries the unabbreviated pair, so the full
        // identity is never only in the short name.
        const std::wstring userShort = r.Get(L"short");
        if (multi) {
          // The channel name, then which of its faders this row is. The
          // tooltip carries the unabbreviated pair, so the tag is never the
          // only place that distinction exists.
          const wchar_t* tag = RoleTag(r.Get(L"id"));
          const std::wstring chShort = ShortName(r.Get(L"chname"));
          shown = tag ? chShort + tag : chShort + L" \u2014 " + full;
          full = r.Get(L"chname") + L"  \u2014 " + full;
        }
        // The tooltip carries what Windows calls it, for a renamed device.
        // Otherwise a short name would leave no way to find out which physical
        // endpoint a row is -- which matters most on exactly the machine that
        // needs short names, where four pairings differ by one character.
        if (renamed) full += L"  \u2014 " + windowsName;

        // The user's own short name for THIS fader wins the label outright.
        //
        // Applied last, over whatever the rules above produced, because it is
        // the most SPECIFIC thing anyone has said about the row: a device
        // rename covers every fader on that device and ShortName() is a guess
        // at what Windows meant, while this names one fader. That is the whole
        // point of it -- a Sonar channel carries two, and "Aux P" against
        // "Aux S" is the distinction worth spending characters on in a narrow
        // list (forgejo#52).
        //
        // Only `shown` is replaced. `full` is the tooltip and keeps the
        // unabbreviated identity, so a name someone chose six months ago is
        // never the only record of which fader this is.
        if (!userShort.empty()) shown = userShort;

        // No row is bold any more. Bold marked the two hotkey slots, and those
        // are gone; a group's membership is shown by its ticks in the Options
        // tab, not by re-weighting rows here.
        HWND hName = CreateLabel(hw, shown.c_str(), tabX, py, labelW, lineH,
                                 hFont);
        // An endpoint name is Windows' and can be any length, so this column
        // cannot be sized to fit every machine. SS_ENDELLIPSIS ends a name
        // that will not fit with "..." rather than in the middle of a word,
        // and the tooltip below carries all of it (issue 159).
        if (hName)
          SetWindowLongPtrW(hName, GWL_STYLE,
              GetWindowLongPtrW(hName, GWL_STYLE) | SS_ENDELLIPSIS);
        PAGE_CTRL(0, hName);
        // The column cannot be wide enough for every endpoint name on every
        // machine, so the whole name is one hover away rather than lost.
        if (hTip && hName) {
          TTTOOLINFOW ti = { sizeof(ti) };
          ti.uFlags = TTF_SUBCLASS | TTF_IDISHWND;
          ti.hwnd = hw;
          ti.uId = (UINT_PTR)hName;
          tipText.push_back(full);
          ti.lpszText = (LPWSTR)tipText.back().c_str();
          SendMessageW(hTip, TTM_ADDTOOLW, 0, (LPARAM)&ti);
        }
        // The mute box is placed FROM the right edge and anchored to it, and
        // the slider fills what is left. Anchoring the slider wide while
        // leaving the mute box on the default top-left anchor is what put the
        // slider straight over it the first time the window was resized.
        const int sliderX = tabX + labelW + gap;
        const int muteX = tabX + tabRW - muteW - rightPad;
        const int iconX = muteX - iconW;
        // A battery cell between the slider and the speaker icon, carved out of
        // the slider's width. Right-hand furniture, so it is anchored to the
        // right edge with the mute box rather than left where a resize would
        // slide the slider straight over it.
        //
        // The cell yields rather than squeezing the slider to nothing: on a
        // narrow window the level is the control that has to stay usable, and a
        // battery reading is worth nothing if the thing beside it cannot be
        // dragged. Below the floor the cell keeps its position and loses its
        // width, so it simply stops being drawn -- no reflow, no missing
        // control, and it comes back when the window is widened again.
        const int minSliderW = MulDiv(60, lineH, 26);
        int battW = MulDiv(40, lineH, 26);
        if (iconX - gap - sliderX - battW < minSliderW) battW = 0;
        const int battX = iconX - battW;
        const int sliderW = battX - gap - sliderX;
        const int pct = (int)(r.GetFloat(L"vol") * 100.0f + 0.5f);
        if (spinStyle) {
          // A slider cannot be driven one percent at a time with any accuracy,
          // which is what Shane wants for the Aux level: "I like to adjust my
          // aux personal volume in steps of 1". The spinner steps by one and
          // the number can be typed outright.
          //
          // Same control id as the slider it replaces: a row has one or the
          // other, never both, so DoHScroll and EN_CHANGE cannot collide.
          // -10 / +10 beside the spinner. The spinner steps by one, which is
          // what it is for, but crossing a useful distance one press at a time
          // is not -- so the coarse step sits next to the fine one rather than
          // replacing it.
          const int nudgeW = MulDiv(34, lineH, 26);
          const int editW = (std::min)(MulDiv(64, lineH, 26),
                                       sliderW - (nudgeW + gap) * 2);
          int spinX = sliderX;
          PAGE_CTRL(0, CreateBtn(hw, L"\u221210", IDC_MW_MIXER_NUDGE_DN_BASE + index,
                                 spinX, py, nudgeW, lineH, hFont));
          spinX += nudgeW + gap;
          HWND hEdit = CreateEdit(hw, Fmt(L"%d", pct).c_str(),
                                  IDC_MW_MIXER_FADER_BASE + index,
                                  spinX, py, editW, lineH, hFont, ES_NUMBER);
          PAGE_CTRL(0, hEdit);
          // The spinner is UDS_ALIGNRIGHT, so it sits INSIDE the edit's right
          // edge -- +10 goes after the edit, not after the edit plus a button.
          // The old expression added nudgeW a second time and put it under the
          // speaker icon, where it was invisible and unclickable.
          PAGE_CTRL(0, CreateBtn(hw, L"+10", IDC_MW_MIXER_NUDGE_UP_BASE + index,
                                 spinX + editW + gap, py, nudgeW,
                                 lineH, hFont));
          HWND hSpin = CreateWindowExW(0, UPDOWN_CLASSW, L"",
              WS_CHILD | WS_VISIBLE | UDS_SETBUDDYINT | UDS_ALIGNRIGHT |
              UDS_ARROWKEYS | UDS_NOTHOUSANDS,
              0, 0, 0, 0, hw, (HMENU)(INT_PTR)IDC_MW_MIXER_SPIN,
              GetModuleHandle(NULL), NULL);
          if (hSpin) {
            SendMessage(hSpin, UDM_SETBUDDY, (WPARAM)hEdit, 0);
            SendMessage(hSpin, UDM_SETRANGE32, 0, 100);
            SendMessage(hSpin, UDM_SETPOS32, 0, pct);
            PAGE_CTRL(0, hSpin);
          }
        } else {
          HWND hSlider = CreateSlider(hw, IDC_MW_MIXER_FADER_BASE + index,
                                      sliderX, py, sliderW, lineH, 0, 100, pct);
          PAGE_CTRL(0, TrackAnchored(hSlider, kAnchorStretchWide));
        }
        // The battery, for a channel whose device reports one.
        //
        // Created for EVERY row and left empty otherwise, the same rule the
        // speaker below follows: a control that comes and goes reflows the row
        // and makes the column jump as devices connect. Most rows will be empty
        // permanently -- speakers have no battery, and only endpoint channels
        // carry one at all -- and that is the ordinary case, not a gap.
        //
        // Right-anchored with the mute box. The row's whole right-hand group
        // moves together on a resize; anchoring this one to the default
        // top-left is the same mistake that once put the slider over the mute.
        {
          HWND hBatt = CreateLabel(hw, BatteryCell(r.GetIntOr(L"battery", -1)).c_str(),
                                   battX, py, battW, lineH, hFont);
          if (hBatt) {
            SendMessageW(hBatt, WM_SETFONT, (WPARAM)hFont, TRUE);
            SetWindowLongPtrW(hBatt, GWLP_ID, (LONG_PTR)(IDC_MW_MIXER_BATT_BASE + index));
            PAGE_CTRL(0, TrackAnchored(hBatt, kAnchorTopRight));
          }
        }

        // A small red speaker beside the box, so a muted channel is visible
        // at a glance down the column rather than by reading each tick.
        // Created for every row and left empty when unmuted, so the rows stay
        // aligned and a mute toggle does not reflow the list.
        // The speaker IS the mute control. There used to be a tick box beside
        // it saying the same thing twice -- the glyph already reads red or
        // green down the column, so the box added a second thing to look at
        // and a second thing to hit.
        //
        // SS_NOTIFY rather than a button: the colour comes from a FgColor prop
        // read in WM_CTLCOLORSTATIC, which a BUTTON never receives. A button
        // would have to be owner-drawn to stay red, and would gain a frame
        // this row does not want.
        //
        // It takes the width the tick box had as well as its own, so the
        // target is a comfortable click rather than one glyph wide.
        const bool muted = r.GetInt(L"mute") != 0;

        // A fader that reports canMute=0 gets NO speaker: the Sonar master,
        // whose mute would take every render channel with it. The control is
        // left out rather than drawn and ignored, because a green speaker that
        // does nothing when clicked reads as a broken window -- and a red one
        // could never appear, since the mute it would report is refused.
        //
        // The placeholder keeps its width, so the sliders in the rows above and
        // below still line up. Absent canMute (an older record) means true, so
        // nothing changes for providers that do not set it.
        const bool canMute = r.Get(L"canMute").empty() || r.GetInt(L"canMute") != 0;

        HWND hIcon = CreateWindowExW(0, L"STATIC",
            canMute ? (muted ? kGlyphMuted : kGlyphLive) : L"",
            WS_CHILD | WS_VISIBLE | SS_CENTERIMAGE |
                (canMute ? SS_NOTIFY : 0),
            iconX, py, iconW + muteW, lineH, hw,
            (HMENU)(INT_PTR)(IDC_MW_MIXER_ICON_BASE + index),
            GetModuleHandle(NULL), NULL);
        if (hIcon && m_hMuteFont)
          SendMessage(hIcon, WM_SETFONT, (WPARAM)m_hMuteFont, TRUE);
        if (hIcon) {
          SetPropW(hIcon, L"FgColor",
                   (HANDLE)(UINT_PTR)(0x01000000 | (muted ? kColMuted : kColLive)));
          PAGE_CTRL(0, TrackAnchored(hIcon, kAnchorTopRight));
        }
        m_faders.push_back({ ch, r.Get(L"id") });
        py += lineH + 2;
        index++;
      }
      if (index == 0) {
        PAGE_CTRL(0, CreateLabel(hw,
                                 enabled ? L"No channels. Is a provider running?"
                                         : L"Switch the mixer on to see channels.",
                                 tabX, py, tabRW, lineH, hFont));
        py += lineH + gap;
      } else if (index < total) {
        // Says so rather than simply stopping. A list that quietly ends short
        // reads as "that is all this machine has", which is the one thing it
        // does not mean -- make the window taller and the rest appear.
        PAGE_CTRL(0, CreateLabel(hw,
            Fmt(index >= IDC_MW_MIXER_MAX_FADERS
                    ? L"%d more \u2014 put the ones you use at the top, on Options"
                    : L"%d more not shown \u2014 make the window taller",
                total - index).c_str(),
            tabX, py, tabRW, lineH, hFont));
        py += lineH + gap;
      }
      // Never silently gone. A row that vanished without explanation is worse
      // than the decoy it replaced, because there is nothing left to notice.
      if (hiddenVirtual > 0) {
        PAGE_CTRL(0, CreateLabel(hw,
            Fmt(L"%d hidden: owned by another program \u2014 Options to show",
                hiddenVirtual).c_str(),
            tabX, py, tabRW, lineH, hFont));
        py += lineH + gap;
      }
      // The ones the user hid. Named separately from the line above because
      // the reason and the way back are both different -- these were chosen,
      // and right-clicking any row offers "Show hidden channels".
      if (hiddenByUser > 0) {
        PAGE_CTRL(0, CreateLabel(hw,
            Fmt(L"%d hidden by you \u2014 right-click to show hidden channels",
                hiddenByUser).c_str(),
            tabX, py, tabRW, lineH, hFont));
        py += lineH + gap;
      }
    }
  }

  // ════════════════════════════════════════════════════════════════════
  // Page 1: Failover
  // ════════════════════════════════════════════════════════════════════
  {
    int py = tabY;
    const int lw = MulDiv(90, lineH, 26);

    // Three blurb lines, and the list below now takes the page's spare height
    // rather than a fixed count -- see the listH computation further down.
    //
    // The note this replaces said "two lines, not three, and the list below is
    // four rows rather than five", measured when the window's minimum height
    // was 720. It has been three lines and six for a while, and the minimum is
    // 900, so the arithmetic it warned about no longer holds.
    PAGE_CTRL(1, CreateLabel(hw,
        L"When the chosen output disappears, move to the first allowed "
        L"replacement that has been present for the stability window. It "
        L"never moves back on its own.",
        tabX, py, tabRW, lineH * 3, hFont));
    py += lineH * 3 + gap;

    // ── Which route ──
    PAGE_CTRL(1, CreateLabel(hw, L"Route:", tabX, py, lw, lineH, hFont));
    {
      HWND hCombo = CreateCombo(hw, IDC_MW_MIXER_ROUTE, tabX + lw, py, tabRW - lw, lineH * 8, hFont);
      int sel = -1, n = 0;
      for (const Rec& r : state) {
        if (r.kind != L"MIXER_ROUTE") continue;
        SendMessageW(hCombo, CB_ADDSTRING, 0, (LPARAM)r.Get(L"name").c_str());
        m_routeIds.push_back(r.Get(L"id"));
        if (r.Get(L"id") == m_routeId) sel = n;
        n++;
      }
      // Nothing chosen yet: prefer a route that is ARMED.
      //
      // The list used to start on whichever route the providers published
      // first, which is the Windows default output -- so the Add button put
      // Shane's six replacements on the one route he specifically did not want
      // touched, while the armed Sonar monitoring route sat unselected behind
      // it. The route someone has already armed is the one they are working
      // on.
      if (sel < 0 && !m_routeIds.empty()) {
        sel = 0;
        for (size_t i = 0; i < m_routeIds.size(); i++) {
          const MixerRouteCfg* r = MixerCfg().FindRoute(m_routeIds[i]);
          if (r && r->armed) { sel = (int)i; break; }
        }
        m_routeId = m_routeIds[sel];
      }
      SendMessageW(hCombo, CB_SETCURSEL, sel < 0 ? 0 : sel, 0);
      PAGE_CTRL(1, TrackAnchored(hCombo, kAnchorStretchWide));
    }
    py += lineH + gap;

    // ── Current device, read from the route record ──
    {
      std::wstring current = L"(no route)";
      for (const Rec& r : state) {
        if (r.kind != L"MIXER_ROUTE" || r.Get(L"id") != m_routeId) continue;
        const std::wstring& dev = r.Get(L"device");
        current = dev.empty() ? L"(none)" : dev;
        for (const Rec& d : state)     // show the name, not the endpoint id
          if (d.kind == L"MIXER_DEVICE" && d.Get(L"id") == dev)
            current = d.Get(L"name");
      }
      PAGE_CTRL(1, CreateLabel(hw, (L"Now on: " + current).c_str(), tabX, py,
                               tabRW, lineH, hFont));
    }
    py += lineH + gap;

    const MixerRouteCfg* rule = m_routeId.empty()
        ? nullptr : MixerCfg().FindRoute(m_routeId);
    const bool armed = rule && rule->armed;
    PAGE_CTRL(1, CreateCheck(hw, L"Armed \x2014 fail this route over automatically",
                             IDC_MW_MIXER_ARMED, tabX, py, tabRW, lineH, hFont, armed));
    py += lineH + gap;

    // ── The allowlist, in order ──
    {
      // The list refreshes itself once a second while this tab is in front, so
      // this button is for the moment you want to know NOW -- and it re-reads
      // the providers rather than only repainting, because "did it actually
      // disconnect" is a question about the devices, not about the list.
      const int bw = lineH;
      // The heading says "most preferred first" only when that is what the
      // list is showing. Under a column sort it would be a plain lie about the
      // rows on screen, and this window has already been bitten once by a
      // label that described the stored order while the view showed another.
      const int sortMode = MixerCfg().Get().allowSort;
      PAGE_CTRL(1, CreateLabel(hw,
          sortMode == 0 ? L"Allowed replacements, most preferred first:"
                        : L"Allowed replacements, sorted for reading:",
          tabX, py, tabRW - bw - gap, lineH, hFont));
      PAGE_CTRL(1, TrackAnchored(
          CreateBtn(hw, L"\x21BB", IDC_MW_MIXER_ALLOW_REFRESH,
                    tabX + tabRW - bw, py, bw, lineH, hFont),
          kAnchorTopRight));
      py += lineH + 2;

      // A dropdown rather than radios: four choices is too many for a radio
      // row on a tab this narrow. The "Sliders / Spin boxes" radios on the
      // Options tab work because there are two of them.
      const int viewLabelW = MulDiv(38, lineH, 26);
      PAGE_CTRL(1, CreateLabel(hw, L"View:", tabX, py, viewLabelW, lineH, hFont));
      HWND hSort = CreateCombo(hw, IDC_MW_MIXER_ALLOW_SORT, tabX + viewLabelW, py,
        tabRW - viewLabelW, lineH * 8, NULL, CBS_DROPDOWNLIST);
      if (hSort) {
        if (hFont) SendMessage(hSort, WM_SETFONT, (WPARAM)hFont, TRUE);
        for (const wchar_t* name : kAllowSortNames)
          SendMessageW(hSort, CB_ADDSTRING, 0, (LPARAM)name);
        SendMessage(hSort, CB_SETCURSEL, (WPARAM)sortMode, 0);
        PAGE_CTRL(1, TrackAnchored(hSort, kAnchorStretchWide));
      }
    }
    py += lineH + 2;
    {
      // Take whatever the page has spare, rather than a fixed six rows.
      //
      // listH is PIXELS, and the client edge plus the column header eat about
      // 1.3 lines of it before the first row draws -- which is why six lines
      // showed four and a half rows and clipped the fifth mid-glyph. There was
      // a large empty band under the last control the whole time.
      //
      // Everything below is positioned from the running `py` and nothing on
      // this page is bottom-anchored, so the list can claim the room and the
      // rest follows it down. belowH counts those five rows with the same steps
      // they are laid out with: the gap under the list, the device combo,
      // Add/Remove, Short name, and the two timing rows.
      //
      // The clamp reproduces the old height exactly on a page too short to fill
      // -- a very large font, say -- so nothing regresses there.
      const int belowH = lineH * 5 + gap * 4 + 4;
      int listH = pageBottom - py - belowH;
      if (listH < lineH * 6) listH = lineH * 6;
      HWND hList = CreateThemedListView(IDC_MW_MIXER_ALLOW_LIST, tabX, py,
                                        tabRW, listH, false, false);
      // Headers, so the date does not need the word "Last" repeated on every
      // row to say what it is. Same control every other list in this window
      // uses; this one was a bare LISTBOX and looked it.
      {
        LVCOLUMNW col = {};
        col.mask = LVCF_TEXT | LVCF_WIDTH | LVCF_FMT;
        col.fmt = LVCFMT_LEFT;
        for (int i = 0; i < (int)_countof(kAllowColPct); i++) {
          col.pszText = (LPWSTR)kAllowColTitles[i];
          col.cx = MulDiv(tabRW * kAllowColPct[i], 1, 100);
          SendMessageW(hList, LVM_INSERTCOLUMNW, i, (LPARAM)&col);
        }
      }

      // Read straight from config: the allowlist is the rule, and the snapshot
      // only reports where the route is now.
      // Straight off the rule now: an ordered array, not sixteen padded
      // keys that have to stop at the first gap.
      m_allowRows.clear();
      if (rule) {
        // The entries in RULE order first, each with the cells it will draw.
        //
        // Sorting happens on this vector and never on rule->allow: the
        // allowlist order IS the failover preference -- the watcher takes the
        // first entry that is present -- so a view rule that reached the rule
        // would silently repoint the audio. That is not hypothetical in this
        // window: m_orderKeys was once taken after a sort and wrote the sort
        // back into mixer.json, and a channel climbed the list on its own.
        struct Row { MixerAllowEntry entry; AllowCells cells; };
        std::vector<Row> rows;
        rows.reserve(rule->allow.size());
        for (const MixerAllowEntry& e : rule->allow)
          rows.push_back({ e, AllowRowCells(state, e.id, e.name) });

        SortAllowRowsForDisplay(rows, state, MixerCfg().Get().allowSort);

        for (const Row& r : rows) {
          const int row = (int)m_allowRows.size();
          LVITEMW it = {};
          it.mask = LVIF_TEXT;
          it.iItem = row;
          it.pszText = (LPWSTR)r.cells.device.c_str();
          SendMessageW(hList, LVM_INSERTITEMW, 0, (LPARAM)&it);
          ListView_SetItemText(hList, row, 1, (LPWSTR)r.cells.battery.c_str());
          ListView_SetItemText(hList, row, 2, (LPWSTR)r.cells.seen.c_str());
          // Both of these are indexed by DRAWN row, and everything that acts on
          // a selection goes through them -- Remove and Set Name by identity,
          // which a sort does not disturb, and Move by position, which it
          // would. Move is unreachable under a sort for exactly that reason.
          m_allowIds.push_back(r.entry.id.empty() ? r.entry.name : r.entry.id);
          m_allowRows.push_back({ r.entry.id, r.entry.name });
        }
      }
      PAGE_CTRL(1, TrackAnchored(hList, kAnchorStretchWide));
      py += listH + gap;
    }

    // ── Add from the device list ──
    {
      std::set<std::wstring> liveIds;
      for (const Rec& r : state)
        if (r.kind == L"MIXER_DEVICE" && r.GetInt(L"active") != 0)
          liveIds.insert(r.Get(L"id"));

      // Typeable, not just pickable. The list can only offer devices that are
      // CONNECTED -- the watcher publishes active endpoints and nothing else --
      // and the ones most worth listing as replacements are the ones currently
      // switched off. Type the name instead and it is stored by name, which is
      // what the failover matches on when the device comes back anyway.
      HWND hCombo = CreateCombo(hw, IDC_MW_MIXER_DEVICE_COMBO, tabX, py, tabRW, lineH * 10,
        hFont, CBS_DROPDOWN | WS_VSCROLL);
      {
        // What is worth offering: a render endpoint, not already on this
        // route's list, sorted so it can be found.
        //
        // Devices that are switched off are INCLUDED and marked, because a
        // replacement list is a list of somewhere to go when the current
        // device disappears -- the useful entries are the ones not connected
        // right now.
        // name, id, last seen
        struct Offer { std::wstring name, id, seen; bool handsFree = false; };
        std::vector<Offer> offer;
        for (const Rec& r : state) {
          // Render only: failing a playback route over to a microphone would
          // be a strange thing to offer.
          if (r.kind != L"MIXER_DEVICE" || r.Get(L"flow") != L"render") continue;
          // Monitors are dropped -- but only the ones that are NOT connected.
          //
          // Windows keeps an HDMI endpoint per port per graphics card whether
          // anything is plugged in or not, and those were 76 of the 84
          // entries. Form factor alone cannot pick them out, though: measured,
          // "Speakers (NVIDIA Broadcast)" reports DigitalAudioDisplayDevice
          // exactly as a real HDMI port does, and it is a perfectly reasonable
          // place to send audio. Being CONNECTED is the second signal. A
          // display device that is plugged in and working is a real choice; a
          // display device that is not is a port with nothing on the end of it.
          if (r.GetInt(L"display") != 0 && r.GetInt(L"active") == 0) continue;

          const std::wstring& name = r.Get(L"name");
          const std::wstring& devId = r.Get(L"id");
          bool already = false;
          if (rule) {
            for (const MixerAllowEntry& e : rule->allow)
              if ((!e.id.empty() && e.id == devId) ||
                  (!e.name.empty() && e.name == name)) already = true;
          }
          if (already) continue;   // it is in the list above; do not offer it twice
          offer.push_back({ name, devId, r.Get(L"seen"),
                            r.GetInt(L"handsfree") != 0 });
        }

        // Connected first, then alphabetical within each group. A name is what
        // a person searches by, and provider order is not an order anyone can
        // predict.
        // Connected first, alphabetically -- there are few of them and you
        // know their names. The rest go MOST RECENTLY CONNECTED FIRST, which
        // is the order that answers the question actually being asked: with
        // nineteen entries all called WF-1000XM5, the four still in use were
        // connected within a day and the fifteen dead ones months ago, so
        // recency puts the real choices at the top and the wreckage at the
        // bottom. Alphabetical interleaved them.
        //
        // "YYYY-MM-DD HH:MM" sorts correctly as text, which is why it is
        // formatted that way. An unknown time sorts last rather than first: a
        // device we cannot date is a worse guess than one we can.
        std::sort(offer.begin(), offer.end(),
                  [&](const Offer& a, const Offer& b) {
                    // Hands-free endpoints go LAST, whether connected or not.
                    //
                    // They are the same physical headset as the entry above
                    // them, differing by one word, and picking one by mistake
                    // costs Shane a change of headsets -- the mic profile is
                    // mono and narrowband. Sorting them into their own block
                    // at the bottom is what keeps that from being a one-row
                    // slip. They are still offered: somebody may genuinely
                    // want the microphone path.
                    if (a.handsFree != b.handsFree) return b.handsFree;

                    const bool aLive = liveIds.count(a.id) != 0;
                    const bool bLive = liveIds.count(b.id) != 0;
                    if (aLive != bLive) return aLive;
                    if (aLive) return _wcsicmp(a.name.c_str(), b.name.c_str()) < 0;

                    const bool aKnown = !a.seen.empty() && a.seen != L"-";
                    const bool bKnown = !b.seen.empty() && b.seen != L"-";
                    if (aKnown != bKnown) return aKnown;
                    if (aKnown && a.seen != b.seen) return a.seen > b.seen;
                    return _wcsicmp(a.name.c_str(), b.name.c_str()) < 0;
                  });

        for (const Offer& d : offer) {
          const bool live = liveIds.count(d.id) != 0;
          // When it was last seen, on the ones that are not here now. That is
          // what tells four identically-named pairs of earbuds apart: the ones
          // in use were last seen within a day, the dead duplicates months ago.
          //
          // The name stored is the plain one; everything after it is
          // decoration, stripped again on Add so a typed or picked entry still
          // matches.
          std::wstring shown = d.name;
          // Said outright rather than left to the name.
          //
          // "Headphones (X)" and "Headset (X)" differ by one word and are
          // close to opposites, and this app carries its own names for these
          // devices -- so the name cannot be relied on to carry the warning.
          if (d.handsFree) shown += kHandsFreeSuffix;
          if (!live)
            shown += kOfflineSuffix + FriendlySeen(d.seen);
          SendMessageW(hCombo, CB_ADDSTRING, 0, (LPARAM)shown.c_str());
          m_deviceIds.push_back(d.id);
        }
      }
      if (!m_deviceIds.empty()) SendMessageW(hCombo, CB_SETCURSEL, 0, 0);

      // The dropped list is not limited to the width of the box it drops from,
      // and these entries are long. Measured against the real font rather than
      // guessed from a character count, and capped at the width of the display
      // so a stray long name cannot produce a list wider than the screen.
      {
        HDC hdc = GetDC(hCombo);
        HFONT old = (HFONT)SelectObject(hdc, hFont);
        int widest = 0;
        const int count = (int)SendMessageW(hCombo, CB_GETCOUNT, 0, 0);
        for (int i = 0; i < count; i++) {
          wchar_t item[320] = {};
          if (SendMessageW(hCombo, CB_GETLBTEXT, i, (LPARAM)item) == CB_ERR) continue;
          SIZE sz = {};
          if (GetTextExtentPoint32W(hdc, item, (int)wcslen(item), &sz) &&
              sz.cx > widest)
            widest = sz.cx;
        }
        SelectObject(hdc, old);
        ReleaseDC(hCombo, hdc);

        const int scrollbar = GetSystemMetrics(SM_CXVSCROLL);
        const int maxWidth = GetSystemMetrics(SM_CXSCREEN) - gap * 4;
        int want = widest + scrollbar + gap * 4;
        if (want < tabRW) want = tabRW;
        if (want > maxWidth) want = maxWidth;
        SendMessageW(hCombo, CB_SETDROPPEDWIDTH, (WPARAM)want, 0);
      }
      PAGE_CTRL(1, TrackAnchored(hCombo, kAnchorStretchWide));
      py += lineH + 2;

      const int bw = MulDiv(60, lineH, 26);
      PAGE_CTRL(1, CreateBtn(hw, L"Add", IDC_MW_MIXER_ALLOW_ADD, tabX, py, bw, lineH, hFont));
      PAGE_CTRL(1, CreateBtn(hw, L"Remove", IDC_MW_MIXER_ALLOW_REMOVE,
                             tabX + bw + gap, py, bw, lineH, hFont));
      // Which replacement is preferred. The list is already searched top-down
      // for the first one that is present, so moving a row up is exactly
      // "reach for this one first" -- these buttons make an existing rule
      // reachable rather than adding one.
      //
      // Shown ONLY in preferred order. A move edits the rule; seen through a
      // column sort it would take the row somewhere off screen while the
      // visible list sat still, so the button would read as broken. That is
      // the same trap forgejo#65 was about -- a list drawn in one order and
      // moved in another -- and hiding the control is the honest answer rather
      // than explaining the discrepancy after the fact.
      if (MixerCfg().Get().allowSort == 0) {
        const int aw = MulDiv(34, lineH, 26);
        PAGE_CTRL(1, CreateBtn(hw, L"\x2191", IDC_MW_MIXER_ALLOW_UP,
                               tabX + (bw + gap) * 2, py, aw, lineH, hFont));
        PAGE_CTRL(1, CreateBtn(hw, L"\x2193", IDC_MW_MIXER_ALLOW_DOWN,
                               tabX + (bw + gap) * 2 + aw + gap, py, aw, lineH, hFont));
      }
      py += lineH + gap;

      // A short name for whichever row is selected.
      //
      // Four sets of the same earbuds arrive as WF-1000XM5-1 through -4 and
      // Windows will not keep a name for them, so the mixer keeps its own.
      // Stored against the Windows name as well as the id, so it survives the
      // device being switched off -- which is when it has no id at all.
      {
        // Wide enough for the label at Shane's font, where 80 clipped it to
        // "Short".
        const int nameW = MulDiv(115, lineH, 26), setW = MulDiv(95, lineH, 26);
        PAGE_CTRL(1, CreateLabel(hw, L"Short name:", tabX, py, nameW, lineH, hFont));
        PAGE_CTRL(1, TrackAnchored(
            CreateEdit(hw, L"", IDC_MW_MIXER_ALLOW_NAME,
                       tabX + nameW, py, tabRW - nameW - setW - gap * 2, lineH,
                       hFont),
            kAnchorStretchWide));
        PAGE_CTRL(1, TrackAnchored(
            CreateBtn(hw, L"Set Name", IDC_MW_MIXER_ALLOW_SETNAME,
                      tabX + tabRW - setW, py, setW, lineH, hFont),
            kAnchorTopRight));
        py += lineH + gap;
      }
    }

    // ── Timing ──
    {
      const int tw = MulDiv(220, lineH, 26), ew = MulDiv(50, lineH, 26);
      PAGE_CTRL(1, CreateLabelTip(hw, L"Steady for (s):",
                               L"How many seconds a replacement device must stay "
                               L"available before the mixer switches to it.",
                               tabX, py, tw, lineH, hFont));
      PAGE_CTRL(1, CreateEdit(hw,
          Fmt(L"%d", MixerCfg().Get().stabilitySeconds).c_str(),
          IDC_MW_MIXER_STABLE_SECS, tabX + tw, py, ew, lineH, hFont, ES_NUMBER));
      py += lineH + 2;
      PAGE_CTRL(1, CreateLabelTip(hw, L"Min between switches (s):",
                               L"The shortest time allowed between two automatic "
                               L"device switches, in seconds.",
                               tabX, py, tw, lineH, hFont));
      PAGE_CTRL(1, CreateEdit(hw,
          Fmt(L"%d", MixerCfg().Get().minDwellSeconds).c_str(),
          IDC_MW_MIXER_DWELL_SECS, tabX + tw, py, ew, lineH, hFont, ES_NUMBER));
      py += lineH + gap;
    }
  }

  // ════════════════════════════════════════════════════════════════════
  // Page 2: Options
  // ════════════════════════════════════════════════════════════════════
  {
    int py = tabY;

    // ── Fader order ──
    //
    // Most-used at the top. The Mixer tab shows as many faders as fit, so on a
    // machine with two dozen endpoints this list is what decides which ones a
    // person can actually reach without resizing the window.
    // One list, two jobs. The tick column decides which faders the group
    // hotkeys move; the order is what the Mixer tab draws.
    //
    // Deliberately not a second list. This page is already 17px taller than
    // the window it opens in at Shane's DPI (see the Visualizer Input note
    // below), so a control that needed its own five rows would have pushed the
    // bottom of the page out of reach. The rows are the same rows either way.
    {
      const std::vector<MixerGroupCfg>& groupCfgs = MixerCfg().Get().groups;
      const std::wstring groupName =
          groupCfgs.empty() ? std::wstring(L"the group") : groupCfgs[0].name;
      PAGE_CTRL(2, CreateLabel(hw,
          (L"Fader order \x2014 most used first; tick for " + groupName).c_str(),
          tabX, py, tabRW, lineH, hFontBold));
    }
    py += lineH + 2;
    {
      // Five rows, not six. The "Whole channel" box below needs a line of its
      // own -- at a 404-wide client three controls on the button row left it
      // reading "Whole cha" -- and the page has no spare height to give it.
      // The list scrolls, so a row here costs less than a truncated label.
      const int listH = lineH * 5;
      HWND hOrder = CreateThemedListView(IDC_MW_MIXER_ORDER_LIST, tabX, py,
                                         tabRW, listH, false, false);
      // Two columns now, so the header comes back. It costs a row of height and
      // it is what makes the second column readable: a bare glyph column with
      // no heading is a puzzle, and this one is not the row checkbox.
      //
      // The row checkbox is already spoken for -- LVS_EX_CHECKBOXES drives
      // GROUP MEMBERSHIP here, and a ListView has exactly one state checkbox
      // per row. Hidden therefore gets a marker cell in its own column, in the
      // same [ ] / [x] spelling the Failover list uses for presence, and is
      // toggled by clicking it (see DoNotify).
      const int hiddenColW = MulDiv(64, lineH, 26);
      if (hOrder) {
        ListView_SetExtendedListViewStyle(hOrder,
            LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER | LVS_EX_CHECKBOXES);
        LVCOLUMNW col = {};
        col.mask = LVCF_WIDTH | LVCF_TEXT;
        col.cx = tabRW - hiddenColW;
        col.pszText = (LPWSTR)L"Fader";
        SendMessageW(hOrder, LVM_INSERTCOLUMNW, 0, (LPARAM)&col);
        col.cx = hiddenColW;
        col.pszText = (LPWSTR)L"Hidden";
        SendMessageW(hOrder, LVM_INSERTCOLUMNW, 1, (LPARAM)&col);
      }

      // Which keys are ticked, taken once rather than searched per row.
      std::set<std::wstring> ticked;
      {
        const std::vector<MixerGroupCfg>& groupCfgs = MixerCfg().Get().groups;
        if (!groupCfgs.empty())
          ticked.insert(groupCfgs[0].members.begin(), groupCfgs[0].members.end());
      }

      // Names, not keys, and only the rows the Mixer tab actually draws --
      // m_orderVisible indexes into m_orderKeys. A control that lists faders
      // the user cannot see does not describe the thing it changes.
      m_fillingOrderList = true;
      for (int idx : m_orderVisible) {
        const std::wstring& key = m_orderKeys[(size_t)idx];
        std::wstring shown = key;
        for (const Rec& r : state) {
          if (r.kind != L"MIXER_FADER") continue;
          if (r.Get(L"ch") + L"|" + r.Get(L"id") != key) continue;
          shown = ShortName(r.Get(L"chname"));
          if (r.Get(L"label") != L"Volume") shown += L" \x2014 " + r.Get(L"label");
          break;
        }
        LVITEMW it = {};
        it.mask = LVIF_TEXT;
        it.iItem = (int)SendMessageW(hOrder, LVM_GETITEMCOUNT, 0, 0);
        it.pszText = (LPWSTR)shown.c_str();
        const int row = (int)SendMessageW(hOrder, LVM_INSERTITEMW, 0, (LPARAM)&it);
        if (row >= 0) {
          ListView_SetCheckState(hOrder, row, ticked.count(key) ? TRUE : FALSE);
          ListView_SetItemText(hOrder, row, 1,
              (LPWSTR)(MixerCfg().IsHidden(key) ? L"[x]" : L"[ ]"));
        }
      }
      m_fillingOrderList = false;
      PAGE_CTRL(2, TrackAnchored(hOrder, kAnchorStretchWide));
      py += listH + 2;

      // Wide enough for "Move Down" at Shane's font size, where 70 clipped it.
      // Trimmed from 100 to 88 to leave room for the "Whole channel" box on the
      // same row: at a 404-wide client two 100s and the gaps left it reading
      // "Whole". 88 still clears "Move Down" -- checked, not assumed.
      const int bw = MulDiv(88, lineH, 26);
      PAGE_CTRL(2, CreateBtn(hw, L"Move Up", IDC_MW_MIXER_ORDER_UP,
                             tabX, py, bw, lineH, hFont));
      // Ticked, a move takes the channel's faders as one block -- Media's
      // monitoring AND streaming together. Unticked it moves the one row, which
      // is still how you put monitoring above streaming within a channel.
      PAGE_CTRL(2, CreateBtn(hw, L"Move Down", IDC_MW_MIXER_ORDER_DOWN,
                             tabX + bw + gap, py, bw, lineH, hFont));
      py += lineH + 2;
      PAGE_CTRL(2, CreateCheck(hw, L"Move the whole channel",
                               IDC_MW_MIXER_ORDER_GROUP, tabX, py, tabRW,
                               lineH, hFont, m_orderMoveGroup));
      py += lineH + 2;

      // The rest of what can be said about the SELECTED fader, in the group
      // that already says things about it (forgejo#50, forgejo#52). The button
      // reads Hide or Show depending on the row, so it states what it will do
      // rather than what is currently true.
      PAGE_CTRL(2, CreateBtn(hw,
                             m_orderSelHidden ? L"Show fader" : L"Hide fader",
                             IDC_MW_MIXER_HIDE_TOGGLE, tabX, py, bw, lineH,
                             hFont));
      py += lineH + 2;

      // Short name, and the same edit-plus-button shape the allowlist already
      // uses for naming a device -- an empty box clears rather than storing an
      // empty label.
      PAGE_CTRL(2, CreateLabel(hw, L"Short name", tabX, py, bw, lineH, hFont));
      PAGE_CTRL(2, CreateEdit(hw, m_orderSelShort.c_str(),
                              IDC_MW_MIXER_FADER_NAME, tabX + bw + gap, py,
                              bw, lineH, hFont));
      PAGE_CTRL(2, CreateBtn(hw, L"Set", IDC_MW_MIXER_FADER_SETNAME,
                             tabX + 2 * (bw + gap), py, bw, lineH, hFont));
      py += lineH + 2;
      PAGE_CTRL(2, CreateCheck(hw, L"Show unmuted channels first",
                               IDC_MW_MIXER_SORT_UNMUTED, tabX, py, tabRW, lineH,
                               hFont,
                               MixerCfg().Get().sortUnmutedFirst));
      py += lineH + gap;
      // Directly under the sort it overrides, because that is the setting it
      // exists to take the edge off.
      PAGE_CTRL(2, CreateCheck(hw, L"Keep failover devices at the top while connected",
                               IDC_MW_MIXER_PIN_FAILOVER, tabX, py, tabRW, lineH,
                               hFont,
                               MixerCfg().Get().pinFailoverDevices));
      py += lineH + gap;
      // The way BACK from hiding a fader, and the reason hiding is safe to
      // offer at all. Without it a fader hidden from the phone could only be
      // recovered by editing mixer.json (forgejo#50).
      PAGE_CTRL(2, CreateCheck(hw, L"Show hidden faders",
                               IDC_MW_MIXER_SHOW_HIDDEN, tabX, py, tabRW, lineH,
                               hFont,
                               MixerCfg().Get().showHiddenFaders));
      py += lineH + gap;
    }

    // ── How a level is adjusted ──
    {
      const bool spin = MixerCfg().Get().spinBoxes;
      PAGE_CTRL(2, CreateLabel(hw, L"Fader control", tabX, py, tabRW, lineH, hFontBold));
      py += lineH + 2;
      const int half = tabRW / 2 - gap;
      PAGE_CTRL(2, CreateRadio(hw, L"Sliders", IDC_MW_MIXER_STYLE_SLIDER,
                               tabX, py, half, lineH, hFont, !spin, true));
      PAGE_CTRL(2, CreateRadio(hw, L"Spin boxes (steps of 1)",
                               IDC_MW_MIXER_STYLE_SPIN, tabX + half + gap, py,
                               half, lineH, hFont, spin, false));
      py += lineH + gap;
    }

    // -- Rows another program owns --
    {
      PAGE_CTRL(2, CreateLabel(hw, L"Windows endpoints", tabX, py, tabRW,
                               lineH, hFontBold));
      py += lineH + 2;
      // Off by default. On this machine eight of these shadow the Sonar
      // channels, truncate to the same names, sit at 1.000 and ignore a write.
      // The switch exists because the same test catches NVIDIA Broadcast and
      // the GS Wavetable Synth, whose volumes DO work.
      PAGE_CTRL(2, CreateCheck(hw,
          L"Show endpoints another program owns (e.g. Sonar)",
          IDC_MW_MIXER_SHOW_VIRTUAL, tabX, py, tabRW, lineH, hFont,
          MixerCfg().Get().showVirtualEndpoints));
      py += lineH + gap;
    }

    py += gap;

    // ── Step size and the two local confirmations ──
    {
      const int lw = MulDiv(190, lineH, 26), ew = MulDiv(50, lineH, 26);
      PAGE_CTRL(2, CreateLabel(hw, L"Hotkey step (percent):", tabX, py, lw, lineH, hFont));
      PAGE_CTRL(2, CreateEdit(hw,
          Fmt(L"%d", MixerCfg().Get().volumeStepPercent).c_str(),
          IDC_MW_MIXER_STEP, tabX + lw, py, ew, lineH, hFont, ES_NUMBER));
      py += lineH + 2;

      PAGE_CTRL(2, CreateLabelTip(hw, L"Confirm actions (s, 0=off):",
                               L"Seconds to wait for a second press before a "
                               L"hotkey action is carried out. 0 turns the "
                               L"confirmation off.",
                               tabX, py, lw, lineH, hFont));
      PAGE_CTRL(2, CreateEdit(hw,
          Fmt(L"%d", MixerCfg().Get().confirmHotkeySeconds).c_str(),
          IDC_MW_MIXER_CONFIRM_HOTKEY, tabX + lw, py, ew, lineH, hFont, ES_NUMBER));
      py += lineH + 2;

      // The Visualizer Input button shares this row rather than taking one of
      // its own. The page was 17px taller than the window it opens in: the
      // window is created at its saved 640x900 and Windows rescales it to
      // 427x600 on a monitor of different DPI WITHOUT scaling the font, so the
      // layout is for 900 while the client is 562. That button was the last
      // control and hung off the bottom with no way to reach it. One row saved
      // is worth more than a tidy column.
      const int visW = MulDiv(150, lineH, 26);
      const int rstW = MulDiv(130, lineH, 26);
      HWND hCfmWin = CreateCheck(hw, L"Confirm here",
                                 IDC_MW_MIXER_CONFIRM_WINDOW, tabX, py,
                                 tabRW - visW - rstW - gap * 2, lineH, hFont,
                                 MixerCfg().Get().confirmToolWindow);
      PAGE_CTRL(2, hCfmWin);
      // It shares its row with two buttons, so it has about 150 pixels to say
      // this in and the sentence goes on the tooltip. "Confirmation req for
      // this window" needed 342 of them.
      AttachTip(hCfmWin, L"Also ask for confirmation when the action is taken "
                         L"from this window, not only from a hotkey.");
      // "Reset Sonar GUI", beside Visualizer Input at Shane's request.
      //
      // Named for what it does. The old name was "Restart SteelSeries", which
      // reads as though it takes the audio down -- it does not: the audio
      // engine survives, and what wedges is the Electron GUI. Shane, after
      // watching one: "you didn't affect my sound in a negative way, the
      // windows gui for sonar was broken."
      //
      // A button rather than the deliberate acts this used to be limited to --
      // an unbound hotkey and an IPC verb -- because it is now understood well
      // enough to offer. It still goes through the window's confirmation.
      PAGE_CTRL(2, TrackAnchored(
          CreateBtn(hw, L"Reset Sonar GUI", IDC_MW_MIXER_RESET_SONAR,
                    tabX + tabRW - visW - rstW - gap, py, rstW, lineH, hFont),
          kAnchorTopRight));
      PAGE_CTRL(2, TrackAnchored(
          CreateBtn(hw, L"Visualizer Input\x2026", IDC_MW_MIXER_VIS_INPUT,
                    tabX + tabRW - visW, py, visW, lineH, hFont),
          kAnchorTopRight));
      py += lineH + gap;
    }

    // ── Where the visualiser listens ──
    //
    // Not a mixer setting, and the one question this window raises that it
    // cannot answer. A link is duplication of a kind -- Shane said as much --
    // but sending someone off to hunt for a combo box on another window's
    // third tab is worse.
    //
    // Restarting SteelSeries used to sit beside this and is deliberately NOT
    // here any more. Shane: "hide the restart steelseries I don't want bad
    // blood" -- shipping a button that kills another vendor's software reads
    // as a swipe at them, whatever it is for. The capability remains, off the
    // shelf rather than on it: an unbound hotkey the user binds themselves,
    // and MIXER_SONAR_RESTART over IPC. Both are deliberate acts; a button on
    // a tab is an invitation.
  }

  // ─────────────────────────────────────────────────────
  // Page 3 — Profiles
  //
  // One file per profile under resources/profiles/mixer/. The list SHOWS the
  // display name held inside each file and ACTS on the file leaf beside it in
  // m_profileLeaves: the two differ whenever a typed name needed sanitising to
  // become a filename, and re-deriving one from the other would break exactly
  // the names that needed sanitising most.
  // ─────────────────────────────────────────────────────
  {
    int py = tabY;

    PAGE_CTRL(3, CreateLabel(hw, L"Saved profiles \x2014 levels and layout together",
                             tabX, py, tabRW, lineH, hFontBold));
    py += lineH + 2;

    {
      // Six rows. The page below it needs a name field, two button rows and a
      // status line, and the list scrolls where those cannot.
      const int listH = lineH * 6;
      HWND hList = CreateListBox(hw, IDC_MW_MIXER_PROF_LIST, tabX, py, tabRW, listH, hFont,
        WS_BORDER | WS_VSCROLL | LBS_NOINTEGRALHEIGHT, true, 0);

      m_profileLeaves.clear();
      if (m_pEngine) {
        const std::vector<std::wstring> leaves = m_pEngine->ListMixerProfiles();
        // Reading each file to get its name is a few hundred bytes per profile
        // on a page rebuilt only when someone looks at it. Showing the leaf
        // instead would show "my-mix- v2-" for a profile called "my/mix: v2?".
        std::vector<std::wstring> names;
        names.reserve(leaves.size());
        for (const std::wstring& leaf : leaves)
          names.push_back(m_pEngine->MixerProfileName(leaf));

        // Saving twice under one name is allowed -- the second becomes
        // Gaming-2.json rather than overwriting -- so the list can hold several
        // rows reading "Gaming", and a row you cannot tell from its neighbour
        // is a row you cannot safely Load or Delete. Where a name repeats, the
        // filename comes along to separate them; where it does not, the name
        // stands alone and the page stays quiet.
        for (size_t i = 0; i < leaves.size(); i++) {
          int seen = 0;
          for (const std::wstring& other : names)
            if (other == names[i] && ++seen > 1) break;
          // Without the .json: the extension is the same on every row, so it
          // is four characters of noise in the one place the row is trying to
          // say what makes it different.
          std::wstring bare = leaves[i];
          if (bare.size() > 5 &&
              _wcsicmp(bare.c_str() + bare.size() - 5, L".json") == 0)
            bare.resize(bare.size() - 5);
          const std::wstring shown =
              seen > 1 ? names[i] + L"   [" + bare + L"]" : names[i];
          SendMessageW(hList, LB_ADDSTRING, 0, (LPARAM)shown.c_str());
          m_profileLeaves.push_back(leaves[i]);
        }
      }
      if (m_profileLeaves.empty())
        SendMessageW(hList, LB_ADDSTRING, 0, (LPARAM)L"(none saved yet)");
      else
        SendMessageW(hList, LB_SETCURSEL, 0, 0);
      PAGE_CTRL(3, TrackAnchored(hList, kAnchorStretchWide));
      py += listH + gap;
    }

    // Blank is not an error here: an unnamed save banks a timestamp, which is
    // the common case -- "keep this, I will know it by when I made it".
    {
      // "Name (blank = timestamp):" did not fit and drew as "Name (blank =".
      // The affordance moved to the help text below rather than the label
      // growing: this page is also seen at a 427-wide client.
      const int lw = MulDiv(96, lineH, 26);
      PAGE_CTRL(3, CreateLabel(hw, L"Profile name:", tabX, py, lw, lineH, hFont));
      PAGE_CTRL(3, TrackAnchored(
          CreateEdit(hw, L"", IDC_MW_MIXER_PROF_NAME,
                     tabX + lw, py, tabRW - lw, lineH, hFont),
          kAnchorStretchWide));
      py += lineH + 2;
    }

    {
      // Two rows of two. Four across a 404-wide client left every caption
      // clipped, and "Save + all settings" is the longest label on the page.
      const int bw = (tabRW - gap) / 2;
      PAGE_CTRL(3, CreateBtn(hw, L"Save", IDC_MW_MIXER_PROF_SAVE,
                             tabX, py, bw, lineH, hFont));
      // The wider save, spelled out rather than a tick box beside Save: the
      // difference is what ends up in the file, and a profile that quietly
      // carried the failover rules because a box was left ticked is a surprise
      // on the day it is loaded.
      PAGE_CTRL(3, CreateBtn(hw, L"Save + all settings", IDC_MW_MIXER_PROF_SAVEALL,
                             tabX + bw + gap, py, bw, lineH, hFont));
      py += lineH + 2;
      PAGE_CTRL(3, CreateBtn(hw, L"Load", IDC_MW_MIXER_PROF_LOAD,
                             tabX, py, bw, lineH, hFont));
      PAGE_CTRL(3, CreateBtn(hw, L"Delete", IDC_MW_MIXER_PROF_DELETE,
                             tabX + bw + gap, py, bw, lineH, hFont));
      py += lineH + gap;
    }

    // What the last action did. A load that skipped a device has something
    // worth saying -- "12 applied, 3 skipped (not connected)" -- and no other
    // place to say it: this window has no notification area of its own.
    PAGE_CTRL(3, CreateLabel(hw,
        m_profileStatus.empty()
            ? L"Stores every fader's level and mute, the list order and the "
              L"two hotkey slots. Leave the name blank to save a timestamp."
            : m_profileStatus.c_str(),
        // Three lines, not two. At a 470-wide client the sentence wraps to
        // three and the third was drawn half-height, cut off mid-word.
        tabX, py, tabRW, lineH * 3, hFont));
    py += lineH * 3 + gap;

    PAGE_CTRL(3, CreateLabel(hw,
        L"Files live in resources\\profiles\\mixer\\, one per profile.",
        tabX, py, tabRW, lineH, hFont));
  }

  SelectInitialTab();

  // With the subsystem off there is nothing behind any of this: no channels to
  // move, no providers to ask, no routes to arm. Only the switch that turns it
  // on stays live.
  //
  // Visualizer Input stays live too: which device the visualiser listens to is
  // a question worth answering whether or not the mixer is running, and the
  // button only opens another window.
  SetControlsEnabled(enabled, { IDC_MW_MIXER_ENABLE, IDC_MW_MIXER_VIS_INPUT });

  // Runs for the life of the window. See DoMessage: this tick is what keeps
  // the subscription honest when an activation message goes missing.
  SetTimer(hw, 1, 1000, nullptr);
  SetActive(LooksActive());
}

// ─────────────────────────────────────────────────────
// Refresh — values only, never a rebuild
// ─────────────────────────────────────────────────────

void MixerWindow::RefreshValues() {
  HWND hw = m_hWnd;
  if (!hw) return;

  // Whether the preferred-device list is the thing on screen. Decided once:
  // it gates both the battery read below and the row rewrite further down,
  // and the two must not be able to disagree.
  const bool showingDevices = (m_nActivePage == 1 && !m_allowRows.empty());

  // Whether a battery figure is on screen anywhere -- a separate question from
  // showingDevices now, and deliberately a separate variable. The Mixer tab
  // carries a battery cell per fader as well, and it is the tab that is in
  // front most of the time, so gating the read on the Failover tab left the
  // Mixer tab's cells showing whatever the watcher last happened to cache.
  // The HUD line counts as a battery on screen too. It is not in this
  // window, but Shane asked for it to be refreshed "with the other
  // updates" while the mixer is visible -- and without this clause a tick
  // spent on the Options or Profiles tab would refresh nothing, leaving the
  // HUD on its own thirty-second poll with the window right there.
  const bool showingBattery = showingDevices ||
                              (m_nActivePage == 0 && !m_faders.empty()) ||
                              m_pEngine->m_bShowBattery;

  // Battery, before the snapshot is read rather than after.
  //
  // It is the only field here that changes with nothing to announce it --
  // every other one is driven by a device-change callback -- so it is also the
  // only one that has to be asked for. MIXER_BATTERY is synchronous and reads
  // the Bluetooth nodes alone, well under a millisecond, so the very next
  // MixerStateLines() call already carries the new figures. Asking after
  // reading would put every row one tick behind.
  if (showingBattery) SendMixer(L"MIXER_BATTERY");

  const std::vector<Rec> state = ParseRecords(m_pEngine->MixerStateLines());

  // The preferred list, but only while that tab is the one in front.
  //
  // Shane asked for exactly this: no faster than the existing one-second poll,
  // and only when the list is visible -- "it's a waste to have the call
  // otherwise". This runs on the same tick as the faders, so the window still
  // asks the providers once a second and no more.
  //
  // Rewritten IN PLACE rather than rebuilt: a rebuild would throw away the row
  // the user has selected, which is the row they are about to name or remove.
  if (showingDevices) {
    HWND hList = GetDlgItem(hw, IDC_MW_MIXER_ALLOW_LIST);
    if (hList) {
      // A ListView can have a cell's text replaced outright, so the selection
      // and the scroll position simply survive. The LISTBOX this replaced had
      // no such call -- a row's text could only be changed by deleting and
      // re-inserting it, which threw away the selected row (the one the user
      // is about to name or remove) and had to be put back by hand afterwards.
      for (size_t i = 0; i < m_allowRows.size(); i++) {
        const AllowCells c =
            AllowRowCells(state, m_allowRows[i].id, m_allowRows[i].name);
        const wchar_t* cells[3] = { c.device.c_str(), c.battery.c_str(),
                                    c.seen.c_str() };
        for (int col = 0; col < 3; col++) {
          wchar_t have[320] = {};
          LVITEMW get = {};
          get.iSubItem = col;
          get.pszText = have;
          get.cchTextMax = (int)_countof(have);
          SendMessageW(hList, LVM_GETITEMTEXTW, (WPARAM)i, (LPARAM)&get);
          if (wcscmp(have, cells[col]) == 0) continue;
          ListView_SetItemText(hList, (int)i, col, (LPWSTR)cells[col]);
        }
      }
    }
  }
  for (size_t i = 0; i < m_faders.size(); i++) {
    for (const Rec& r : state) {
      if (r.kind != L"MIXER_FADER") continue;
      if (r.Get(L"ch") != m_faders[i].first) continue;
      if (r.Get(L"id") != m_faders[i].second) continue;

      HWND hCtrl = GetDlgItem(hw, IDC_MW_MIXER_FADER_BASE + (int)i);
      // Never while the user has hold of it: a level moved from a phone must
      // not fight a slider being dragged, or a number being typed, here.
      const int want = (int)(r.GetFloat(L"vol") * 100.0f + 0.5f);
      if (hCtrl && GetCapture() != hCtrl && GetFocus() != hCtrl) {
        wchar_t cls[32] = {};
        GetClassNameW(hCtrl, cls, 32);
        if (_wcsicmp(cls, L"Edit") == 0) {
          wchar_t now[16] = {};
          GetWindowTextW(hCtrl, now, 16);
          if (_wtoi(now) != want) SetWindowTextW(hCtrl, Fmt(L"%d", want).c_str());
        } else if ((int)SendMessage(hCtrl, TBM_GETPOS, 0, 0) != want) {
          SendMessage(hCtrl, TBM_SETPOS, TRUE, want);
        }
      }
      // The battery, rewritten only when it has actually changed -- a
      // SetWindowText every second on every row repaints the column
      // continuously for a figure that moves once in several minutes.
      HWND hBatt = GetDlgItem(hw, IDC_MW_MIXER_BATT_BASE + (int)i);
      if (hBatt) {
        const std::wstring want2 = BatteryCell(r.GetIntOr(L"battery", -1));
        wchar_t now[16] = {};
        GetWindowTextW(hBatt, now, 16);
        if (want2 != now) SetWindowTextW(hBatt, want2.c_str());
      }

      // Keeps the indicator in step with a mute toggled anywhere else --
      // a hotkey, a phone, or Sonar itself -- without rebuilding the page
      // under whoever is using it.
      HWND hIcon = GetDlgItem(hw, IDC_MW_MIXER_ICON_BASE + (int)i);
      // Left blank for a fader that cannot be muted, or this would put the
      // speaker back that the build above deliberately left out -- the row
      // would grow a control on the next refresh instead of at creation.
      if (hIcon && (r.Get(L"canMute").empty() || r.GetInt(L"canMute") != 0)) {
        const bool m = r.GetInt(L"mute") != 0;
        SetPropW(hIcon, L"FgColor",
                 (HANDLE)(UINT_PTR)(0x01000000 | (m ? kColMuted : kColLive)));
        SetWindowTextW(hIcon, m ? kGlyphMuted : kGlyphLive);
        // The colour lives in a property the parent reads while painting, so
        // the control has to be asked to repaint for a change to show.
        InvalidateRect(hIcon, NULL, TRUE);
      }
      break;
    }
  }
}

// ─────────────────────────────────────────────────────
// Commands
// ─────────────────────────────────────────────────────

LRESULT MixerWindow::DoCommand(HWND hWnd, int id, int code, LPARAM lParam) {
  UNREFERENCED_PARAMETER(hWnd);
  UNREFERENCED_PARAMETER(lParam);
  Engine* p = m_pEngine;

  // Mute buttons come first: they are a range, not a single id.
  // Mute, from clicking the speaker. STN_CLICKED, because the control is a
  // static with SS_NOTIFY -- the tick box that used to send BN_CLICKED is gone.
  if (id >= IDC_MW_MIXER_ICON_BASE &&
      id < IDC_MW_MIXER_ICON_BASE + IDC_MW_MIXER_MAX_FADERS) {
    if (code != STN_CLICKED) return 0;
    const size_t index = (size_t)(id - IDC_MW_MIXER_ICON_BASE);
    if (index < m_faders.size()) {
      // The window's own confirmation, off by default: a button in your own
      // window is something you reached deliberately.
      if (!p->MixerConfirmRequired(1) || p->MixerConfirmAnswer(true) ||
          !p->MixerConfirmBegin(L"change the mixer")) {
        SendMixer(L"MIXER_MUTE=" + m_faders[index].first + L"|" +
                  m_faders[index].second + L"|toggle");
      }
      // With the sort on, a mute is what decides where the row belongs, so
      // the list is rebuilt to move it. With the sort off nothing reflows: a
      // row jumping under the pointer for no reason would be worse than a
      // stale order.
      if (MixerCfg().Get().sortUnmutedFirst)
        RebuildFonts();
      else
        RefreshValues();
    }
    return 0;
  }

  // A fader edit, in spin mode. Same id range as the sliders, which is safe
  // because a row carries one or the other.
  if (id >= IDC_MW_MIXER_FADER_BASE &&
      id < IDC_MW_MIXER_FADER_BASE + IDC_MW_MIXER_MAX_FADERS) {
    if (code != EN_CHANGE) return 0;
    const size_t index = (size_t)(id - IDC_MW_MIXER_FADER_BASE);
    if (index >= m_faders.size()) return 0;
    wchar_t buf[16] = {};
    GetDlgItemTextW(m_hWnd, id, buf, 16);
    if (!buf[0]) return 0;             // mid-edit, not a value yet
    int value = _wtoi(buf);
    if (value < 0) value = 0;
    if (value > 100) value = 100;
    SendMixer(Fmt(L"MIXER_SET=%s|%s|%.3f", m_faders[index].first.c_str(),
                  m_faders[index].second.c_str(), (float)value / 100.0f));
    return 0;
  }

  // -10 / +10 beside the spin box. Read the CURRENT value off the edit rather
  // than the snapshot, so a press lands on what is on screen even if a refresh
  // has not caught up; setting the edit fires EN_CHANGE, which is what sends
  // MIXER_SET, so the step and the typed value share one path and one clamp.
  if (id >= IDC_MW_MIXER_NUDGE_DN_BASE &&
      id < IDC_MW_MIXER_NUDGE_UP_BASE + IDC_MW_MIXER_MAX_FADERS) {
    const bool up = id >= IDC_MW_MIXER_NUDGE_UP_BASE;
    const int index = up ? id - IDC_MW_MIXER_NUDGE_UP_BASE
                         : id - IDC_MW_MIXER_NUDGE_DN_BASE;
    if ((size_t)index >= m_faders.size()) return 0;
    wchar_t buf[16] = {};
    GetDlgItemTextW(m_hWnd, IDC_MW_MIXER_FADER_BASE + index, buf, 16);
    int value = _wtoi(buf) + (up ? 10 : -10);
    if (value < 0) value = 0;
    if (value > 100) value = 100;
    SetDlgItemTextW(m_hWnd, IDC_MW_MIXER_FADER_BASE + index,
                    Fmt(L"%d", value).c_str());
    return 0;
  }

  switch (id) {
    case IDC_MW_MIXER_ORDER_GROUP:
      // Toggle the model, then tell the control -- never ask the control.
      // CreateCheck makes a BS_OWNERDRAW button and keeps its state in a
      // "Checked" window property, so IsDlgButtonChecked reports 0 forever and
      // the box could not be ticked at all. Every working checkbox in this
      // window does it this way; these two did not.
      m_orderMoveGroup = !m_orderMoveGroup;
      SetChecked(IDC_MW_MIXER_ORDER_GROUP, m_orderMoveGroup);
      return 0;

    case IDC_MW_MIXER_SHOW_VIRTUAL: {
      // Same trap: the model is the truth, the control only displays it.
      MixerCfg().Get().showVirtualEndpoints =
          !MixerCfg().Get().showVirtualEndpoints;
      MixerCfg().Save();
      RebuildFonts();   // the row list is what changed, and redraws the box
      return 0;
    }

    case IDC_MW_MIXER_STYLE_SLIDER:
    case IDC_MW_MIXER_STYLE_SPIN: {
      MixerCfg().Get().spinBoxes = (id == IDC_MW_MIXER_STYLE_SPIN);
      MixerCfg().Save();
      RebuildFonts();                  // every row changes shape
      return 0;
    }

    case IDC_MW_MIXER_ENABLE: {
      const bool want = !p->MixerIsEnabled();
      p->MixerSetEnabled(want);
      SetChecked(IDC_MW_MIXER_ENABLE, p->MixerIsEnabled());
      RebuildFonts();          // the channel list changes entirely
      return 0;
    }

    case IDC_MW_MIXER_REFRESH:
      SendMixer(L"MIXER_REFRESH");
      RebuildFonts();
      return 0;

    case IDC_MW_MIXER_VIS_INPUT:
      // Straight to the control, not merely the page: the System tab holds a
      // dozen settings and the audio device is one combo among them.
      m_pEngine->ShowToolWindowUI(L"OpenSettings", SP_SYSTEM, IDC_MW_AUDIO_DEVICE);
      return 0;

    case IDC_MW_MIXER_RESTART: {
      // Always asks here, whatever the confirm setting says. This one is not
      // a volume change: it takes the whole audio path down for several
      // seconds, and a mis-click would be expensive.
      if (MessageBoxW(m_hWnd,
                      L"Stop every SteelSeries process and start GG again?\n\n"
                      L"Sonar's outputs go away with it and come back "
                      L"when GG has restarted.",
                      L"Restart SteelSeries",
                      MB_YESNO | MB_ICONWARNING | MB_DEFBUTTON2) != IDYES)
        return 0;
      SendMixer(L"MIXER_SONAR_RESTART");
      return 0;
    }

    case IDC_MW_MIXER_ALLOW_SORT: {
      if (code != CBN_SELCHANGE) return 0;
      const int sel = (int)SendMessage(GetDlgItem(m_hWnd, id), CB_GETCURSEL, 0, 0);
      if (sel < 0 || sel > 3) return 0;
      MixerCfg().Get().allowSort = sel;
      MixerCfg().Save();
      // A full rebuild, not a re-sort in place: the ↑/↓ buttons appear and
      // disappear with this setting, and the heading above the list changes
      // with it too.
      RebuildFonts();
      return 0;
    }

    case IDC_MW_MIXER_STEP:
    case IDC_MW_MIXER_CONFIRM_HOTKEY:
    case IDC_MW_MIXER_STABLE_SECS:
    case IDC_MW_MIXER_DWELL_SECS: {
      if (code != EN_CHANGE) return 0;
      wchar_t buf[32] = {};
      GetDlgItemTextW(m_hWnd, id, buf, 32);
      const int value = _wtoi(buf);
      if (id == IDC_MW_MIXER_STEP)
        MixerCfg().Get().volumeStepPercent = value < 1 ? 1 : value;
      else if (id == IDC_MW_MIXER_CONFIRM_HOTKEY)
        SendMixer(Fmt(L"MIXER_CONFIRM_SET=hotkey|%d", value));
      else if (id == IDC_MW_MIXER_STABLE_SECS)
        MixerCfg().Get().stabilitySeconds = value < 1 ? 1 : value;
      else
        MixerCfg().Get().minDwellSeconds = value < 0 ? 0 : value;
      MixerCfg().Save();
      // The watcher re-reads its timings when the subsystem reloads them.
      if (id == IDC_MW_MIXER_STABLE_SECS || id == IDC_MW_MIXER_DWELL_SECS)
        p->MixerLoadFailoverConfig();
      return 0;
    }

    case IDC_MW_MIXER_CONFIRM_WINDOW: {
      const bool want = !MixerCfg().Get().confirmToolWindow;
      SendMixer(Fmt(L"MIXER_CONFIRM_SET=toolwindow|%d", want ? 1 : 0));
      SetChecked(IDC_MW_MIXER_CONFIRM_WINDOW,
                 MixerCfg().Get().confirmToolWindow);
      return 0;
    }

    case IDC_MW_MIXER_SORT_UNMUTED: {
      const bool want = !MixerCfg().Get().sortUnmutedFirst;
      MixerCfg().Get().sortUnmutedFirst = want;
      MixerCfg().Save();
      RebuildFonts();          // the list re-sorts
      // A view rule toggled here reorders every subscriber's list too, and
      // nothing else would tell them: no fader changed and no device arrived.
      p->MixerNotifyViewChanged();
      return 0;
    }

    // ── Profiles tab ──
    //
    // Every one of these acts on the SELECTED ROW mapped through
    // m_profileLeaves, never on the visible text: the text is the display name
    // and the file is the leaf, and they differ exactly when a name had to be
    // sanitised.
    case IDC_MW_MIXER_PROF_SAVE:
    case IDC_MW_MIXER_PROF_SAVEALL: {
      if (!p) return 0;
      wchar_t name[256] = {};
      GetDlgItemTextW(m_hWnd, IDC_MW_MIXER_PROF_NAME, name, 255);
      const bool all = (id == IDC_MW_MIXER_PROF_SAVEALL);
      const std::wstring leaf = p->SaveMixerProfile(name, all);
      m_profileStatus = leaf.empty()
          ? L"Could not write the profile \x2014 see debug.log."
          : Fmt(L"Saved \x201C%s\x201D%s.", p->MixerProfileName(leaf).c_str(),
                all ? L", with all mixer settings" : L"");
      // Clear the field: leaving the name in it invites a second click that
      // silently makes "Gaming-2" rather than overwriting, which is right but
      // reads as nothing having happened.
      SetDlgItemTextW(m_hWnd, IDC_MW_MIXER_PROF_NAME, L"");
      RebuildFonts();          // the list gains a row
      return 0;
    }

    case IDC_MW_MIXER_PROF_LOAD: {
      if (!p) return 0;
      HWND hList = GetDlgItem(m_hWnd, IDC_MW_MIXER_PROF_LIST);
      const int row = (int)SendMessage(hList, LB_GETCURSEL, 0, 0);
      if (row < 0 || (size_t)row >= m_profileLeaves.size()) return 0;
      // The window's own confirmation, same gate the mute button uses: loading
      // a profile moves every fader at once, which is the largest single thing
      // this window can do.
      if (p->MixerConfirmRequired(1) && !p->MixerConfirmAnswer(true) &&
          p->MixerConfirmBegin(L"load a mixer profile"))
        return 0;
      int applied = 0, skipped = 0;
      const std::wstring leaf = m_profileLeaves[(size_t)row];
      if (!p->LoadMixerProfile(leaf, &applied, &skipped)) {
        m_profileStatus = L"That profile could not be read.";
      } else {
        // Skipped is reported rather than hidden: a profile saved with
        // headphones plugged in loads fine without them, and the count is how
        // the user knows which of those two happened.
        m_profileStatus = skipped
            ? Fmt(L"Loaded \x201C%s\x201D \x2014 %d faders set, %d skipped "
                  L"(not connected).", p->MixerProfileName(leaf).c_str(),
                  applied, skipped)
            : Fmt(L"Loaded \x201C%s\x201D \x2014 %d faders set.",
                  p->MixerProfileName(leaf).c_str(), applied);
      }
      RebuildFonts();          // levels, order and the checkboxes all moved
      return 0;
    }

    case IDC_MW_MIXER_PROF_DELETE: {
      if (!p) return 0;
      HWND hList = GetDlgItem(m_hWnd, IDC_MW_MIXER_PROF_LIST);
      const int row = (int)SendMessage(hList, LB_GETCURSEL, 0, 0);
      if (row < 0 || (size_t)row >= m_profileLeaves.size()) return 0;
      const std::wstring leaf = m_profileLeaves[(size_t)row];
      const std::wstring shown = p->MixerProfileName(leaf);
      // Deleting a file is the one irreversible thing on this page, so it asks
      // regardless of the confirmation setting -- that setting is about moving
      // audio, which undoes itself the moment you move it back.
      if (MessageBoxW(m_hWnd,
                      Fmt(L"Delete the profile \x201C%s\x201D?\n\n"
                          L"The file is removed from resources\\profiles\\mixer.",
                          shown.c_str()).c_str(),
                      L"Delete profile", MB_YESNO | MB_ICONWARNING) != IDYES)
        return 0;
      m_profileStatus = p->DeleteMixerProfile(leaf)
          ? Fmt(L"Deleted \x201C%s\x201D.", shown.c_str())
          : L"That profile could not be deleted.";
      RebuildFonts();          // the list loses a row
      return 0;
    }

    case IDC_MW_MIXER_RESET_SONAR: {
      if (!p) return 0;
      // The same gate the mute button uses. Killing a process tree is at least
      // as large a thing as moving every fader at once.
      if (p->MixerConfirmRequired(1) && !p->MixerConfirmAnswer(true) &&
          p->MixerConfirmBegin(L"reset the Sonar GUI"))
        return 0;
      SendMixer(L"MIXER_SONAR_RESTART");
      return 0;
    }

    case IDC_MW_MIXER_PIN_FAILOVER: {
      const bool want = !MixerCfg().Get().pinFailoverDevices;
      MixerCfg().Get().pinFailoverDevices = want;
      MixerCfg().Save();
      RebuildFonts();          // the list re-sorts
      p->MixerNotifyViewChanged();
      return 0;
    }

    case IDC_MW_MIXER_SHOW_HIDDEN:
    case IDC_MW_MIXER_CTX_SHOWHIDDEN: {
      // The Options checkbox and the Mixer tab's context item are the same
      // setting, deliberately sharing one handler: two places that toggle the
      // same stored value must not become two ways of computing it.
      const bool want = !MixerCfg().Get().showHiddenFaders;
      MixerCfg().Get().showHiddenFaders = want;
      MixerCfg().Save();
      RebuildFonts();          // rows appear or disappear
      p->MixerNotifyViewChanged();
      return 0;
    }

    case IDC_MW_MIXER_CTX_HIDE: {
      // Same view-preference semantics as the Options tab's button: the fader
      // keeps its level, its mute state, its place in the order, and still
      // answers every write verb. Only whether it is drawn changes.
      if (m_ctxFaderKey.empty()) return 0;
      const bool want = !MixerCfg().IsHidden(m_ctxFaderKey);
      MixerCfg().SetHidden(m_ctxFaderKey, want);
      MixerCfg().Save();
      // With "Show hidden channels" off, hiding removes the row that was just
      // right-clicked -- correct, and the reason the same menu offers the way
      // back.
      RebuildFonts();
      p->MixerNotifyViewChanged();
      return 0;
    }

    case IDC_MW_MIXER_HIDE_TOGGLE: {
      // Hiding is a VIEW preference: the fader keeps its level, its mute
      // state, its place in the order and its answer to every write verb. The
      // only thing that changes is whether it is drawn (forgejo#50).
      HWND hList = GetDlgItem(m_hWnd, IDC_MW_MIXER_ORDER_LIST);
      const int row = LvSelection(hList);
      if (row < 0 || (size_t)row >= m_orderVisible.size()) return 0;
      const std::wstring key = m_orderKeys[(size_t)m_orderVisible[(size_t)row]];

      const bool want = !MixerCfg().IsHidden(key);
      MixerCfg().SetHidden(key, want);
      MixerCfg().Save();
      m_orderSelHidden = want;
      // With "Show hidden faders" off, hiding removes the row that is
      // selected -- which is correct, and is why the checkbox exists.
      RebuildFonts();
      p->MixerNotifyViewChanged();
      return 0;
    }

    case IDC_MW_MIXER_FADER_SETNAME: {
      HWND hList = GetDlgItem(m_hWnd, IDC_MW_MIXER_ORDER_LIST);
      const int row = LvSelection(hList);
      if (row < 0 || (size_t)row >= m_orderVisible.size()) return 0;
      const std::wstring key = m_orderKeys[(size_t)m_orderVisible[(size_t)row]];

      wchar_t buf[128] = {};
      GetDlgItemTextW(m_hWnd, IDC_MW_MIXER_FADER_NAME, buf, 128);
      // An empty box CLEARS rather than storing an empty label, so a fader
      // goes back to its default name without a second control (forgejo#52).
      MixerCfg().SetShortName(key, buf);
      MixerCfg().Save();
      m_orderSelShort = buf;
      RebuildFonts();          // the fader row relabels
      p->MixerNotifyViewChanged();
      return 0;
    }

    case IDC_MW_MIXER_ORDER_UP:
    case IDC_MW_MIXER_ORDER_DOWN: {
      HWND hList = GetDlgItem(m_hWnd, IDC_MW_MIXER_ORDER_LIST);
      const int row = LvSelection(hList);
      const int step = (id == IDC_MW_MIXER_ORDER_UP) ? -1 : 1;
      const int toRow = row + step;
      // Bounds are checked against the VIEW: the list shows only the faders
      // the Mixer tab draws, so the last visible row is the end of the journey
      // even when hidden entries sit below it in the stored order.
      if (row < 0 || toRow < 0 || (size_t)row >= m_orderVisible.size() ||
          (size_t)toRow >= m_orderVisible.size())
        return 0;

      // Two rows of the view, translated to their places in the full order.
      // Swapping THOSE steps over any hidden entries between them rather than
      // landing on one, so a hidden fader keeps its absolute position and the
      // visible ones move past each other exactly as the list shows.
      const int sel = m_orderVisible[(size_t)row];
      const int to  = m_orderVisible[(size_t)toRow];

      // The KEY of the row being moved, taken before anything is mutated.
      // This is what the selection is restored by afterwards -- see below.
      const std::wstring moved = m_orderKeys[(size_t)sel];

      if (m_orderMoveGroup) {
        MoveOrderGroupKeys(m_orderKeys, sel, to - sel);
      } else {
        std::swap(m_orderKeys[sel], m_orderKeys[to]);
      }
      m_pEngine->MixerSetFaderOrder(m_orderKeys);
      RebuildFonts();

      // Follow the fader by KEY, never by index (forgejo#55).
      //
      // toRow was computed before the rebuild and applied after it, which is
      // right only for a single-step swap of two adjacent visible rows. A group
      // move displaces the selected fader by the SIZE of the group it travels
      // past, not by one; and "Show unmuted channels first" re-partitions the
      // view during the rebuild, so any index taken beforehand can point
      // anywhere. Both were on when this was reported -- "sometimes when moving
      // up the selection loses focus and I get confused as to what it where".
      //
      // The key survives all of it, because RebuildFonts rebuilds
      // m_orderKeys/m_orderVisible from the saved order that was just written.
      HWND hAfter = GetDlgItem(m_hWnd, IDC_MW_MIXER_ORDER_LIST);
      if (hAfter) {
        for (size_t i = 0; i < m_orderVisible.size(); i++) {
          if (m_orderKeys[(size_t)m_orderVisible[i]] != moved) continue;
          // LvSelect scrolls the row into view itself. The listbox this
          // replaced needed three more messages to do that -- walking a fader
          // up a list longer than the box otherwise loses sight of the thing
          // being moved, which is the same confusion by another route.
          LvSelect(hAfter, (int)i);
          break;
        }
      }
      // Focus stays on the button: repeated clicks are how this is used, and
      // moving it to the list would break that.
      return 0;
    }

    case IDC_MW_MIXER_ROUTE: {
      if (code != CBN_SELCHANGE) return 0;
      const int sel = (int)SendMessage(GetDlgItem(m_hWnd, id), CB_GETCURSEL, 0, 0);
      if (sel >= 0 && (size_t)sel < m_routeIds.size()) m_routeId = m_routeIds[sel];
      RebuildFonts();          // the allowlist below belongs to the route
      return 0;
    }

    case IDC_MW_MIXER_ARMED: {
      if (m_routeId.empty()) return 0;
      const MixerRouteCfg* cur = MixerCfg().FindRoute(m_routeId);
      const bool want = !(cur && cur->armed);
      SendMixer(Fmt(L"MIXER_ROUTE_ARM=%s|%d", m_routeId.c_str(), want ? 1 : 0));
      const MixerRouteCfg* now = MixerCfg().FindRoute(m_routeId);
      SetChecked(IDC_MW_MIXER_ARMED, now && now->armed);
      return 0;
    }

    case IDC_MW_MIXER_ALLOW_REFRESH:
      // Re-reads the providers, not just the list. "Did it actually
      // disconnect" is a question about the devices.
      SendMixer(L"MIXER_REFRESH");
      RefreshValues();
      return 0;

    case IDC_MW_MIXER_ALLOW_SETNAME: {
      HWND hList = GetDlgItem(m_hWnd, IDC_MW_MIXER_ALLOW_LIST);
      const int sel = LvSelection(hList);
      if (sel < 0 || (size_t)sel >= m_allowRows.size()) {
        m_pEngine->AddNotification((wchar_t*)L"Pick a device in the list first");
        return 0;
      }
      wchar_t typed[160] = {};
      GetDlgItemTextW(m_hWnd, IDC_MW_MIXER_ALLOW_NAME, typed, 160);

      const AllowRow& row = m_allowRows[sel];
      // Through MIXER_RENAME_DEVICE when there is an id, which is what this
      // file's opening comment requires of every write here: the window and
      // the Android client drive the same verbs, so a difference between what
      // a button does and what a remote does cannot creep in. It had crept in
      // -- this wrote the alias straight into the settings and then asked for
      // a refresh, which is NOT what the verb does. The verb also refreshes
      // the watcher, and tries the Windows-side rename first so the name is
      // visible outside this app when Windows will accept it.
      //
      // An entry with no id is one added while its device was switched off, so
      // there is nothing to rename in Windows and no fader showing the name.
      // Those keep the direct write; MIXER_REFRESH now rebuilds the watcher
      // too, so they refresh correctly either way.
      if (!row.id.empty()) {
        SendMixer(L"MIXER_RENAME_DEVICE=" + row.id + L"|" +
                  (typed[0] ? std::wstring(typed) : std::wstring(L"-")));
      } else {
        if (typed[0]) MixerCfg().SetAlias(row.id, row.name, typed);
        else MixerCfg().ClearAlias(row.name);
        SendMixer(L"MIXER_REFRESH");
      }
      SetDlgItemTextW(m_hWnd, IDC_MW_MIXER_ALLOW_NAME, L"");
      RebuildFonts();
      return 0;
    }

    case IDC_MW_MIXER_ALLOW_ADD: {
      if (m_routeId.empty()) return 0;
      HWND hCombo = GetDlgItem(m_hWnd, IDC_MW_MIXER_DEVICE_COMBO);
      const int sel = (int)SendMessage(hCombo, CB_GETCURSEL, 0, 0);

      // A chosen device is added by id AND name -- an id match is exact, and
      // the name is the fallback for when a Bluetooth device returns under a
      // new one. Anything typed that is not in the list is added by name only,
      // which is the only way to list a device that is not connected.
      std::wstring deviceId, deviceName;   // not 'id': that is the control id
      if (sel >= 0 && (size_t)sel < m_deviceIds.size())
        deviceId = m_deviceIds[sel];
      wchar_t typed[320] = {};
      GetWindowTextW(hCombo, typed, 320);
      deviceName = typed;
      // Every suffix the list adds comes off again, so what gets stored is the
      // plain name the failover matches on. The hands-free marker is cut first
      // because it sits before the date, and erasing at the earlier position
      // takes both.
      for (const wchar_t* suffix : { kHandsFreeSuffix, kOfflineSuffix }) {
        const size_t mark = deviceName.find(suffix);
        if (mark != std::wstring::npos) deviceName.erase(mark);
      }
      if (deviceId.empty() && deviceName.empty()) return 0;

      SendMixer(L"MIXER_ALLOW_ADD=" + m_routeId + L"|" + deviceId + L"|" + deviceName);
      RebuildFonts();
      return 0;
    }

    case IDC_MW_MIXER_ALLOW_REMOVE: {
      if (m_routeId.empty()) return 0;
      const int sel = LvSelection(GetDlgItem(m_hWnd, IDC_MW_MIXER_ALLOW_LIST));
      if (sel < 0 || (size_t)sel >= m_allowIds.size()) return 0;
      SendMixer(L"MIXER_ALLOW_REMOVE=" + m_routeId + L"|" + m_allowIds[sel]);
      RebuildFonts();
      return 0;
    }

    case IDC_MW_MIXER_ALLOW_UP:
    case IDC_MW_MIXER_ALLOW_DOWN: {
      if (m_routeId.empty()) return 0;
      HWND hList = GetDlgItem(m_hWnd, IDC_MW_MIXER_ALLOW_LIST);
      const int sel = LvSelection(hList);
      if (sel < 0 || (size_t)sel >= m_allowIds.size()) {
        m_pEngine->AddNotification((wchar_t*)L"Pick a device in the list first");
        return 0;
      }
      const int delta = (id == IDC_MW_MIXER_ALLOW_UP) ? -1 : 1;
      SendMixer(L"MIXER_ALLOW_MOVE=" + m_routeId + L"|" + m_allowIds[sel] +
                (delta < 0 ? L"|-1" : L"|1"));
      RebuildFonts();
      // Follow the row that just moved, so a second press keeps moving the
      // same device rather than whichever one fell into the selected slot.
      // Clamped, because at either end the move was a no-op and the selection
      // must not walk past the list.
      int want = sel + delta;
      if (want < 0) want = 0;
      if (want >= (int)m_allowIds.size()) want = (int)m_allowIds.size() - 1;
      LvSelect(GetDlgItem(m_hWnd, IDC_MW_MIXER_ALLOW_LIST), want);
      return 0;
    }

    default:
      return -1;
  }
}

// ─────────────────────────────────────────────────────
// Sliders
// ─────────────────────────────────────────────────────

// The allowlist reports selection here rather than through DoCommand: it is a
// ListView now, and a ListView sends WM_NOTIFY. The old LBN_SELCHANGE case
// went silently dead the moment the control changed, which would have left the
// short-name box showing whatever it last held.
// The list stretches with the window; its COLUMNS have to be told to.
//
// Widths are set once at creation from the tab width at that moment, and
// anchoring then stretches the control and leaves them where they were -- so
// widening the window grew empty space to the right of "Last Seen" while
// "Battery" stayed truncated. That is the same shape as forgejo#51's complaint
// that the fader list ignores a resize, in a list small enough to fix here.
// Timer 1 is the once-a-second refresh. This one fires once, after a resize
// stops, to lay the fader rows out for the size the window ended at.
static const UINT_PTR kRelayoutTimer = 2;

void MixerWindow::OnResize() {
  ToolWindow::OnResize();

  // Make the footer's advice true.
  //
  // How many fader rows fit is decided in DoBuildControls, against the page
  // height AT THAT MOMENT. This window anchors twelve controls, so OnResize
  // takes the ApplyAnchors path and never rebuilds -- which left the row count
  // frozen at whatever the window was when it opened, while the footer said
  // "make the window taller" and making it taller did nothing. That is the one
  // instruction on screen and it was the one thing that could not work
  // (forgejo#51).
  //
  // Rebuilt only when the number of rows that FIT has actually changed, not on
  // every WM_SIZE: dragging an edge sends a stream of them, and rebuilding per
  // pixel would flicker the whole page for no gain. Crossing a row boundary is
  // rare enough to be free and is exactly when the answer differs.
  // DEFERRED, never inline.
  //
  // RebuildFonts destroys and re-creates every control in the window. Doing
  // that from inside WM_SIZE runs it in the middle of the sizing loop, and the
  // drag fights it: Shane could not resize the window vertically at all while
  // the row count was crossing boundaries under the mouse.
  //
  // A one-shot timer instead. Each further WM_SIZE just pushes it out again,
  // so a drag rebuilds once when it settles rather than once per row boundary
  // -- and it covers the paths a drag does not, a maximise or a SetWindowPos
  // over IPC, which never send WM_EXITSIZEMOVE.
  // Any size change, not just one that moves a fader-row boundary.
  //
  // Keying it on the fader count was too narrow: it is a Mixer-tab quantity,
  // and the OPTIONS tab lays its controls out at fixed positions with only a
  // few anchored -- so resizing while Options was in front changed nothing
  // visible and read as "I can't resize this tab". Shane: "can resize other
  // tabs just not the options". Every page is laid out against the window
  // height, so every page needs the relayout.
  RECT rc = {};
  if (m_hWnd && GetClientRect(m_hWnd, &rc)) {
    const int w = rc.right - rc.left, h = rc.bottom - rc.top;
    if (w != m_builtClientW || h != m_builtClientH) {
      m_builtClientW = w;
      m_builtClientH = h;
      SetTimer(m_hWnd, kRelayoutTimer, 200, nullptr);
    }
  }

  HWND hList = GetDlgItem(m_hWnd, IDC_MW_MIXER_ALLOW_LIST);
  if (!hList) return;
  RECT rcList = {};
  if (!GetClientRect(hList, &rcList)) return;
  const int w = rcList.right - rcList.left;
  if (w <= 0) return;
  for (int i = 0; i < (int)_countof(kAllowColPct); i++)
    SendMessageW(hList, LVM_SETCOLUMNWIDTH, (WPARAM)i,
                 MulDiv(w * kAllowColPct[i], 1, 100));
}

LRESULT MixerWindow::DoNotify(HWND hWnd, NMHDR* pnm) {
  UNREFERENCED_PARAMETER(hWnd);
  if (!pnm) return 0;

  // A tick in the order list changes which faders the group hotkeys move.
  // A click in the Hidden column toggles it.
  //
  // Its own column and its own marker because the ROW checkbox is already
  // group membership, and a ListView has one state checkbox per row. Hit-tested
  // by subitem so a click anywhere else still just selects, and the Move
  // Up/Down buttons keep working the way they did.
  if (pnm->idFrom == IDC_MW_MIXER_ORDER_LIST && pnm->code == NM_CLICK) {
    const NMITEMACTIVATE* ia = (const NMITEMACTIVATE*)pnm;
    if (ia->iSubItem == 1 && ia->iItem >= 0 &&
        (size_t)ia->iItem < m_orderVisible.size()) {
      const std::wstring key =
          m_orderKeys[(size_t)m_orderVisible[(size_t)ia->iItem]];
      const bool want = !MixerCfg().IsHidden(key);
      MixerCfg().SetHidden(key, want);
      MixerCfg().Save();
      m_orderSelHidden = want;
      // The whole page is rebuilt rather than just this cell: with "Show
      // hidden channels" off the Mixer tab's strip changes too, and the
      // Options button's caption has to follow.
      RebuildFonts();
      m_pEngine->MixerNotifyViewChanged();
      return 0;
    }
  }

  if (pnm->idFrom == IDC_MW_MIXER_ORDER_LIST && pnm->code == LVN_ITEMCHANGED) {
    // Filling the list ticks boxes too, and those arrive here identically.
    if (m_fillingOrderList) return 0;
    const NMLISTVIEW* nm = (const NMLISTVIEW*)pnm;

    // A selection change refreshes the per-fader controls (forgejo#50, #52).
    // Handled BEFORE the state-image test below, which returns early for
    // exactly this case -- selecting a row is an LVN_ITEMCHANGED whose check
    // state did not move.
    if ((nm->uChanged & LVIF_STATE) &&
        ((nm->uOldState & LVIS_SELECTED) != (nm->uNewState & LVIS_SELECTED))) {
      HWND hSel = GetDlgItem(m_hWnd, IDC_MW_MIXER_ORDER_LIST);
      const int sel = LvSelection(hSel);
      std::wstring key;
      if (sel >= 0 && (size_t)sel < m_orderVisible.size())
        key = m_orderKeys[(size_t)m_orderVisible[(size_t)sel]];
      m_orderSelHidden = !key.empty() && MixerCfg().IsHidden(key);
      m_orderSelShort  = key.empty() ? std::wstring()
                                     : MixerCfg().ShortNameFor(key);
      SetDlgItemTextW(m_hWnd, IDC_MW_MIXER_FADER_NAME, m_orderSelShort.c_str());
      // The button says what it will DO, not what is true now.
      SetDlgItemTextW(m_hWnd, IDC_MW_MIXER_HIDE_TOGGLE,
                      m_orderSelHidden ? L"Show fader" : L"Hide fader");
      // CreateBtn makes an owner-draw button, which does not repaint on a
      // text change by itself.
      if (HWND hBtn = GetDlgItem(m_hWnd, IDC_MW_MIXER_HIDE_TOGGLE))
        InvalidateRect(hBtn, NULL, TRUE);
    }

    // Only a change to the STATE IMAGE, which is where a ListView keeps the
    // check. Selecting a row is also an LVN_ITEMCHANGED, and rewriting the
    // group every time the selection moved would be a file write per arrow
    // key -- and would fight the Move Up/Down buttons, which select as they go.
    if (!(nm->uChanged & LVIF_STATE)) return 0;
    if ((nm->uOldState & LVIS_STATEIMAGEMASK) ==
        (nm->uNewState & LVIS_STATEIMAGEMASK))
      return 0;
    // Nothing to write into before the list has been built once.
    if (m_orderVisible.empty()) return 0;

    HWND hList = GetDlgItem(m_hWnd, IDC_MW_MIXER_ORDER_LIST);
    if (!hList) return 0;

    // Read every row rather than toggling the one that changed. The membership
    // IS what is ticked, so sending the whole set cannot drift out of step
    // with the boxes the way an incremental add/remove would.
    //
    // Stored in the list's own order, so a group reads down the page the same
    // way the Mixer tab does.
    std::vector<std::wstring> members;
    const int rows = (int)SendMessageW(hList, LVM_GETITEMCOUNT, 0, 0);
    for (int i = 0; i < rows && (size_t)i < m_orderVisible.size(); i++) {
      if (!ListView_GetCheckState(hList, i)) continue;
      members.push_back(m_orderKeys[(size_t)m_orderVisible[(size_t)i]]);
    }
    m_pEngine->MixerSetGroupMembers(0, members);
    return 0;
  }

  if (pnm->idFrom != IDC_MW_MIXER_ALLOW_LIST || pnm->code != LVN_ITEMCHANGED)
    return 0;

  const NMLISTVIEW* nm = (const NMLISTVIEW*)pnm;
  // Only the transition INTO selected. LVN_ITEMCHANGED also fires for the row
  // being deselected, and acting on that would overwrite the box a moment
  // after filling it.
  if (!(nm->uNewState & LVIS_SELECTED) || (nm->uOldState & LVIS_SELECTED))
    return 0;

  const int sel = nm->iItem;
  if (sel < 0 || (size_t)sel >= m_allowRows.size()) return 0;
  // Selecting a row offers its current short name for editing, so the box
  // shows what is set rather than always being blank.
  SetDlgItemTextW(m_hWnd, IDC_MW_MIXER_ALLOW_NAME,
                  MixerCfg().AliasFor(m_allowRows[sel].id,
                                      m_allowRows[sel].name).c_str());
  return 0;
}

LRESULT MixerWindow::DoHScroll(HWND hWnd, int id, int pos) {
  UNREFERENCED_PARAMETER(hWnd);
  if (id < IDC_MW_MIXER_FADER_BASE ||
      id >= IDC_MW_MIXER_FADER_BASE + IDC_MW_MIXER_MAX_FADERS)
    return -1;

  const size_t index = (size_t)(id - IDC_MW_MIXER_FADER_BASE);
  if (index >= m_faders.size()) return 0;

  // Posted on every notch of a drag. The mixer's queue coalesces these and its
  // worker applies at a bounded rate, so a drag reaches a slow provider as a
  // handful of writes rather than a hundred -- which is exactly why the queue
  // is there.
  SendMixer(Fmt(L"MIXER_SET=%s|%s|%.3f", m_faders[index].first.c_str(),
                m_faders[index].second.c_str(), (float)pos / 100.0f));
  return 0;
}

// ─────────────────────────────────────────────────────
// Activation, the polling rule
// ─────────────────────────────────────────────────────

// Open is not enough. A mixer window sitting behind the visualiser must cost
// nothing, so the subscription follows whether anyone is actually LOOKING at
// it, and a provider is polled only then.
bool MixerWindow::LooksActive() const {
  if (!m_hWnd || !IsWindow(m_hWnd)) return false;
  if (IsIconic(m_hWnd)) return false;
  // SUBSCRIBING stays on being in front. What follows visibility is the
  // refresh, in DoMessage's tick -- the two were briefly made one and this is
  // the half that had to go back (#328).
  //
  // Subscribing makes the worker poll every provider, Sonar over HTTP among
  // them. Driven by a window merely being on screen, that runs constantly, and
  // the moment it costs most is a headset switch: the route moves, devices
  // come and go, and the providers are re-read underneath it. Shane, on the
  // build where these were coupled: "when I switched headsets the lag was
  // horrible."
  //
  // Reading the battery and the failover list needs none of that. It touches
  // the Bluetooth nodes and a snapshot that already exists, so it can follow
  // visibility while this does not.
  return GetForegroundWindow() == m_hWnd;
}

void MixerWindow::SetActive(bool active) {
  if (active == m_subscribed) return;
  m_subscribed = active;
  m_pEngine->MixerSetSubscribed(active);
}


// ─────────────────────────────────────────────────────
// DoContextMenu — right-click on a Mixer tab fader row
// ─────────────────────────────────────────────────────
//
// The base class routes WM_CONTEXTMENU here (tool_window.cpp), which is why
// this is an override rather than a case in DoMessage: DoMessage is only
// reached by the switch's default branch, and WM_CONTEXTMENU never gets there.
// Returning -1 means "not handled", and the base falls through to DefWindowProc.

LRESULT MixerWindow::DoContextMenu(HWND hWnd, int x, int y) {
  UNREFERENCED_PARAMETER(hWnd);
  // Right-click a fader to hide it, or to bring the hidden ones back.
  //
  // Hiding already existed, on the Options tab: select the fader in the
  // ordering list, press "Hide fader". That is the wrong place to be when the
  // thing you want gone is in front of you -- the hands-free endpoint of every
  // headset, typically, which Windows offers as a separate output and which
  // nobody wants a strip for.
  //
  // Offered on the Mixer tab (the strip itself) and on the Options ordering
  // list (the only view that shows a hidden fader WHILE it is hidden, so the
  // natural place to bring one back).
  //
  // "Show hidden channels" is on both menus, and it is what makes hiding safe:
  // without a way back, a hidden fader could only be recovered by editing
  // mixer.json by hand.
  if (m_nActivePage != 0 && m_nActivePage != 2) return -1;

  POINT screen = { x, y };
  // Keyboard menu key gives (-1,-1); put the menu where the pointer is.
  if (screen.x == -1 && screen.y == -1) GetCursorPos(&screen);
  POINT client = screen;
  ScreenToClient(m_hWnd, &client);

  if (m_nActivePage == 0 && m_faders.empty()) return -1;

  // Which row was hit. The row's controls carry the ids; the fader control
  // is on every row in both spin and slider styles, so its vertical band IS
  // the row. Matching on y alone means the whole strip is clickable, not
  // just the few pixels of one control.
  int hit = -1;
  for (size_t i = 0; i < m_faders.size(); i++) {
    HWND h = GetDlgItem(m_hWnd, IDC_MW_MIXER_FADER_BASE + (int)i);
    if (!h) continue;
    RECT r;
    GetWindowRect(h, &r);
    MapWindowPoints(NULL, m_hWnd, (POINT*)&r, 2);
    if (client.y >= r.top && client.y < r.bottom) { hit = (int)i; break; }
  }

  const bool showHidden = MixerCfg().Get().showHiddenFaders;
  HMENU hMenu = CreatePopupMenu();

  // The Options tab's ordering list gets the same menu, on the row under the
  // pointer. It is the list that already has "Hide fader" as a button, so a
  // right-click doing the same thing is what someone will try -- and this is
  // the only view that shows hidden faders while they are hidden, which makes
  // it the natural place to bring one back.
  if (m_nActivePage == 2) {
    HWND hList = GetDlgItem(m_hWnd, IDC_MW_MIXER_ORDER_LIST);
    if (!hList) { DestroyMenu(hMenu); return 0; }
    LVHITTESTINFO ht = {};
    ht.pt = screen;
    ScreenToClient(hList, &ht.pt);
    const int row = (int)SendMessageW(hList, LVM_HITTEST, 0, (LPARAM)&ht);
    // Fall back to the selection when the click missed a row, so the menu is
    // still useful on the empty space below a short list.
    const int use = (row >= 0) ? row : LvSelection(hList);
    if (use >= 0 && (size_t)use < m_orderVisible.size()) {
      m_ctxFaderKey = m_orderKeys[(size_t)m_orderVisible[(size_t)use]];
      AppendMenuW(hMenu, MF_STRING, IDC_MW_MIXER_CTX_HIDE,
                  MixerCfg().IsHidden(m_ctxFaderKey) ? L"Show this channel"
                                                     : L"Hide this channel");
      AppendMenuW(hMenu, MF_SEPARATOR, 0, NULL);
    } else {
      m_ctxFaderKey.clear();
    }
    AppendMenuW(hMenu, MF_STRING | (showHidden ? MF_CHECKED : 0),
                IDC_MW_MIXER_CTX_SHOWHIDDEN, L"Show hidden channels");
    TrackPopupMenu(hMenu, TPM_LEFTALIGN | TPM_TOPALIGN, screen.x, screen.y,
                   0, m_hWnd, NULL);
    DestroyMenu(hMenu);
    return 0;
  }
  if (hit >= 0) {
    const std::wstring key = m_faders[(size_t)hit].first + L"|" +
                             m_faders[(size_t)hit].second;
    m_ctxFaderKey = key;
    // A row can only be ON screen while hidden when "show hidden" is on, so
    // the item says which way it will go rather than assuming Hide.
    AppendMenuW(hMenu, MF_STRING, IDC_MW_MIXER_CTX_HIDE,
                MixerCfg().IsHidden(key) ? L"Show this channel"
                                         : L"Hide this channel");
    AppendMenuW(hMenu, MF_SEPARATOR, 0, NULL);
  } else {
    m_ctxFaderKey.clear();
  }
  AppendMenuW(hMenu, MF_STRING | (showHidden ? MF_CHECKED : 0),
              IDC_MW_MIXER_CTX_SHOWHIDDEN, L"Show hidden channels");
  TrackPopupMenu(hMenu, TPM_LEFTALIGN | TPM_TOPALIGN, screen.x, screen.y,
                 0, m_hWnd, NULL);
  DestroyMenu(hMenu);
  return 0;
}

LRESULT MixerWindow::DoMessage(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam) {
  UNREFERENCED_PARAMETER(lParam);
  UNREFERENCED_PARAMETER(wParam);


  // WM_ACTIVATE is the fast path, not the authority. Measured: opening the
  // window while the machine is locked delivers WA_ACTIVE -- ToolWindow::Open
  // calls SetForegroundWindow -- but the matching WA_INACTIVE never arrives,
  // because the window never really held the foreground. Keyed on that message
  // alone the subscription stuck on, and the providers would have been polled
  // for a window nobody could even see.
  if (msg == WM_ACTIVATE) {
    SetActive(LooksActive());
    if (m_subscribed) RefreshValues();
    return 0;
  }

  // The timer runs for the life of the window, not just while subscribed, so
  // it can reconcile in BOTH directions. Once a second is the rate Shane
  // asked for, and a tick that finds nothing to do costs a foreground check.
  if (msg == WM_TIMER && wParam == kRelayoutTimer) {
    // The resize has settled: lay the rows out for the size it ended at.
    KillTimer(hWnd, kRelayoutTimer);
    RebuildFonts();
    return 0;
  }

  if (msg == WM_TIMER && wParam == 1) {
    SetActive(LooksActive());

    // Refreshing follows being ON SCREEN; subscribing follows being in front,
    // and LooksActive above says why they are not the same question (#328).
    //
    // Both used to hang off m_subscribed, so a mixer window in plain sight but
    // not focused -- which is how it is normally read -- updated nothing at
    // all. The battery column held whatever it had when focus last left. Shane
    // caught it against the HUD: 38% there, 68% in the list, the same headset,
    // because the HUD has a poll of its own and the list had none.
    //
    // Every tick in front, every fifth while merely visible, none while hidden
    // or minimised. Shane's figure: "it's relatively cheap to do every 5
    // seconds if the window is not hidden".
    const bool visible = m_hWnd && IsWindow(m_hWnd) &&
                         IsWindowVisible(m_hWnd) && !IsIconic(m_hWnd);
    bool refresh = false;
    if (m_subscribed) {
      m_idleTicks = 0;
      refresh = true;
    } else if (visible && ++m_idleTicks >= 5) {
      m_idleTicks = 0;
      refresh = true;
    }

    if (refresh) {
      RefreshValues();
      // Values are not the only thing that moves.
      //
      // The engine reorders the list on its own -- a failover device
      // connecting is lifted to the top by pinFailoverDevices -- and it does
      // so without any fader changing, so RefreshValues has nothing to notice
      // and the window kept showing the arrangement from before the device
      // arrived: "the failover works but the fader list is then stale ... I
      // had to hit the refresh button". A relayout costs a rebuild, so it is
      // done only when the token says the arrangement really did change.
      //
      // Never while the mouse is captured. A rebuild destroys and recreates
      // every control, and doing that under a held slider drops the grab
      // mid-drag. GetCapture is per THREAD and this window owns its own, so a
      // non-null answer means one of these controls is being dragged right
      // now. The relayout simply waits for the next tick.
      if (m_pEngine && GetCapture() == NULL &&
          m_pEngine->MixerOrderRev() != m_viewRev)
        RebuildFonts();
    }
    return 0;
  }

  return -1;
}

void MixerWindow::DoDestroy() {
  if (m_hMuteFont) {
    DeleteObject(m_hMuteFont);
    m_hMuteFont = NULL;
  }
  // Released here as well as on deactivate: a window closed while active would
  // otherwise leave the count raised for the life of the subsystem, and the
  // providers would poll forever with nobody watching.
  if (m_hWnd) KillTimer(m_hWnd, 1);
  SetActive(false);
}

}  // namespace mdrop
