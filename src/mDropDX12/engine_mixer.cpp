// engine_mixer.cpp — the only place the engine and the mixer module meet.
//
// Everything project-specific lives here: ConfigStore, the child-mode gate,
// the IPC verbs and the broadcast. audio_mixer.* and the providers stay free
// of engine types, so hosting them elsewhere later is a change of owner rather
// than a rewrite.
#include "tcp_server.h"  // Must precede engine.h — winsock2.h before windows.h
#include "engine.h"
#include "order_group.h"
#include "config_store.h"
#include "utility.h"
#include "audio_mixer.h"
#include "mixer_device_watch.h"
#include "mixer_provider_endpoint.h"
#include "mixer_provider_sonar.h"
#include "mixer_failover.h"
#include "mixer_settings.h"
#include "mixer_sonar_restart.h"
#include "profile_paths.h"
#include "json_utils.h"
#include "format_to.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <algorithm>
#include <map>
#include <memory>
#include <atomic>
#include <mutex>
#include <set>
#include <thread>
#include <vector>

extern TcpServer g_tcpServer;

namespace mdrop {

namespace {

std::unique_ptr<DeviceWatcher> g_watcher;
std::unique_ptr<AudioMixer> g_mixer;
bool g_enabled = false;
int  g_subscribers = 0;
// The last arrangement token pushed to subscribers, so a change is announced
// once rather than on every provider tick. Cleared when the mixer starts: a
// client connecting has no idea what a previous session broadcast.
std::wstring g_lastViewRev;

FailoverWatcher g_failover;

// ── The battery figure the HUD draws ───────────────────────────────
//
// Which device counts as "the headset the user is wearing" is not the
// question it looks like. The Windows default render endpoint is the obvious
// answer and the wrong one: with SteelSeries Sonar running it IS Sonar, a
// virtual endpoint with no PnP container behind it and therefore no battery,
// no container id and no physical identity of any kind. Asking it returns a
// blank on the one machine that matters.
//
// The physical headset is where the mixer ROUTES point, and the Failover tab
// already shows exactly that -- "monitoring -> Headphones (WF-1000XM5-3),
// Now on: XM5 Black #2, 100%". So a route's current device is asked first,
// armed routes before unarmed ones, and the default render endpoint only as a
// fallback for a machine with no middleware and no routes configured.
std::atomic<int> g_hudBattery{-1};

// The endpoint ids to ask, best first. Rebuilt on the change callback, which
// is the only moment a route can move -- so the poll below walks two or three
// strings rather than copying a snapshot of every channel and fader.
std::mutex g_hudBatteryMutex;
std::vector<std::wstring> g_hudBatteryIds;

// Which device the last recompute took its answer from, and how it was
// reached. Diagnostic only -- but "why is it showing that number" is the
// question this feature will actually be asked, and a percentage on its own
// cannot answer it. Guarded by the mutex above.
std::wstring g_hudBatteryDevice;
const wchar_t* g_hudBatterySource = L"none";

// GetTickCount of the last read. Zero means "due now", which is how switching
// the line on jumps the queue instead of waiting out the interval.
std::atomic<DWORD> g_hudBatteryLast{0};

// A forced percentage, for looking at what the HUD does at a given charge.
//
// The line is drawn in three colours by threshold and only one of them can
// be seen at a time on a real headset -- so without this, two of the three
// are only ever verified by reading the source. Same reasoning as
// g_simPresence above, which makes a connected device look gone.
//
// -2 is off. In memory only, never persisted, and cleared by
// DIAG_BATTERY_SIM=off.
std::atomic<int> g_hudBatterySim{-2};

// ── Confirmation policy ───────────────────────────────────────────────────
//
// A bound hotkey fires immediately: the binding is the confirmation, and a
// dialog is least usable exactly when the action is most needed. The guard
// exists for anyone who wants it and is off by default.
struct ConfirmState {
  bool pending = false;
  std::wstring what;
  unsigned askedAt = 0;
  std::wstring lastOutcome = L"none";   // none | confirmed | dropped
};
ConfirmState g_confirm;

int ConfirmSecondsFor(int surface) {
  const MixerSettings& s = MixerCfg().Get();
  switch (surface) {
    case 0: return s.confirmHotkeySeconds;
    case 1: return s.confirmToolWindow ? 1 : 0;
    default: return s.confirmRemote ? 1 : 0;
  }
}

// The remote guard may only be relaxed once a PIN exists. forgejo#44 made a
// configured PIN actually mean something; before that this gate would have
// advertised a protection that was not there.
bool RemoteUnlockable() {
  wchar_t stored[128] = {};
  Config().GetStringTo(L"Network", L"PinHash", L"", stored, 128);
  return stored[0] != L'\0';
}

// Testing-mode only. Lets a test say a device came or went without touching
// hardware, which is the only way to exercise disconnect, flap and re-pair.
std::map<std::wstring, bool> g_simPresence;

// Route rules live in mixer.json, which can hold the id as it really is and
// the allowlist as an ordered array -- rather than sixteen padded INI keys
// with the colon replaced by an underscore.
void SaveRule(const RouteRule& rule) {
  MixerRouteCfg& cfg = MixerCfg().Route(rule.routeId);
  cfg.armed = rule.armed;
  cfg.allow.clear();
  for (const AllowEntry& e : rule.allow) {
    MixerAllowEntry out;
    out.id = e.id;
    out.name = e.name;
    cfg.allow.push_back(out);
  }
  MixerCfg().Save();
}

RouteRule LoadRule(const std::wstring& routeId) {
  RouteRule rule;
  rule.routeId = routeId;
  const MixerRouteCfg* cfg = MixerCfg().FindRoute(routeId);
  if (!cfg) return rule;
  rule.armed = cfg->armed;
  for (const MixerAllowEntry& e : cfg->allow) {
    AllowEntry out;
    out.id = e.id;
    out.name = e.name;
    rule.allow.push_back(out);
  }
  return rule;
}

// The endpoint ids the HUD battery may come from, best first.
//
// Armed routes before unarmed ones: an armed route is one the user has
// deliberately configured, so it is the better answer to "what am I
// listening on" when a machine has several. Empty device ids are dropped
// rather than kept as blanks, so the reader needs no second test.
void RebuildHudBatteryIds(const MixerSnapshot& snap) {
  std::vector<std::wstring> ids;
  for (int pass = 0; pass < 2; pass++)
    for (const RouteTarget& r : snap.routes) {
      if (r.currentDeviceId.empty()) continue;
      if (LoadRule(r.id).armed != (pass == 0)) continue;
      ids.push_back(r.currentDeviceId);
    }
  std::lock_guard<std::mutex> lock(g_hudBatteryMutex);
  g_hudBatteryIds.swap(ids);
}

// A FILETIME as local "YYYY-MM-DD HH:MM", or "-" when it is not known.
// Sortable as a string, which is what a reader wants from it.
std::wstring FormatLastSeen(unsigned long long filetime) {
  if (!filetime) return L"-";
  FILETIME ft;
  ft.dwLowDateTime = (DWORD)(filetime & 0xFFFFFFFFull);
  ft.dwHighDateTime = (DWORD)(filetime >> 32);
  FILETIME local = {};
  SYSTEMTIME st = {};
  if (!FileTimeToLocalFileTime(&ft, &local) ||
      !FileTimeToSystemTime(&local, &st))
    return L"-";
  wchar_t buf[32];
  FormatTo(buf, L"%04d-%02d-%02d %02d:%02d", st.wYear, st.wMonth, st.wDay,
           st.wHour, st.wMinute);
  return buf;
}

std::wstring Fmt(const wchar_t* fmt, ...) {
  wchar_t buf[1024];
  va_list args;
  va_start(args, fmt);
  _vsnwprintf_s(buf, _countof(buf), _TRUNCATE, fmt, args);
  va_end(args);
  return buf;
}

const wchar_t* HealthText(ProviderHealth h) {
  switch (h) {
    case ProviderHealth::Ok: return L"ok";
    case ProviderHealth::Degraded: return L"degraded";
    default: return L"unavailable";
  }
}

// Splits "a|b|c" after the '=' of a verb. Channel and route ids contain ':'
// but never '|', which is what makes this unambiguous.
std::vector<std::wstring> SplitArgs(const std::wstring& s) {
  std::vector<std::wstring> out;
  size_t start = 0;
  for (size_t i = 0; i <= s.size(); i++) {
    if (i == s.size() || s[i] == L'|') {
      out.push_back(s.substr(start, i - start));
      start = i + 1;
    }
  }
  return out;
}

std::wstring FaderLine(const Channel& c, const Fader& f) {
  // Battery, read from the WATCHER rather than carried on the Channel.
  //
  // It is the one field in the snapshot that changes with no device event, so
  // MIXER_BATTERY re-reads it without re-polling any provider -- which means a
  // figure cached on the Channel would be the one from the last provider poll
  // and would not move when that verb ran. Reading it here is always current
  // and costs one locked scan.
  //
  // -1 is ORDINARY: a provider's own channel has no device behind it, plenty
  // of devices report nothing, and an inactive endpoint is deliberately -1
  // rather than the reading it had on the way out.
  int battery = -1;
  {
    const std::wstring prefix = L"endpoint:";
    if (g_watcher && c.id.compare(0, prefix.size(), prefix) == 0)
      battery = g_watcher->BatteryFor(c.id.substr(prefix.size()));
  }
  // `virtual` says this fader's channel is a software endpoint another program
  // may own -- Sonar's eight, which are named like its channels, sit at 1.000
  // and ignore a write. Reported so every surface can make the same decision;
  // the window hides them by default and a remote should too (forgejo#51).
  // `canMute=0` says this fader has a working volume but must not be muted --
  // the Sonar master, whose mute writes every render channel and loses their
  // states coming back. Reported so every surface can make the same decision
  // instead of special-casing a channel id: the window hides the checkbox and
  // a remote should hide its button, rather than offering a control that does
  // damage. Named rather than inferred, because a client cannot discover this
  // by trying it -- the write succeeds.
  // `chwindowsName` is present only when the user has given this device a
  // short name, and then it carries the name Windows uses. A reader therefore
  // gets both without joining back to MIXER_DEVICE -- the short one for a
  // narrow row, the full one for a tooltip -- and its ABSENCE is itself the
  // answer to "has this been renamed", which is what decides whether a name
  // may be trimmed for display. Appended last so nothing positional moves.
  //
  // `groups=` lists the 1-based volume-hotkey groups this fader belongs to,
  // comma-separated, empty if none (forgejo#77). MIXER_GROUPS already reports
  // each group's member COUNT and its raw "channel|fader" keys, but nothing
  // told a remote which fader on ITS OWN screen those keys resolve to, so it
  // could not mark the faders the volume hotkeys will actually move.
  std::wstring groups;
  {
    const std::vector<MixerGroupCfg>& g = MixerCfg().Get().groups;
    for (size_t gi = 0; gi < g.size(); gi++) {
      for (const std::wstring& key : g[gi].members) {
        const std::vector<std::wstring> t = SplitArgs(key);
        if (t.size() >= 2 && t[0] == c.id && t[1] == f.id) {
          if (!groups.empty()) groups += L",";
          groups += std::to_wstring(gi + 1);
          break;
        }
      }
    }
  }
  // `hidden=` and `short=` are the user's per-fader preferences (forgejo#50,
  // forgejo#52). Both are ALWAYS present -- hidden as 0 or 1, short possibly
  // empty -- so a client reads a value rather than inferring one from a
  // missing field.
  //
  // `chname` and `label` keep reporting the originals either way. A short name
  // is an alias, not a replacement, and the originals are what let a client
  // show "Aux - Monitoring" in a tooltip when the short name is cryptic.
  //
  // Both go BEFORE nothing that ends in a key: every record carrying a
  // "<channel>|<fader>" key must keep it last, because a reader takes
  // everything after "|key=" as the key. This record has no key= field, so the
  // two are simply appended.
  const std::wstring prefKey = c.id + L"|" + f.id;
  const bool hidden = MixerCfg().IsHidden(prefKey);
  const std::wstring shortName = MixerCfg().ShortNameFor(prefKey);

  return Fmt(L"MIXER_FADER|ch=%s|chname=%s|provider=%s|id=%s|label=%s"
             L"|vol=%.3f|mute=%d|health=%s|virtual=%d|canMute=%d"
             L"|chwindowsName=%s|battery=%d|groups=%s|hidden=%d|short=%s",
             c.id.c_str(), c.displayName.c_str(), c.providerId.c_str(),
             f.id.c_str(), f.label.c_str(), f.volume, f.muted ? 1 : 0,
             HealthText(c.health), c.isVirtual ? 1 : 0, f.canMute ? 1 : 0,
             c.windowsName.c_str(), battery, groups.c_str(),
             hidden ? 1 : 0, shortName.c_str());
}

// The record for one fader as it stands in the snapshot, or an empty string if
// that fader is not there.
std::wstring FaderLineFor(const std::wstring& channelId,
                          const std::wstring& faderId) {
  if (!g_mixer) return std::wstring();
  const MixerSnapshot snap = g_mixer->Snapshot();
  for (const Channel& c : snap.channels) {
    if (c.id != channelId) continue;
    for (const Fader& f : c.faders)
      if (f.id == faderId) return FaderLine(c, f);
  }
  return std::wstring();
}

bool FaderExists(const std::wstring& channelId, const std::wstring& faderId) {
  return !FaderLineFor(channelId, faderId).empty();
}

// Whether this fader may be muted at all. Absent faders answer true so the
// caller's own unknown_fader check stays the one that reports them.
bool FaderCanMute(const std::wstring& channelId, const std::wstring& faderId) {
  if (!g_mixer) return true;
  const MixerSnapshot snap = g_mixer->Snapshot();
  for (const Channel& c : snap.channels) {
    if (c.id != channelId) continue;
    for (const Fader& f : c.faders)
      if (f.id == faderId) return f.canMute;
  }
  return true;
}

// ── Hotkey groups ───────────────────────────────────────────────
//
// One key moves every fader ticked into a group. This replaced two named
// slots, "personal" and "streaming", that each pointed at a single fader --
// an implementation invention rather than a request, and one Shane summed up
// as "they never did anything for me". A group of one member is the old slot,
// so nothing they could do is lost.
//
// Groups are addressed by INDEX over IPC, one-based on the wire because the
// hotkey actions and the window both say "Group 1". Parsing is deliberately
// strict -- an unparseable index is refused rather than silently meaning the
// first group, which would move faders the sender did not name.
int GroupIndex(const std::wstring& text) {
  if (text.empty()) return -1;
  wchar_t* end = nullptr;
  const long n = wcstol(text.c_str(), &end, 10);
  if (end == text.c_str() || (end && *end)) return -1;
  if (n < 1) return -1;
  return (int)(n - 1);
}

}  // namespace

void Engine::MixerInit() {
  // A child instance is a headless renderer and owns no external interfaces.
  if (m_bChildMode) return;
  // Settings first: whether the subsystem runs at all is one of them.
  MixerCfg().SetResourceDir(m_szMilkdrop2Path);
  MixerCfg().Load();
  if (!MixerCfg().Get().enabled) return;

  // On a thread of its own, with its own COM apartment -- see forgejo#46.
  //
  // MixerInit is called from WinMain before anything on that thread has called
  // CoInitialize, so CoCreateInstance(MMDeviceEnumerator) failed and the whole
  // enable was abandoned. Every time, since the mixer was written. It went
  // unseen because tests always send MIXER_ENABLE=1 afterwards and that arrives
  // on the IPC thread, where COM is up.
  //
  // The cost was not "the window opens empty". The failover watcher only ticks
  // while the mixer runs, so an ARMED ROUTE WAS DOING NOTHING -- and worse, a
  // mixer that is off publishes no routes, so the Failover tab had nothing to
  // select and showed an empty allowlist. Shane read that as the app having
  // forgotten his devices: "i filled out the list everything looked great
  // exited the app and nothing in the list". The settings were on disk the
  // whole time.
  //
  // Initialising the WINMAIN thread's apartment was tried instead and reverted:
  // it fixes the enable and the app dies during startup. This way that thread
  // is left exactly as it was.
  //
  // MULTITHREADED, not APARTMENTTHREADED: an MTA object is callable from any
  // thread, and the watcher is used from the IPC thread, the worker and the
  // window. An STA would need marshalling that nothing here does.
  std::thread([this]() {
    const HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    MixerSetEnabled(true);
    // The apartment is deliberately NOT torn down: the providers' COM objects
    // outlive this thread and belong to it.
    (void)hr;
  }).detach();
}

void Engine::MixerShutdown() {
  if (g_mixer) {
    g_mixer->Stop();
    g_mixer.reset();
  }
  if (g_watcher) {
    g_watcher->Stop();
    g_watcher.reset();
  }
  g_subscribers = 0;
  g_enabled = false;
  // No watcher, no figure. Left behind, the HUD would keep drawing the last
  // charge read before the mixer was switched off.
  g_hudBattery.store(-1, std::memory_order_relaxed);
  {
    std::lock_guard<std::mutex> lock(g_hudBatteryMutex);
    g_hudBatteryIds.clear();
  }
  g_hudBatteryLast.store(0);
}

bool Engine::MixerIsEnabled() const { return g_enabled && g_mixer != nullptr; }

bool Engine::MixerSetEnabled(bool enabled) {
  if (m_bChildMode) return false;

  if (!enabled) {
    MixerShutdown();
    MixerCfg().Get().enabled = false;
    MixerCfg().Save();
    return true;
  }
  if (g_mixer) {
    g_enabled = true;
    return true;
  }

  g_watcher.reset(new DeviceWatcher());
  // A device arriving or leaving wakes the worker, which re-hooks the
  // providers and rebuilds the snapshot. Nothing polls the device list.
  // Our names for devices, applied as the watcher reads each endpoint.
  g_watcher->SetNameHook([](const std::wstring& id,
                            const std::wstring& windowsName) {
    return MixerCfg().AliasFor(id, windowsName);
  });
  if (!g_watcher->Start([]() { if (g_mixer) g_mixer->RefreshNow(); })) {
    DebugLogW(L"Audio mixer: device watcher failed to start", LOG_ERROR);
    g_watcher.reset();
    return false;
  }

  g_mixer.reset(new AudioMixer());
  g_mixer->AddProvider(std::unique_ptr<IMixerProvider>(
      new EndpointProvider(g_watcher.get())));
  // Second, and optional. Sonar can be absent, and its channels are the only
  // way to reach a Sonar mix level -- the virtual endpoints ignore Windows
  // volume writes entirely. The two id namespaces are disjoint, so Apply()
  // routes by whichever provider claims the id.
  if (MixerCfg().Get().sonarEnabled)
    g_mixer->AddProvider(std::unique_ptr<IMixerProvider>(new SonarProvider()));

  g_failover.SetClock([]() { return (unsigned)GetTickCount(); });
  g_failover.SetPresence([](const std::wstring& id) {
    // A simulated answer wins, so a test can make a present device look gone.
    const auto sim = g_simPresence.find(id);
    if (sim != g_simPresence.end()) return sim->second;
    return g_watcher && g_watcher->IsActive(id);
  });
  g_failover.SetNames([](const std::wstring& name) -> std::wstring {
    // NAME -> the id currently carrying it: the Bluetooth re-pair fallback.
    //
    // Either name counts. An allowlist entry stores the name that was current
    // when it was added, so giving that device a short name afterwards would
    // otherwise strand the entry -- it would hold a name nothing answers to.
    if (!g_watcher) return std::wstring();
    for (const EndpointInfo& e : g_watcher->Endpoints())
      if (e.friendlyName == name || e.windowsName == name) return e.id;
    return std::wstring();
  });
  g_failover.SetStabilitySeconds(MixerCfg().Get().stabilitySeconds);
  g_failover.SetMinDwellSeconds(MixerCfg().Get().minDwellSeconds);
  g_failover.SetOnCommit([this](const std::wstring& routeId,
                                const std::wstring& deviceId) {
    if (g_mixer) g_mixer->SetRouteDevice(routeId, deviceId);
    AddNotification(L"Audio output moved");
    DebugLogW(L"Failover moved a route to another device", LOG_INFO);
  });

  // The heartbeat the watcher runs on. Arming and committing are separate
  // ticks, so without this an armed route would wait for a device event that
  // may never arrive.
  g_mixer->SetOnTick([this]() {
    g_failover.Tick();
    // The HUD battery, on a poll of its own. Does nothing unless the line is
    // switched on, and no oftener than every thirty seconds.
    MixerBatteryTick();
    // The confirm window expires here too. Without a heartbeat it would stay
    // pending until the next command, and a stale window would then accept an
    // answer long after the moment had passed.
    MixerConfirmTick();
  });

  g_lastViewRev.clear();
  g_mixer->SetOnChanged([this]() {
    // Worker thread. Nothing is sent when nobody is watching -- the same rule
    // that gates polling gates the push, so an enabled-but-unwatched mixer is
    // silent on the wire as well as idle.
    if (!g_mixer) return;
    const MixerSnapshot snap = g_mixer->Snapshot();
    for (const RouteTarget& r : snap.routes)
      g_failover.NoteRouteDevice(r.id, r.currentDeviceId);
    // Where the HUD battery may be read from. Here rather than in the poll:
    // this is the moment a route moves, and the poll runs far more often
    // than the routes change.
    RebuildHudBatteryIds(snap);
    if (g_subscribers <= 0) return;
    for (const Channel& c : snap.channels)
      for (const Fader& f : c.faders)
        g_tcpServer.Broadcast(FaderLine(c, f));
    // A device arriving or leaving is where the list reorders on its own: a
    // failover target connecting is pinned to the top by the app, and the
    // remote was left showing the arrangement from before it plugged in --
    // "the failover works but the fader list is then stale". The faders above
    // say nothing about that, because none of their values changed.
    MixerNotifyViewChanged();
  });
  if (!g_mixer->Start()) {
    DebugLogW(L"Audio mixer: worker failed to start", LOG_ERROR);
    g_mixer.reset();
    g_watcher->Stop();
    g_watcher.reset();
    return false;
  }

  g_enabled = true;
  MixerCfg().Get().enabled = true;
  MixerCfg().Save();

  // Bounded wait for the first snapshot. Enabling is a rare, user-initiated
  // act, not a hot path, and without this the very next command sees an empty
  // mixer and reasonably concludes the machine has no audio devices.
  g_mixer->WaitForIdle(2000);

  // AFTER the snapshot, not before. The rules are keyed by route id and the
  // routes come from the providers, so loading them first walked an empty list
  // and armed nothing -- a route the user had armed came back DISARMED on the
  // next start, which is the one moment it is supposed to be watching. It went
  // unnoticed because arming from the window or over IPC sets the rule
  // directly, so it only bit across a restart.
  MixerLoadFailoverConfig();

  DebugLogW(L"Audio mixer enabled", LOG_INFO);
  return true;
}

void Engine::MixerSetSubscribed(bool subscribed) {
  if (subscribed) g_subscribers++;
  else if (g_subscribers > 0) g_subscribers--;
  if (g_mixer) g_mixer->SetSubscribed(g_subscribers > 0);
}

std::vector<std::wstring> Engine::MixerStateLines() {
  std::vector<std::wstring> lines;
  lines.push_back(L"MIXER_BEGIN");

  if (!MixerIsEnabled()) {
    // Say why there is nothing, rather than returning an empty list that reads
    // like "this machine has no audio devices".
    lines.push_back(L"MIXER_HEALTH|provider=endpoint|health=unavailable");
    lines.push_back(L"MIXER_END");
    return lines;
  }

  const MixerSnapshot snap = g_mixer->Snapshot();
  for (const Channel& c : snap.channels)
    for (const Fader& f : c.faders)
      lines.push_back(FaderLine(c, f));

  for (const RouteTarget& r : snap.routes) {
    // The armed flag and the state were hardcoded here, so every reader --
    // the window, the harness, the phone -- was told "armed=0 state=idle"
    // however the failover was actually configured or whatever it was in the
    // middle of doing. Report what the watcher really holds.
    const FailoverState st = g_failover.StateOf(r.id);
    const wchar_t* stateName = st == FailoverState::Arming    ? L"arming"
                             : st == FailoverState::Searching ? L"searching"
                                                              : L"idle";
    const std::wstring reason = g_failover.ReasonOf(r.id);
    lines.push_back(Fmt(L"MIXER_ROUTE|id=%s|name=%s|device=%s|armed=%d"
                        L"|state=%s|reason=%s",
                        r.id.c_str(), r.displayName.c_str(),
                        r.currentDeviceId.c_str(),
                        LoadRule(r.id).armed ? 1 : 0, stateName,
                        reason.empty() ? L"-" : reason.c_str()));
  }

  if (g_watcher) {
    for (const EndpointInfo& e : g_watcher->Endpoints())
      // windowsName as well as name, because they differ once a device has
      // been given a short name -- name carries the alias. An allowlist entry
      // added while its device was switched off has no id and matches by name
      // alone, and the name it stored is the WINDOWS one. Without this field
      // the window's matching silently failed for exactly those entries and
      // drew a connected device as disconnected.
      lines.push_back(Fmt(L"MIXER_DEVICE|id=%s|name=%s|windowsName=%s|flow=%s"
                          L"|active=%d|display=%d|handsfree=%d|seen=%s|battery=%d",
                          e.id.c_str(), e.friendlyName.c_str(),
                          e.windowsName.c_str(),
                          e.isRender ? L"render" : L"capture",
                          e.isActive ? 1 : 0, e.isDisplayAudio ? 1 : 0,
                          e.isHandsFree ? 1 : 0,
                          FormatLastSeen(e.lastSeen).c_str(),
                          e.batteryPercent));
  }

  lines.push_back(L"MIXER_END");
  return lines;
}

// ── Mixer profiles ──────────────────────────────────────────────
//
// resources/profiles/mixer/<name>.json, one file per profile -- see
// profile_paths.h for why per-file rather than one document holding many.
//
// A profile carries the MIX and the LAYOUT together, which is what was asked
// for: recalling "Gaming" should both set the levels and put the list back the
// way it is wanted, rather than making that two actions. The wider mixer
// configuration -- failover rules, device aliases, confirmation -- is written
// only when explicitly asked for, so the common file stays small and readable.
// The cautionary example is settings.ini, which grew section by section until
// it could no longer be read.
//
// Levels cover Sonar channels AND connected Windows endpoints. A device that is
// absent when the profile is recalled is SKIPPED and counted, never an error:
// unplugging headphones must not make a profile fail to load.

std::wstring Engine::MixerProfileDir() const {
  return profiles::DirFor(m_szBaseDir, profiles::Kind::Mixer);
}

std::vector<std::wstring> Engine::ListMixerProfiles() const {
  return profiles::ListDir(MixerProfileDir());
}

// The display name inside a profile file, falling back to its leaf. The two
// differ whenever a typed name needed sanitising to become a filename.
std::wstring Engine::MixerProfileName(const std::wstring& leaf) const {
  const std::wstring path =
      profiles::PathFor(m_szBaseDir, profiles::Kind::Mixer, leaf);
  try {
    const JsonValue root = JsonLoadFile(path.c_str());
    const std::wstring name = root[L"name"].asString();
    if (!name.empty()) return name;
  } catch (...) {}
  return leaf;
}

std::wstring Engine::SaveMixerProfile(const std::wstring& displayName,
                                      bool allSettings) {
  if (!profiles::EnsureDirPath(MixerProfileDir())) return std::wstring();

  // An empty name means "just bank it": a timestamp, which sorts by age
  // because the format is yyyy-MM-dd_HH-mm-ss.
  const std::wstring shown =
      displayName.empty() ? profiles::TimestampNow() : displayName;
  const std::wstring leaf = profiles::UniqueLeafIn(MixerProfileDir(), shown);
  const std::wstring path = MixerProfileDir() + profiles::WithJson(leaf);

  const MixerSettings& cfg = MixerCfg().Get();

  // ── the mix ──
  JsonValue levels;
  levels.type = JsonValue::Array;
  if (g_mixer) {
    const MixerSnapshot snap = g_mixer->Snapshot();
    for (const Channel& c : snap.channels) {
      for (const Fader& f : c.faders) {
        JsonValue e;
        e.type = JsonValue::Object;
        e.members.push_back({ L"ch",    JsonValue(c.id) });
        e.members.push_back({ L"id",    JsonValue(f.id) });
        e.members.push_back({ L"vol",   JsonValue((double)f.volume) });
        e.members.push_back({ L"mute",  JsonValue(f.muted) });
        // Kept so a listing can say what a profile refers to without the
        // device having to be present to ask.
        e.members.push_back({ L"label", JsonValue(c.displayName) });
        levels.elements.push_back(e);
      }
    }
  }

  JsonValue order;
  order.type = JsonValue::Array;
  for (const std::wstring& key : cfg.order)
    order.elements.push_back(JsonValue(key));

  JsonWriter w;
  w.BeginObject();
  w.Int(L"version", 1);
  w.String(L"kind", L"mixer");
  w.String(L"name", shown);
  w.Bool(L"allSettings", allSettings);
  w.Value(L"levels", levels);

  // The hotkey groups travel with the arrangement, not with the levels: which
  // faders one key moves is part of how the mixer is laid out for a job.
  JsonValue groups;
  groups.type = JsonValue::Array;
  for (const MixerGroupCfg& g : cfg.groups) {
    JsonValue members;
    members.type = JsonValue::Array;
    for (const std::wstring& key : g.members)
      members.elements.push_back(JsonValue(key));
    JsonValue entry;
    entry.type = JsonValue::Object;
    entry.members.push_back({ L"name", JsonValue(g.name) });
    entry.members.push_back({ L"members", members });
    groups.elements.push_back(entry);
  }

  w.BeginObject(L"layout");
  w.Value(L"order", order);
  w.Bool(L"sortUnmutedFirst", cfg.sortUnmutedFirst);
  w.Bool(L"pinFailoverDevices", cfg.pinFailoverDevices);
  w.Bool(L"showVirtualEndpoints", cfg.showVirtualEndpoints);
  w.Bool(L"spinBoxes", cfg.spinBoxes);
  w.Value(L"groups", groups);
  w.EndObject();

  if (allSettings) {
    // Everything else mixer.json holds. Written only on request, so the common
    // profile stays small and readable -- the failure mode being avoided is
    // settings.ini, which grew until it could no longer be read.
    JsonValue routes;
    routes.type = JsonValue::Object;
    for (const auto& r : cfg.routes) {
      JsonValue allow;
      allow.type = JsonValue::Array;
      for (const MixerAllowEntry& a : r.second.allow) {
        JsonValue e;
        e.type = JsonValue::Object;
        e.members.push_back({ L"id",   JsonValue(a.id) });
        e.members.push_back({ L"name", JsonValue(a.name) });
        allow.elements.push_back(e);
      }
      JsonValue rule;
      rule.type = JsonValue::Object;
      rule.members.push_back({ L"armed", JsonValue(r.second.armed) });
      rule.members.push_back({ L"allow", allow });
      routes.members.push_back({ r.first, rule });
    }

    JsonValue names;
    names.type = JsonValue::Array;
    for (const MixerDeviceName& n : cfg.deviceNames) {
      JsonValue e;
      e.type = JsonValue::Object;
      e.members.push_back({ L"id",          JsonValue(n.id) });
      e.members.push_back({ L"windowsName", JsonValue(n.windowsName) });
      e.members.push_back({ L"alias",       JsonValue(n.alias) });
      names.elements.push_back(e);
    }

    w.BeginObject(L"settings");
    w.Bool(L"enabled", cfg.enabled);
    w.Bool(L"sonarEnabled", cfg.sonarEnabled);
    w.Int(L"volumeStepPercent", cfg.volumeStepPercent);
    w.BeginObject(L"confirm");
    w.Int(L"hotkeySeconds", cfg.confirmHotkeySeconds);
    w.Bool(L"toolWindow", cfg.confirmToolWindow);
    w.Bool(L"remote", cfg.confirmRemote);
    w.EndObject();
    w.Int(L"stabilitySeconds", cfg.stabilitySeconds);
    w.Int(L"minDwellSeconds", cfg.minDwellSeconds);
    w.Value(L"routes", routes);
    w.Value(L"deviceNames", names);
    w.EndObject();
  }

  w.EndObject();
  if (!w.SaveToFile(path.c_str())) return std::wstring();
  DLOG_INFO("mixer profile saved: %ls (allSettings=%d)", path.c_str(),
            allSettings ? 1 : 0);
  return leaf;
}

bool Engine::LoadMixerProfile(const std::wstring& nameOrPath,
                              int* pApplied, int* pSkipped) {
  if (pApplied) *pApplied = 0;
  if (pSkipped) *pSkipped = 0;
  const std::wstring path =
      profiles::PathFor(m_szBaseDir, profiles::Kind::Mixer, nameOrPath);
  if (path.empty()) return false;

  try {
    // JsonParse throws on malformed input and this build is /EHa, so the whole
    // read sits in the try -- a hand-edited profile must not take the app down.
    const JsonValue root = JsonLoadFile(path.c_str());
    if (root.isNull()) return false;
    MixerSettings& cfg = MixerCfg().Get();

    // ── the layout ──
    const JsonValue& layout = root[L"layout"];
    if (!layout.isNull()) {
      const JsonValue& order = layout[L"order"];
      if (order.isArray()) {
        cfg.order.clear();
        for (size_t i = 0; i < order.size(); i++) {
          const std::wstring key = order.at(i).asString();
          if (!key.empty()) cfg.order.push_back(key);
        }
      }
      cfg.sortUnmutedFirst     = layout[L"sortUnmutedFirst"].asBool(cfg.sortUnmutedFirst);
      cfg.pinFailoverDevices   = layout[L"pinFailoverDevices"].asBool(cfg.pinFailoverDevices);
      cfg.showVirtualEndpoints = layout[L"showVirtualEndpoints"].asBool(cfg.showVirtualEndpoints);
      cfg.spinBoxes            = layout[L"spinBoxes"].asBool(cfg.spinBoxes);
      // A profile written before groups existed carries "slots" instead, which
      // nothing reads any more. Its absence leaves the configured groups
      // alone rather than clearing them -- loading an old arrangement must
      // not silently empty a key the user has bound.
      const JsonValue& groups = layout[L"groups"];
      if (groups.size() > 0) {
        cfg.groups.clear();
        for (size_t i = 0; i < groups.size(); i++) {
          const JsonValue& g = groups.at(i);
          MixerGroupCfg entry;
          entry.name = g[L"name"].asString(L"");
          const JsonValue& members = g[L"members"];
          for (size_t j = 0; j < members.size(); j++) {
            const std::wstring key = members.at(j).asString();
            if (!key.empty()) entry.members.push_back(key);
          }
          if (entry.name.empty()) entry.name = L"Group " + std::to_wstring(i + 1);
          cfg.groups.push_back(entry);
        }
      }
    }

    // ── everything else, when the profile carries it ──
    const JsonValue& st = root[L"settings"];
    if (!st.isNull()) {
      cfg.enabled           = st[L"enabled"].asBool(cfg.enabled);
      cfg.sonarEnabled      = st[L"sonarEnabled"].asBool(cfg.sonarEnabled);
      cfg.volumeStepPercent = st[L"volumeStepPercent"].asInt(cfg.volumeStepPercent);
      cfg.stabilitySeconds  = st[L"stabilitySeconds"].asInt(cfg.stabilitySeconds);
      cfg.minDwellSeconds   = st[L"minDwellSeconds"].asInt(cfg.minDwellSeconds);
      const JsonValue& cf = st[L"confirm"];
      if (!cf.isNull()) {
        cfg.confirmHotkeySeconds = cf[L"hotkeySeconds"].asInt(cfg.confirmHotkeySeconds);
        cfg.confirmToolWindow    = cf[L"toolWindow"].asBool(cfg.confirmToolWindow);
        cfg.confirmRemote        = cf[L"remote"].asBool(cfg.confirmRemote);
      }
      const JsonValue& routes = st[L"routes"];
      if (routes.isObject()) {
        cfg.routes.clear();
        for (const auto& m : routes.members) {
          MixerRouteCfg rule;
          rule.armed = m.second[L"armed"].asBool(false);
          const JsonValue& allow = m.second[L"allow"];
          for (size_t i = 0; i < allow.size(); i++) {
            MixerAllowEntry e;
            e.id   = allow.at(i)[L"id"].asString();
            e.name = allow.at(i)[L"name"].asString();
            if (!e.id.empty() || !e.name.empty()) rule.allow.push_back(e);
          }
          cfg.routes.push_back({ m.first, rule });
        }
      }
      const JsonValue& names = st[L"deviceNames"];
      if (names.isArray()) {
        cfg.deviceNames.clear();
        for (size_t i = 0; i < names.size(); i++) {
          MixerDeviceName n;
          n.id          = names.at(i)[L"id"].asString();
          n.windowsName = names.at(i)[L"windowsName"].asString();
          n.alias       = names.at(i)[L"alias"].asString();
          if (!n.id.empty() || !n.windowsName.empty()) cfg.deviceNames.push_back(n);
        }
      }
    }
    MixerCfg().Save();

    // ── the mix, last: the layout must not depend on a device being here ──
    const JsonValue& levels = root[L"levels"];
    for (size_t i = 0; i < levels.size(); i++) {
      const JsonValue& e = levels.at(i);
      const std::wstring ch = e[L"ch"].asString();
      const std::wstring id = e[L"id"].asString();
      if (ch.empty() || id.empty()) continue;
      if (!FaderExists(ch, id)) {          // absent device: skipped, not failed
        if (pSkipped) (*pSkipped)++;
        continue;
      }
      if (g_mixer) {
        g_mixer->SetVolume(ch, id, e[L"vol"].asFloat(1.0f));
        // A fader that refuses mute is left alone rather than asked -- see
        // canMute in mixer_provider_sonar.cpp.
        if (FaderCanMute(ch, id))
          g_mixer->SetMute(ch, id, e[L"mute"].asBool(false));
      }
      if (pApplied) (*pApplied)++;
    }
    DLOG_INFO("mixer profile loaded: %ls (%d applied, %d skipped)", path.c_str(),
              pApplied ? *pApplied : -1, pSkipped ? *pSkipped : -1);
    return true;
  } catch (...) {
    DLOG_ERROR("mixer profile %ls is malformed", path.c_str());
    return false;
  }
}

bool Engine::DeleteMixerProfile(const std::wstring& nameOrPath) {
  const std::wstring path =
      profiles::PathFor(m_szBaseDir, profiles::Kind::Mixer, nameOrPath);
  if (path.empty()) return false;
  return DeleteFileW(path.c_str()) != FALSE;
}

std::vector<std::wstring> Engine::MixerFaderOrder() {
  return MixerCfg().Get().order;
}

// The addressable order: the stored arrangement, then anything the providers
// publish that is not in it. What MIXER_ORDER reports and what a move edits.
//
// Note what it is NOT. It keeps stored keys for devices that are switched off
// -- MergeAbsentFaderOrder holds their place deliberately -- so this list is
// longer than the one the window draws, and an index into it is not a row on
// screen. That is the distinction forgejo#65 turns on.
static std::vector<std::wstring> MixerOrderKeys(Engine* self) {
  std::vector<std::wstring> keys = self->MixerFaderOrder();
  if (self->MixerIsEnabled() && g_mixer) {
    const MixerSnapshot snap = g_mixer->Snapshot();
    for (const Channel& c : snap.channels)
      for (const Fader& f : c.faders) {
        const std::wstring key = c.id + L"|" + f.id;
        if (std::find(keys.begin(), keys.end(), key) == keys.end())
          keys.push_back(key);
      }
  }
  return keys;
}

std::vector<Engine::MixerViewRow> Engine::MixerViewRows() {
  std::vector<MixerViewRow> rows;
  if (!MixerIsEnabled() || !g_mixer) return rows;

  const MixerSnapshot snap = g_mixer->Snapshot();

  // Where each key sits in the addressable order, so a client can render this
  // list and still move by an index MIXER_ORDER_MOVE understands.
  std::map<std::wstring, int> orderIndex;
  {
    const std::vector<std::wstring> keys = MixerOrderKeys(this);
    for (size_t i = 0; i < keys.size(); i++) orderIndex[keys[i]] = (int)i;
  }

  // -- The base list: the stored order, then anything new at the BOTTOM --
  //
  // A device that appears after the order was saved must not push the faders
  // someone uses every day off the visible part of the list.
  struct Row { std::wstring key, channel; bool muted; bool isVirtual; };
  std::vector<Row> list;
  size_t orderedCount = 0;   // how many of `list` the stored order names
  {
    std::vector<Row> remaining;
    for (const Channel& c : snap.channels)
      for (const Fader& f : c.faders)
        remaining.push_back({ c.id + L"|" + f.id, c.id, f.muted, c.isVirtual });

    for (const std::wstring& key : MixerFaderOrder()) {
      for (size_t i = 0; i < remaining.size(); i++) {
        if (remaining[i].key != key) continue;
        list.push_back(remaining[i]);
        remaining.erase(remaining.begin() + (ptrdiff_t)i);
        break;
      }
    }
    orderedCount = list.size();
    for (const Row& r : remaining) list.push_back(r);
  }

  // -- Unmuted channels floated above muted ones --
  //
  // Optional and off by default. Applied ON TOP of the stored order and
  // stably, so within each group the arrangement is still the one the Move Up
  // / Move Down buttons produced.
  //
  // By CHANNEL, not by fader: a channel counts as muted only when every one of
  // its faders is, so the Sonar master -- monitoring muted, streaming live --
  // stays in one piece under its heading rather than being split across the
  // divide.
  //
  // Restricted to the faders the stored order NAMES. A device plugged in for
  // the first time is unmuted by definition, so floating unmuted channels
  // lifted it over every muted one the moment it appeared. New faders stay
  // where the block above put them, at the bottom, until they are placed.
  if (MixerCfg().Get().sortUnmutedFirst) {
    std::map<std::wstring, bool> allMuted;
    for (const Row& r : list) {
      auto it = allMuted.find(r.channel);
      if (it == allMuted.end()) allMuted[r.channel] = r.muted;
      else it->second = it->second && r.muted;
    }
    std::stable_partition(list.begin(), list.begin() + (ptrdiff_t)orderedCount,
                          [&](const Row& r) { return !allMuted[r.channel]; });
  }

  // -- Failover devices held at the top, while they are here --
  //
  // Applied LAST, so it wins over both the stored order and the sort above.
  // That is the point: sortUnmutedFirst files a row by its mute state, and the
  // device you reach for most is the one whose mute you toggle most.
  //
  // Membership of ANY route allowlist counts. Entries carry an id, or a
  // Windows name when they were added while the device was switched off (see
  // MixerAllowEntry), so both are matched -- an id against the channel, a name
  // against the endpoint the watcher knows by that name.
  //
  // "While connected" needs no test of its own: the endpoint provider
  // publishes a channel only for an ACTIVE endpoint, so a fader existing is
  // what being connected means here.
  std::set<std::wstring> pinnedKeys;
  if (MixerCfg().Get().pinFailoverDevices) {
    std::set<std::wstring> pinIds;
    for (const auto& route : MixerCfg().Get().routes) {
      for (const MixerAllowEntry& e : route.second.allow) {
        if (!e.id.empty()) pinIds.insert(e.id);
        if (e.name.empty() || !g_watcher) continue;
        for (const EndpointInfo& d : g_watcher->Endpoints())
          if (d.windowsName == e.name && !d.id.empty()) pinIds.insert(d.id);
      }
    }
    if (!pinIds.empty()) {
      const std::wstring prefix = L"endpoint:";
      const auto isPinned = [&](const Row& r) {
        if (r.channel.compare(0, prefix.size(), prefix) != 0) return false;
        return pinIds.count(r.channel.substr(prefix.size())) != 0;
      };
      for (const Row& r : list) if (isPinned(r)) pinnedKeys.insert(r.key);
      std::stable_partition(list.begin(), list.end(), isPinned);
    }
  }

  // -- Provider-owned endpoints dropped, unless asked for --
  //
  // They truncate to exactly the provider channel names they shadow, sit at
  // 1.000 and ignore a write, so the row that LOOKS like the one you want is a
  // decoy while the real channel sits below the fold (forgejo#51). Reported
  // rather than silently omitted, so a client can offer the same switch.
  // -- And faders the USER hid, unless asked for --
  //
  // A second, unrelated reason for a row not to be drawn (forgejo#50), and the
  // two are kept apart deliberately. `hidden` on a view row has always meant
  // "not drawn", whatever the reason, and it keeps that meaning; `userHidden`
  // says the reason is a stored preference rather than the virtual-endpoint
  // filter. A client needs the distinction because "Show hidden faders" and
  // "Show virtual endpoints" are separate switches, and offering to reveal a
  // row that the OTHER switch is suppressing would do nothing.
  //
  // Note the asymmetry with MIXER_FADER's `hidden=`, which is the stored
  // preference alone -- there it is the only sense the field could have, since
  // a fader record says nothing about the drawn list.
  const bool showVirtual = MixerCfg().Get().showVirtualEndpoints;
  const bool showHidden = MixerCfg().Get().showHiddenFaders;
  int pos = 0;
  rows.reserve(list.size());
  for (const Row& r : list) {
    MixerViewRow out;
    out.key = r.key;
    const auto it = orderIndex.find(r.key);
    out.order = (it == orderIndex.end()) ? -1 : it->second;
    out.userHidden = MixerCfg().IsHidden(r.key);
    out.hidden = (!showVirtual && r.isVirtual) || (!showHidden && out.userHidden);
    out.pinned = pinnedKeys.count(r.key) != 0;
    out.pos = out.hidden ? -1 : pos++;
    rows.push_back(out);
  }
  return rows;
}

void Engine::MixerNotifyViewChanged() {
  // Only the TOKEN goes out, never the list.
  //
  // A client compares eight characters against what it holds and asks for
  // MIXER_VIEW only when they differ, so the common case -- a fader moving,
  // which reorders nothing -- costs one short message instead of twenty-five
  // rows. Pushing the list would also race the client's own fetch, and this
  // cannot: the token it carries is the one the next fetch will answer with.
  if (g_subscribers <= 0) return;
  const std::wstring now = MixerOrderRev();
  if (now == g_lastViewRev) return;
  g_lastViewRev = now;
  g_tcpServer.Broadcast(Fmt(L"MIXER_VIEW_CHANGED|rev=%s", now.c_str()));
}

std::wstring Engine::MixerOrderRev() {
  // FNV-1a 32, eight hex digits.
  //
  // Deliberately NOT ComputePresetHashFromBytes. That one is a persisted file
  // format whose rule must never change (preset_hash.h), and giving it a
  // second caller over different input is how such a rule starts to move. This
  // value lives for as long as one phone screen and is written nowhere, so a
  // short local hash is the right size of tool.
  //
  // It covers three things, and each is here for a reason:
  //
  //   the addressable order   a move is an index into it, so a change there
  //                           changes what a move would do;
  //   the drawn sequence      what the client is actually looking at, which
  //                           can change while the stored order does not;
  //   the three view flags    so toggling one in the window invalidates a
  //                           list a remote fetched before it.
  //
  // A consequence worth knowing: with sortUnmutedFirst ON, muting anything
  // reorders the view and so moves the token. A remote's pending move is then
  // refused and it re-fetches -- correct, because the list it was showing
  // really did change, but it does make the token livelier with that flag set.
  std::wstring material;
  for (const std::wstring& k : MixerOrderKeys(this)) material += k + L"\n";
  material += L"--\n";
  for (const MixerViewRow& r : MixerViewRows())
    material += Fmt(L"%d|%d|%d|%s\n", r.pos, r.hidden ? 1 : 0,
                    r.pinned ? 1 : 0, r.key.c_str());
  material += Fmt(L"--|%d%d%d", MixerCfg().Get().sortUnmutedFirst ? 1 : 0,
                  MixerCfg().Get().pinFailoverDevices ? 1 : 0,
                  MixerCfg().Get().showVirtualEndpoints ? 1 : 0);

  unsigned h = 2166136261u;
  for (wchar_t wc : material) {
    const unsigned c = (unsigned)(unsigned short)wc;
    h = (h ^ (c & 0xFFu)) * 16777619u;
    h = (h ^ ((c >> 8) & 0xFFu)) * 16777619u;
  }
  return Fmt(L"%08x", h);
}

void Engine::MixerSetFaderOrder(const std::vector<std::wstring>& order) {
  // The caller only knows the faders that EXIST right now, so writing its list
  // verbatim forgot any device that happened to be unplugged. A place near the
  // top is kept for those -- see MergeAbsentFaderOrder.
  MixerCfg().Get().order =
      MergeAbsentFaderOrder(order, MixerCfg().Get().order);
  MixerCfg().Save();
  // The one place the stored order is written, so the one place a reorder made
  // at the PC has to be announced. A second remote holding the old token finds
  // out here rather than the next time it happens to look.
  MixerNotifyViewChanged();
}

float Engine::MixerVolumeStep() const {
  const int pct = MixerCfg().Get().volumeStepPercent;
  return (float)(pct <= 0 ? 5 : pct) / 100.0f;
}

int Engine::MixerGroupCount() const {
  return (int)MixerCfg().Get().groups.size();
}

std::vector<std::wstring> Engine::MixerGroupMembers(int group) const {
  const std::vector<MixerGroupCfg>& g = MixerCfg().Get().groups;
  if (group < 0 || group >= (int)g.size()) return std::vector<std::wstring>();
  return g[(size_t)group].members;
}

bool Engine::MixerSetGroupMembers(int group,
                                  const std::vector<std::wstring>& members) {
  std::vector<MixerGroupCfg>& g = MixerCfg().Get().groups;
  if (group < 0 || group >= (int)g.size()) return false;
  // Deliberately NOT validated against the current channel list, for the same
  // reason the slots it replaced were not: a member may legitimately name a
  // Sonar channel while Sonar is restarting, or a headset that is switched
  // off, and refusing the write then would throw the setting away.
  g[(size_t)group].members = members;
  MixerCfg().Save();
  return true;
}

// `present` is the count that can actually be moved right now. It differs from
// `members` exactly when a member's hardware is absent, which is worth seeing
// rather than inferring from a key press that did less than expected.
std::wstring Engine::MixerGroupLine(int group,
                                    const std::wstring& extra) const {
  const std::vector<MixerGroupCfg>& g = MixerCfg().Get().groups;
  if (group < 0 || group >= (int)g.size()) return std::wstring();
  const MixerGroupCfg& cfg = g[(size_t)group];

  // A pipe in the NAME would corrupt the record exactly as one in a key does,
  // and the name is user-editable. Replaced rather than refused: a name is
  // decoration, and losing the window's list to a stray character would not be.
  std::wstring name = cfg.name;
  for (wchar_t& c : name)
    if (c == L'|' || c == L'\n' || c == L'\r') c = L' ';

  std::wstring head = Fmt(L"MIXER_GROUP|group=%d|name=%s|members=%d",
                          group + 1, name.c_str(), (int)cfg.members.size());

  int present = 0;
  if (MixerIsEnabled()) {
    const MixerSnapshot snap = g_mixer->Snapshot();
    for (const std::wstring& key : cfg.members) {
      const std::vector<std::wstring> t = SplitArgs(key);
      if (t.size() < 2) continue;
      for (const Channel& c : snap.channels) {
        if (c.id != t[0]) continue;
        for (const Fader& f : c.faders)
          if (f.id == t[1]) { present++; break; }
        break;
      }
    }
  }
  head += Fmt(L"|present=%d", present);
  if (!extra.empty()) head += extra;

  // The keys LAST, comma-separated, in one field.
  //
  // A key is "<channel>|<fader>" and the pipe is this protocol's field
  // separator, so a key cannot sit anywhere but the end: a reader splits the
  // line on "|" and takes everything after "keys=" verbatim. MIXER_ORDER_SET
  // carries its key list the same way for the same reason. Emitting one
  // "member=" field per fader looked tidier and was unparseable -- every key
  // arrived truncated at its pipe.
  if (!cfg.members.empty()) {
    head += L"|keys=";
    for (size_t i = 0; i < cfg.members.size(); i++) {
      if (i) head += L",";
      head += cfg.members[i];
    }
  }
  return head;
}

int Engine::MixerNudgeGroup(int group, float delta) {
  if (!MixerIsEnabled()) return 0;
  const std::vector<std::wstring> members = MixerGroupMembers(group);
  if (members.empty()) return 0;

  int moved = 0;
  const MixerSnapshot snap = g_mixer->Snapshot();
  for (const std::wstring& key : members) {
    const std::vector<std::wstring> t = SplitArgs(key);
    if (t.size() < 2) continue;
    for (const Channel& c : snap.channels) {
      if (c.id != t[0]) continue;
      for (const Fader& f : c.faders) {
        if (f.id != t[1]) continue;
        float value = f.volume + delta;
        // Clamped per fader, never as a group. A group that started spread out
        // must still be spread out after the key is held at the top and let
        // back down; scaling the members together, or refusing the whole press
        // because one of them is at the rail, both flatten that.
        if (value < 0.0f) value = 0.0f;
        if (value > 1.0f) value = 1.0f;
        g_mixer->SetVolume(t[0], t[1], value);
        moved++;
        break;
      }
      break;
    }
  }
  return moved;
}

int Engine::MixerToggleGroupMute(int group) {
  if (!MixerIsEnabled()) return 0;
  const std::vector<std::wstring> members = MixerGroupMembers(group);
  if (members.empty()) return 0;

  // Resolve the present members first, then take ONE decision for all of them.
  // Toggling each fader independently would leave a group whose members
  // disagree flip-flopping between two mixed states forever, and a mute key
  // has to be able to reach silence.
  std::vector<std::pair<std::wstring, std::wstring>> targets;
  bool anyUnmuted = false;
  const MixerSnapshot snap = g_mixer->Snapshot();
  for (const std::wstring& key : members) {
    const std::vector<std::wstring> t = SplitArgs(key);
    if (t.size() < 2) continue;
    for (const Channel& c : snap.channels) {
      if (c.id != t[0]) continue;
      for (const Fader& f : c.faders) {
        if (f.id != t[1]) continue;
        // A fader that refuses mute is skipped rather than counted. Both Sonar
        // masters refuse, and letting one of those decide the group direction
        // would make the key do nothing while looking like it should.
        if (!f.canMute) break;
        targets.push_back({ t[0], t[1] });
        if (!f.muted) anyUnmuted = true;
        break;
      }
      break;
    }
  }

  for (const auto& t : targets) g_mixer->SetMute(t.first, t.second, anyUnmuted);
  return (int)targets.size();
}

// What the hotkeys call. A key press that reaches nothing has to say so on
// screen: a keystroke has no reply channel, so a silent no-op would read as a
// broken key rather than an empty group.
void Engine::MixerGroupHotkey(int group, float delta, bool toggleMute) {
  if (!MixerIsEnabled()) {
    AddNotification((wchar_t*)L"Audio mixer is off");
    return;
  }
  const std::vector<MixerGroupCfg>& cfgs = MixerCfg().Get().groups;
  if (group < 0 || group >= (int)cfgs.size()) {
    AddNotification((wchar_t*)L"No such mixer group");
    return;
  }
  const MixerGroupCfg cfg = cfgs[(size_t)group];
  // Separated from "nothing moved" before anything is attempted, because the
  // two read identically from a key press and mean opposite things: an empty
  // group is a setting nobody filled in, a full group that moves nothing is
  // hardware that is not here.
  if (cfg.members.empty()) {
    AddNotification((wchar_t*)L"That mixer group has no faders ticked");
    return;
  }

  const int n = toggleMute ? MixerToggleGroupMute(group)
                           : MixerNudgeGroup(group, delta);
  if (n <= 0) {
    AddNotification((wchar_t*)(toggleMute
        ? L"Nothing in that group can be muted"
        : L"Nothing in that group is present"));
    return;
  }

  // Counted, not named: five fader names will not fit on a notification, and
  // the count is the part that says whether the key reached what was expected.
  wchar_t text[256];
  if (toggleMute) {
    // Read the state back rather than reporting the requested one, the same
    // rule the sliders follow: a provider may refuse or quantise, and the word
    // on screen should be what is true.
    bool muted = false;
    const MixerSnapshot snap = g_mixer->Snapshot();
    for (const std::wstring& key : cfg.members) {
      const std::vector<std::wstring> t = SplitArgs(key);
      if (t.size() < 2) continue;
      bool found = false;
      for (const Channel& c : snap.channels) {
        if (c.id != t[0]) continue;
        for (const Fader& f : c.faders)
          if (f.id == t[1]) { muted = f.muted; found = true; break; }
        break;
      }
      if (found) break;
    }
    FormatTo(text, L"%s: %d fader%s %s", cfg.name.c_str(), n,
             n == 1 ? L"" : L"s", muted ? L"muted" : L"unmuted");
  } else {
    const int pct = (int)(MixerVolumeStep() * 100.0f + 0.5f);
    FormatTo(text, L"%s: %d fader%s %s%d%%", cfg.name.c_str(), n,
             n == 1 ? L"" : L"s", delta < 0.0f ? L"−" : L"+", pct);
  }
  AddNotification(text);
}

bool Engine::MixerHandleCommand(const wchar_t* message, std::wstring& reply) {
  const std::wstring msg = message ? message : L"";

  if (msg.rfind(L"MIXER_ENABLE=", 0) == 0) {
    const bool want = msg.substr(13) != L"0";
    MixerSetEnabled(want);
    reply = Fmt(L"MIXER_ENABLED=%d", MixerIsEnabled() ? 1 : 0);
    return true;
  }

  if (msg.rfind(L"MIXER_SUBSCRIBE=", 0) == 0) {
    const bool want = msg.substr(16) != L"0";
    MixerSetSubscribed(want);
    reply = Fmt(L"MIXER_SUBSCRIBED=%d", want ? 1 : 0);
    return true;
  }

  if (msg.rfind(L"MIXER_SET=", 0) == 0) {
    const std::vector<std::wstring> a = SplitArgs(msg.substr(10));
    if (a.size() < 3) {
      reply = L"MIXER_ERR|reason=malformed";
      return true;
    }
    if (!MixerIsEnabled()) {
      reply = L"MIXER_ERR|reason=disabled";
      return true;
    }
    if (!FaderExists(a[0], a[1])) {
      reply = L"MIXER_ERR|reason=unknown_fader";
      return true;
    }

    const float value = (float)_wtof(a[2].c_str());
    g_mixer->SetVolume(a[0], a[1], value);

    // Optimistic, and deliberately NOT synchronised. Waiting for the worker
    // here would block the message pump behind a provider that may be slow or
    // hung, which is exactly what the worker exists to prevent. The reply
    // carries the requested value; the broadcast that follows the write
    // carries the truth.
    const float clamped = value < 0.0f ? 0.0f : (value > 1.0f ? 1.0f : value);
    const MixerSnapshot snap = g_mixer->Snapshot();
    for (const Channel& c : snap.channels) {
      if (c.id != a[0]) continue;
      for (const Fader& f : c.faders) {
        if (f.id != a[1]) continue;
        Fader optimistic = f;
        optimistic.volume = clamped;
        reply = FaderLine(c, optimistic);
      }
    }
    return true;
  }

  if (msg.rfind(L"MIXER_MUTE=", 0) == 0) {
    const std::vector<std::wstring> a = SplitArgs(msg.substr(11));
    if (a.size() < 3) {
      reply = L"MIXER_ERR|reason=malformed";
      return true;
    }
    if (!MixerIsEnabled()) {
      reply = L"MIXER_ERR|reason=disabled";
      return true;
    }
    if (!FaderExists(a[0], a[1])) {
      reply = L"MIXER_ERR|reason=unknown_fader";
      return true;
    }

    // Refused rather than attempted. The write would succeed and take every
    // render channel's mute with it -- see canMute in mixer_provider_sonar.cpp.
    // A named error is the difference between a client disabling its button
    // and a client offering one that quietly does damage.
    if (!FaderCanMute(a[0], a[1])) {
      reply = L"MIXER_ERR|reason=cannot_mute";
      return true;
    }

    bool muted = (a[2] == L"1");
    if (a[2] == L"toggle") {
      const MixerSnapshot snap = g_mixer->Snapshot();
      for (const Channel& c : snap.channels)
        if (c.id == a[0])
          for (const Fader& f : c.faders)
            if (f.id == a[1]) muted = !f.muted;
    }
    g_mixer->SetMute(a[0], a[1], muted);
    reply = Fmt(L"MIXER_MUTED|ch=%s|id=%s|mute=%d", a[0].c_str(), a[1].c_str(),
                muted ? 1 : 0);
    return true;
  }

  if (msg.rfind(L"MIXER_ROUTE_ARM=", 0) == 0) {
    const std::vector<std::wstring> a = SplitArgs(msg.substr(16));
    if (a.size() < 2) { reply = L"MIXER_ERR|reason=malformed"; return true; }
    RouteRule rule = LoadRule(a[0]);
    rule.armed = (a[1] != L"0");
    g_failover.SetRule(rule);
    SaveRule(rule);
    reply = Fmt(L"MIXER_ROUTE|id=%s|armed=%d", a[0].c_str(), rule.armed ? 1 : 0);
    return true;
  }

  if (msg.rfind(L"MIXER_ALLOW_ADD=", 0) == 0 ||
      msg.rfind(L"MIXER_ALLOW_REMOVE=", 0) == 0) {
    const bool add = msg.rfind(L"MIXER_ALLOW_ADD=", 0) == 0;
    const std::vector<std::wstring> a = SplitArgs(msg.substr(add ? 16 : 19));
    if (a.size() < 2) { reply = L"MIXER_ERR|reason=malformed"; return true; }

    RouteRule rule = LoadRule(a[0]);
    if (add) {
      AllowEntry e;
      e.id = a[1];
      // Capture the friendly name too. A re-paired Bluetooth device comes back
      // under a new id, and the name is what still matches it.
      if (g_watcher)
        for (const EndpointInfo& ep : g_watcher->Endpoints())
          if (ep.id == a[1]) e.name = ep.friendlyName;
      // An optional third argument names the device outright, which is the
      // only way to list one that is not connected right now -- and a pair of
      // Bluetooth headphones is switched off far more often than it is on.
      // The watcher publishes active endpoints only, so there is no id to give
      // until the moment it comes back.
      if (a.size() >= 3 && !a[2].empty()) e.name = a[2];
      if (e.id.empty() && e.name.empty()) {
        reply = L"MIXER_ERR|reason=need_id_or_name";
        return true;
      }
      rule.allow.push_back(e);
    } else {
      // By id OR by name. Matching on the id alone was right when every entry
      // had one; a device that is not connected is listed by NAME with no id
      // at all, and those could never be removed again -- they just piled up,
      // which is how a test that adds three entries and removes them started
      // finding four.
      for (size_t i = 0; i < rule.allow.size(); i++) {
        if (AllowEntryMatches(rule.allow[i], a[1])) {
          rule.allow.erase(rule.allow.begin() + i);
          break;
        }
      }
    }
    g_failover.SetRule(rule);
    SaveRule(rule);
    reply = Fmt(L"MIXER_ROUTE|id=%s|allowed=%d", a[0].c_str(),
                (int)rule.allow.size());
    return true;
  }

  if (msg.rfind(L"MIXER_ALLOW_MOVE=", 0) == 0) {
    // Reorder one route's allowlist. The order is the preference: `allow` is
    // "first present entry wins", so this is what decides which replacement
    // failover reaches for when more than one is around.
    //
    // Delta and swap, matching MIXER_ORDER_MOVE= for faders exactly, so a
    // client that already drives fader order needs no second pattern.
    const std::vector<std::wstring> a = SplitArgs(msg.substr(17));
    if (a.size() < 3 || !MixerIsEnabled()) {
      reply = L"MIXER_ERR|reason=malformed";
      return true;
    }
    RouteRule rule = LoadRule(a[0]);
    const int to = MoveAllowEntry(rule, a[1], _wtoi(a[2].c_str()));
    if (to < 0) {
      reply = L"MIXER_ERR|reason=unknown_device";
      return true;
    }
    g_failover.SetRule(rule);
    SaveRule(rule);
    reply = Fmt(L"MIXER_ALLOW|route=%s|pos=%d|id=%s", a[0].c_str(), to,
                a[1].c_str());
    return true;
  }

  if (msg == L"MIXER_ALLOW" || msg.rfind(L"MIXER_ALLOW=", 0) == 0) {
    // One row per allowed replacement, in priority order, with everything a
    // client needs to render the list: where it sits, whether it is here now,
    // and when it was last seen if it is not.
    //
    // Separate from DIAG_FAILOVER rather than folded into it. That reply's
    // `allow=` is a bare comma-separated id list which the failover tests
    // parse, and widening it would break them to say something this can say
    // better.
    const std::wstring routeId = (msg.size() > 12) ? msg.substr(12)
                                                   : L"endpoint:default-render";
    const RouteRule rule = LoadRule(routeId);
    for (size_t i = 0; i < rule.allow.size(); i++) {
      const AllowEntry& e = rule.allow[i];
      // The record, whether or not the device is here now. Endpoints() carries
      // both, distinguished by isActive -- so one lookup answers presence AND
      // when it was last seen, and an absent device still has a date.
      //
      // Matched on windowsName as well as friendlyName: an entry added while
      // its device was switched off has no id and stored the WINDOWS name,
      // while friendlyName carries any short name since given to it.
      const EndpointInfo* rec = nullptr;
      if (g_watcher) {
        for (const EndpointInfo& ep : g_watcher->Endpoints()) {
          if ((!e.id.empty()   && ep.id == e.id) ||
              (!e.name.empty() && (ep.friendlyName == e.name ||
                                   ep.windowsName  == e.name))) {
            rec = &ep;
            break;
          }
        }
      }
      reply += Fmt(L"MIXER_ALLOW|route=%s|pos=%d|id=%s|name=%s|present=%d|seen=%s\n",
                   routeId.c_str(), (int)i, e.id.c_str(), e.name.c_str(),
                   (rec && rec->isActive) ? 1 : 0,
                   rec ? FormatLastSeen(rec->lastSeen).c_str() : L"");
    }
    reply += Fmt(L"MIXER_ALLOW_END|route=%s|count=%d", routeId.c_str(),
                 (int)rule.allow.size());
    return true;
  }

  if (msg.rfind(L"MIXER_ROUTE_SET=", 0) == 0) {
    const std::vector<std::wstring> a = SplitArgs(msg.substr(16));
    if (a.size() < 2 || !MixerIsEnabled()) {
      reply = L"MIXER_ERR|reason=malformed";
      return true;
    }
    g_mixer->SetRouteDevice(a[0], a[1]);
    // Re-baseline immediately: a manual move answers the question the watcher
    // might be about to answer, so any pending arm is abandoned.
    g_failover.NoteRouteDevice(a[0], a[1]);
    reply = Fmt(L"MIXER_ROUTE|id=%s|device=%s", a[0].c_str(), a[1].c_str());
    return true;
  }

  if (msg == L"DIAG_FAILOVER" || msg.rfind(L"DIAG_FAILOVER=", 0) == 0) {
    // Which route, said out loud. This used to be chosen by a config flag,
    // and while moving the settings to JSON it briefly became "whichever
    // route has a rule" -- which quietly repointed the diagnostic at the Sonar
    // route the moment one was configured, and five failover tests started
    // reporting on a route they had never armed.
    const std::wstring routeId = (msg.size() > 14) ? msg.substr(14)
                                                   : L"endpoint:default-render";
    const RouteRule rule = LoadRule(routeId);
    std::wstring allow;
    for (const AllowEntry& e : rule.allow) {
      if (!allow.empty()) allow += L",";
      allow += e.id;
    }
    const wchar_t* state = L"idle";
    switch (g_failover.StateOf(routeId)) {
      case FailoverState::Searching: state = L"searching"; break;
      case FailoverState::Arming:    state = L"arming"; break;
      default: break;
    }
    reply = Fmt(L"DIAG_FAILOVER|route=%s|armed=%d|state=%s|allow=%s|reason=%s",
                routeId.c_str(), rule.armed ? 1 : 0, state, allow.c_str(),
                g_failover.ReasonOf(routeId).c_str());
    return true;
  }

  if (msg.rfind(L"MIXER_SIM_DEVICE=", 0) == 0) {
    // Testing mode only. This lies to the watcher about the world, which is
    // what makes disconnect and re-pair testable without hardware -- and
    // exactly why it must not be reachable in ordinary use.
    if (!m_bTestingMode) {
      reply = L"MIXER_ERR|reason=testing_mode_required";
      return true;
    }
    const std::vector<std::wstring> a = SplitArgs(msg.substr(17));
    if (a.size() < 2) { reply = L"MIXER_ERR|reason=malformed"; return true; }
    if (a[1] == L"gone")          g_simPresence[a[0]] = false;
    else if (a[1] == L"present")  g_simPresence[a[0]] = true;
    else if (a[1] == L"forget")   g_simPresence.erase(a[0]);
    else if (a[1] == L"current")
      g_failover.NoteRouteDevice(L"endpoint:default-render", a[0]);
    g_failover.Tick();
    reply = Fmt(L"MIXER_SIM|device=%s|%s", a[0].c_str(), a[1].c_str());
    return true;
  }

  if (msg == L"UI_LIST") {
    // Every window a caller may name, and whether it is open. Nothing here
    // needs a window handle, which is the point: a remote cannot hold one.
    for (const std::wstring& row : ToolWindowNames()) {
      if (!reply.empty()) reply += L"\n";
      reply += L"UI_WINDOW|name=" + row;
    }
    if (reply.empty()) reply = L"UI_ERR|reason=no_windows";
    return true;
  }

  if (msg.rfind(L"UI_CLOSE=", 0) == 0) {
    const std::wstring name = msg.substr(9);
    reply = CloseToolWindowUI(name)
        ? Fmt(L"UI_CLOSED|name=%s", name.c_str())
        : Fmt(L"UI_ERR|reason=not_open|name=%s", name.c_str());
    return true;
  }

  if (msg.rfind(L"UI_CLICK=", 0) == 0) {
    const std::vector<std::wstring> a = SplitArgs(msg.substr(9));
    int ctrlId = 0;
    for (size_t i = 1; i < a.size(); i++)
      if (a[i].rfind(L"ctrl=", 0) == 0) ctrlId = _wtoi(a[i].substr(5).c_str());
    if (a.empty() || ctrlId <= 0) { reply = L"UI_ERR|reason=malformed"; return true; }
    // Accepted, not done: the window presses it on its own thread and answers
    // with UI_CLICKED|pressed=... -- a disabled control is reported, not
    // silently ignored.
    reply = ClickToolWindowUI(a[0], ctrlId)
        ? Fmt(L"UI_ACCEPTED|name=%s|ctrl=%d", a[0].c_str(), ctrlId)
        : Fmt(L"UI_ERR|reason=not_open|name=%s", a[0].c_str());
    return true;
  }

  if (msg.rfind(L"UI_MOVE=", 0) == 0) {
    const std::vector<std::wstring> a = SplitArgs(msg.substr(8));
    if (a.empty()) { reply = L"UI_ERR|reason=malformed"; return true; }
    int x = INT_MIN, y = INT_MIN;
    if (a.size() >= 3 && a[1] != L"render") {
      x = _wtoi(a[1].c_str());
      y = _wtoi(a[2].c_str());
    }
    reply = MoveToolWindowUI(a[0], x, y)
        ? Fmt(L"UI_MOVED|name=%s|x=%d|y=%d", a[0].c_str(), x, y)
        : Fmt(L"UI_ERR|reason=not_open|name=%s", a[0].c_str());
    return true;
  }

  if (msg.rfind(L"UI_SHOW=", 0) == 0) {
    // Open a tool window, optionally at a page and on a control.
    //
    //   UI_SHOW=OpenSettings
    //   UI_SHOW=OpenSettings|tab=2
    //   UI_SHOW=OpenSettings|ctrl=9410
    //
    // Not a mixer verb by rights, but this is where the IPC surface for
    // windows currently lives; the work behind it is on Engine and ToolWindow
    // and is not mixer-specific at all.
    const std::vector<std::wstring> a = SplitArgs(msg.substr(8));
    if (a.empty() || a[0].empty()) {
      reply = L"UI_ERR|reason=malformed";
      return true;
    }
    int tab = -1, ctrlId = 0;
    for (size_t i = 1; i < a.size(); i++) {
      if (a[i].rfind(L"tab=", 0) == 0) tab = _wtoi(a[i].substr(4).c_str());
      else if (a[i].rfind(L"ctrl=", 0) == 0) ctrlId = _wtoi(a[i].substr(5).c_str());
    }
    if (!ShowToolWindowUI(a[0], tab, ctrlId)) {
      reply = Fmt(L"UI_ERR|reason=unknown_window|name=%s", a[0].c_str());
      return true;
    }
    // Accepted, not finished. A control request is answered by the WINDOW,
    // on its own thread, with UI_SHOWN|window=...|ctrl=...|found=... once it
    // has actually looked -- which is the only place that knows.
    reply = ctrlId > 0
        ? Fmt(L"UI_ACCEPTED|name=%s|tab=%d|ctrl=%d", a[0].c_str(), tab, ctrlId)
        : Fmt(L"UI_SHOWN|name=%s|tab=%d", a[0].c_str(), tab);
    return true;
  }

  if (msg.rfind(L"MIXER_RENAME_DEVICE=", 0) == 0) {
    // Ids contain ':' and '{}' but never '|', so the first one separates.
    const std::wstring rest = msg.substr(20);
    const size_t bar = rest.find(L'|');
    if (bar == std::wstring::npos || bar == 0 || bar + 1 >= rest.size()) {
      reply = L"MIXER_ERR|reason=malformed";
      return true;
    }
    const std::wstring id = rest.substr(0, bar);
    const std::wstring name = rest.substr(bar + 1);

    // OUR name, in mixer.json -- not Windows'.
    //
    // Writing PKEY_Device_DeviceDesc through IPolicyConfig is refused on this
    // machine, which matches Shane's experience of trying "registry rescans,
    // reboots, etc." and getting nowhere. The Windows attempt is still made
    // first, because when it does work the name is visible everywhere in
    // Windows and not just here; when it does not, ours stands.
    // windowsName, NOT friendlyName.
    //
    // The watcher's name hook has already replaced friendlyName with the alias
    // for any device that has one, so reading it here stored the PREVIOUS
    // alias as "the name Windows uses" on every rename after the first. That
    // name is the thread back to a re-paired device, which comes back under a
    // brand new endpoint id -- so the entry would have been stranded, holding
    // a name nothing answers to.
    const std::wstring windowsName = [&]() -> std::wstring {
      if (!g_watcher) return std::wstring();
      for (const EndpointInfo& e : g_watcher->Endpoints())
        if (e.id == id) return e.windowsName;
      return std::wstring();
    }();
    const bool inWindows = RenameEndpoint(id, name);
    if (name == L"-") {
      // By id, then by the Windows name. An entry made while the device was
      // switched off has no id to be found by, and clearing has to reach it
      // either way -- a name you cannot remove is worse than one you cannot
      // set.
      if (!MixerCfg().ClearAlias(id) && !windowsName.empty())
        MixerCfg().ClearAlias(windowsName);
    }
    else MixerCfg().SetAlias(id, windowsName, name);
    // The name is what the failover allowlist matches on, so a rename has to
    // reach the snapshot rather than waiting for the next device event.
    //
    // And the reply must not arrive BEFORE it has. RefreshNow only wakes the
    // worker, so without the wait this returned while the channel list still
    // carried the old name -- and the window rebuilds itself the instant this
    // replies, reading that stale snapshot and drawing the name the user had
    // just changed. Measured: renaming and reopening the window showed the new
    // name, renaming and rebuilding immediately showed the old one.
    //
    // Same bounded wait MIXER_REFRESH uses, and for the same reason: a rename
    // is a rare, user-initiated act, so it can afford to answer with the truth.
    if (g_watcher) g_watcher->Refresh();
    if (g_mixer) {
      g_mixer->RefreshNow();
      g_mixer->WaitForIdle(2000);
    }
    reply = Fmt(L"MIXER_RENAMED|id=%s|name=%s|windows=%d", id.c_str(),
                name.c_str(), inWindows ? 1 : 0);
    return true;
  }

  if (msg.rfind(L"MIXER_CFG_FILE=", 0) == 0) {
    // Testing only. Without this a test can prove the settings CHANGE but
    // never that they survive a save and a load, because testing mode makes
    // the save a no-op to protect the real file. Pointed at a scratch path,
    // the round trip is the real one. Refused outside testing mode so no
    // remote can redirect a user's settings somewhere they will not find them.
    if (!m_bTestingMode) {
      reply = L"MIXER_ERR|reason=testing_mode_required";
      return true;
    }
    MixerCfg().SetTestPath(msg.substr(15));
    reply = Fmt(L"MIXER_CFG_FILE|path=%s", MixerCfg().StorePath().c_str());
    return true;
  }

  // ── Mixer profiles ──
  //
  // resources/profiles/mixer/<name>.json, one file each. An empty name banks a
  // timestamp, so "just save this" needs no name invented for it.
  if (msg == L"MIXER_PROFILE_LIST") {
    for (const std::wstring& leaf : ListMixerProfiles()) {
      if (!reply.empty()) reply += L"\n";
      // Both, because they differ whenever a typed name needed sanitising:
      // the leaf addresses the file, the name is what a person called it.
      reply += Fmt(L"MIXER_PROFILE|file=%s|name=%s", leaf.c_str(),
                   MixerProfileName(leaf).c_str());
    }
    if (reply.empty()) reply = L"MIXER_PROFILE_NONE";
    return true;
  }

  if (msg.rfind(L"MIXER_PROFILE_SAVE=", 0) == 0) {
    const std::vector<std::wstring> a = SplitArgs(msg.substr(19));
    // "|all" adds the wider mixer configuration -- failover rules, device
    // aliases, confirmation. Off by default so the usual profile stays small.
    const bool all = a.size() > 1 && (a[1] == L"all" || a[1] == L"1");
    const std::wstring leaf = SaveMixerProfile(a.empty() ? std::wstring() : a[0], all);
    reply = leaf.empty() ? L"MIXER_ERR|reason=save_failed"
                         : Fmt(L"MIXER_PROFILE_SAVED|file=%s|all=%d",
                               leaf.c_str(), all ? 1 : 0);
    return true;
  }

  if (msg.rfind(L"MIXER_PROFILE_LOAD=", 0) == 0) {
    const std::wstring name = msg.substr(19);
    if (name.empty()) { reply = L"MIXER_ERR|reason=malformed"; return true; }
    int applied = 0, skipped = 0;
    if (!LoadMixerProfile(name, &applied, &skipped)) {
      reply = L"MIXER_ERR|reason=unknown_profile";
      return true;
    }
    // skipped is reported, never fatal: a profile recorded with headphones
    // plugged in must still load with them switched off.
    reply = Fmt(L"MIXER_PROFILE_LOADED|file=%s|applied=%d|skipped=%d",
                name.c_str(), applied, skipped);
    // No explicit broadcast: SetVolume/SetMute above fire the provider's
    // Changed callback, which is what pushes fader lines to subscribers.
    return true;
  }

  if (msg.rfind(L"MIXER_PROFILE_DELETE=", 0) == 0) {
    const std::wstring name = msg.substr(21);
    if (name.empty()) { reply = L"MIXER_ERR|reason=malformed"; return true; }
    reply = DeleteMixerProfile(name)
                ? Fmt(L"MIXER_PROFILE_DELETED|file=%s", name.c_str())
                : L"MIXER_ERR|reason=unknown_profile";
    return true;
  }

  if (msg == L"MIXER_CFG_PATH") {
    reply = Fmt(L"MIXER_CFG_FILE|path=%s|test=%d", MixerCfg().StorePath().c_str(),
                MixerCfg().UsingTestPath() ? 1 : 0);
    return true;
  }

  if (msg.rfind(L"MIXER_ALLOW=", 0) == 0) {
    // A route's preferred devices, and whether each is here NOW.
    //
    // The window shows this, but the state has to be answerable without one:
    // Shane wants it "able to be queried via ipc", and a phone cannot read a
    // list box. Present is judged the same way the window judges it -- by id
    // when the entry has one, otherwise by either name, since a device given a
    // short name still carries its Windows name underneath.
    const std::wstring routeId = msg.substr(12);
    const RouteRule rule = LoadRule(routeId);
    for (size_t i = 0; i < rule.allow.size(); i++) {
      const AllowEntry& e = rule.allow[i];
      bool present = false;
      int battery = -1;
      std::wstring windowsName;
      if (g_watcher) {
        for (const EndpointInfo& ep : g_watcher->Endpoints()) {
          const bool byId = !e.id.empty() && ep.id == e.id;
          const bool byName = !e.name.empty() &&
              (ep.friendlyName == e.name || ep.windowsName == e.name);
          if (!byId && !byName) continue;
          present = ep.isActive;
          // Already -1 unless the endpoint is active, so a disconnected
          // device cannot report a figure left over from last time.
          battery = ep.batteryPercent;
          windowsName = ep.windowsName;
          break;
        }
      }
      if (!reply.empty()) reply += L"\n";
      // Held in a local. Calling AliasFor inside the argument list took
      // c_str() of a TEMPORARY already destroyed by the time Fmt read it --
      // and AliasFor is not idempotent either: it rewrites a stored id when it
      // matches on name, so calling it twice does more than waste work.
      const std::wstring alias = MixerCfg().AliasFor(e.id, e.name);
      reply += Fmt(L"MIXER_ALLOW|route=%s|pos=%d|id=%s|name=%s|alias=%s|present=%d"
                   L"|battery=%d",
                   routeId.c_str(), (int)i,
                   e.id.empty() ? L"-" : e.id.c_str(),
                   e.name.empty() ? L"-" : e.name.c_str(),
                   alias.empty() ? L"-" : alias.c_str(),
                   present ? 1 : 0, battery);
    }
    if (reply.empty())
      reply = Fmt(L"MIXER_ALLOW|route=%s|pos=-1|count=0", routeId.c_str());
    return true;
  }

  if (msg.rfind(L"DIAG_BATTERY_SIM=", 0) == 0) {
    // Force the figure, so the three colour bands can be looked at rather
    // than reasoned about. "off" restores the real reading.
    const std::wstring arg = msg.substr(17);
    const int want = (arg == L"off") ? -2 : _wtoi(arg.c_str());
    g_hudBatterySim.store(want, std::memory_order_relaxed);
    MixerBatteryRecompute();
    reply = Fmt(L"DIAG_BATTERY_SIM|sim=%d|percent=%d", want,
                g_hudBattery.load(std::memory_order_relaxed));
    return true;
  }

  if (msg == L"DIAG_BATTERY") {
    // What the HUD line is doing, and why. Answers with the mixer disabled
    // too: "switched on but the subsystem is off" is one of the states worth
    // telling apart, and gating the verb would hide exactly that one.
    if (MixerIsEnabled() && g_watcher) {
      // Read now rather than reporting the last poll. A diagnostic that can
      // be up to thirty seconds stale is a diagnostic nobody can trust.
      g_watcher->RefreshBattery();
      MixerBatteryRecompute();
      g_hudBatteryLast.store(GetTickCount());
    }
    std::wstring device, name;
    const wchar_t* how = L"none";
    {
      std::lock_guard<std::mutex> lock(g_hudBatteryMutex);
      device = g_hudBatteryDevice;
      how = g_hudBatterySource;
    }
    if (!device.empty() && g_watcher)
      for (const EndpointInfo& e : g_watcher->Endpoints())
        if (e.id == device) { name = e.friendlyName; break; }
    reply = Fmt(L"DIAG_BATTERY|shown=%d|enabled=%d|percent=%d|source=%s"
                L"|device=%s|name=%s",
                m_bShowBattery ? 1 : 0, MixerIsEnabled() ? 1 : 0,
                g_hudBattery.load(std::memory_order_relaxed), how,
                device.empty() ? L"-" : device.c_str(),
                name.empty() ? L"-" : name.c_str());
    return true;
  }

  if (msg == L"MIXER_BATTERY") {
    // Re-reads Bluetooth battery levels, and nothing else.
    //
    // Its own verb because battery is the one thing in the snapshot that
    // changes with no event to announce it -- every other field is driven by a
    // device-change callback. MIXER_REFRESH would do the job too, but it also
    // re-reads every provider including Sonar over HTTP, which is far too much
    // to run on a window's one-second tick. This costs well under a
    // millisecond, so the window can simply call it while its device list is
    // in front.
    if (!MixerIsEnabled()) { reply = L"MIXER_ERR|reason=disabled"; return true; }
    const int reporting = g_watcher ? g_watcher->RefreshBattery() : 0;
    // Push the fader records, because battery now rides on them.
    //
    // Nothing else will. This figure changes with no device event -- which is
    // the reason it has a verb of its own -- so the ordinary change broadcast
    // never fires for it, and a subscriber that does not poll would hold the
    // charge from whenever it last asked for a full state.
    //
    // No provider is re-polled: FaderLine reads the battery from the watcher,
    // which RefreshBattery has just updated, so the cached channel list is
    // current enough and the cost this verb exists to avoid is still avoided.
    if (g_subscribers > 0 && g_mixer) {
      const MixerSnapshot snap = g_mixer->Snapshot();
      for (const Channel& c : snap.channels)
        for (const Fader& f : c.faders)
          g_tcpServer.Broadcast(FaderLine(c, f));
    }
    // The HUD figure rides along. This verb is what the mixer window sends
    // on its one-second tick, so while that window is up the HUD tracks it
    // exactly and the thirty-second poll never has to fire at all.
    MixerBatteryRecompute();
    g_hudBatteryLast.store(GetTickCount());
    reply = Fmt(L"MIXER_BATTERY|reporting=%d|hud=%d", reporting,
                g_hudBattery.load(std::memory_order_relaxed));
    return true;
  }

  if (msg == L"MIXER_REFRESH") {
    // Re-reads every provider now rather than waiting for a device event.
    // Nothing polls on a schedule, so a Sonar channel that appeared while the
    // window was closed would otherwise not show until something else woke the
    // worker. Bounded wait, so the reply describes the state after the refresh
    // rather than before it.
    if (!MixerIsEnabled()) { reply = L"MIXER_ERR|reason=disabled"; return true; }
    // The WATCHER first, then the providers.
    //
    // The endpoint provider builds its channels from the watcher's cached
    // endpoint list, and the short name a user gives a device is applied to
    // that cache by the watcher's name hook -- so re-polling the providers
    // alone hands back the names from before the rename. That is exactly what
    // happened: renaming a connected headset on the Failover tab left the
    // Mixer tab showing the old name, and pressing Refresh did not help,
    // because Refresh was this verb. Only MIXER_RENAME_DEVICE refreshed the
    // watcher, and the window's Set Name button did not use it.
    if (g_watcher) g_watcher->Refresh();
    g_mixer->RefreshNow();
    g_mixer->WaitForIdle(2000);
    const MixerSnapshot snap = g_mixer->Snapshot();
    int faders = 0;
    for (const Channel& c : snap.channels) faders += (int)c.faders.size();
    reply = Fmt(L"MIXER_REFRESHED|channels=%d|faders=%d|routes=%d",
                (int)snap.channels.size(), faders, (int)snap.routes.size());
    return true;
  }

  if (msg.rfind(L"MIXER_WINDOW=", 0) == 0) {
    // The window opens even when the subsystem is off: the switch that turns
    // it on lives in the window, so refusing here would make it unreachable.
    if (msg.substr(13) != L"0") {
      OpenAudioMixerWindow();
      // A tool window is created on its own thread, so m_hWnd is not set when
      // Open() returns and the reply would say open=0 for a window that is
      // about to appear. Bounded wait on the IPC thread, which is idle anyway
      // and where a rare user-initiated open can afford to block briefly.
      for (int waited = 0; waited < 2000 && !IsAudioMixerWindowOpen(); waited += 25)
        Sleep(25);
    } else {
      CloseAudioMixerWindow();   // Close() already waits for the thread to end
    }
    reply = Fmt(L"MIXER_WINDOW|open=%d", IsAudioMixerWindowOpen() ? 1 : 0);
    return true;
  }

  if (msg == L"MIXER_ORDER") {
    // The STORED order, then anything the providers publish that is not in it.
    //
    // This is the addressable order -- what MIXER_ORDER_MOVE edits and what
    // mixer.json holds -- and it is NOT what the window draws. Three view
    // rules sit on top of it before anything reaches the screen; ask
    // MIXER_VIEW for that list. This comment used to claim the opposite, and a
    // remote that believed it rendered a list disagreeing with the window on
    // its first and last rows (forgejo#65).
    //
    // It stays the stored order on purpose. MIXER_ORDER_MOVE rebuilds its
    // working list from this reply and writes the result back through
    // MixerSetFaderOrder, so answering with a sorted view here would save the
    // view into mixer.json -- the bug already fixed in the window, where a
    // device climbed the list on its own every time anything was reordered.
    const std::vector<std::wstring> keys = MixerOrderKeys(this);
    for (size_t i = 0; i < keys.size(); i++) {
      if (!reply.empty()) reply += L"\n";
      reply += Fmt(L"MIXER_ORDER|pos=%d|key=%s", (int)i, keys[i].c_str());
    }
    reply += (reply.empty() ? L"" : L"\n");
    reply += Fmt(L"MIXER_ORDER_END|count=%d|rev=%s", (int)keys.size(),
                 MixerOrderRev().c_str());
    return true;
  }

  if (msg == L"MIXER_VIEW") {
    // What the Mixer tab DRAWS: the stored order with the three view rules
    // applied, which is the list a person is actually looking at.
    //
    // Computed here rather than read out of the window, so the answer is the
    // same whether the window is open or not, and so there is one
    // implementation of the rules instead of two that can drift.
    //
    // Rows carry `order=`, their index in MIXER_ORDER space, because a move is
    // still expressed there: a client renders this list and moves in that one.
    // Where the two disagree -- a pinned row whose drawn position the stored
    // order does not control -- `pinned=1` says so, so a client can explain
    // why a move did not shift the row rather than looking broken.
    // `key=` is LAST, and every record carrying one must keep it there.
    //
    // A key is "<channel>|<fader>" and so contains the field separator, which
    // is why a reader takes everything after "|key=" rather than one field of
    // the split -- MessageParser.parseMixerOrder says so in as many words. A
    // field placed after it is silently swallowed into the key, and the client
    // then looks up a fader that does not exist. Caught here by a test whose
    // own parser had the same bug.
    const std::vector<MixerViewRow> rows = MixerViewRows();
    int shown = 0, hidden = 0;
    for (const MixerViewRow& r : rows) {
      if (r.hidden) hidden++; else shown++;
      // userHidden goes BEFORE key=, like everything else: a reader takes the
      // rest of the line as the key, so a field after it is swallowed.
      reply += Fmt(L"MIXER_VIEW|pos=%d|order=%d|hidden=%d|userHidden=%d"
                   L"|pinned=%d|key=%s\n",
                   r.pos, r.order, r.hidden ? 1 : 0, r.userHidden ? 1 : 0,
                   r.pinned ? 1 : 0, r.key.c_str());
    }
    // The three flags, so a client can explain the list rather than only
    // mirror it. `pinFailoverDevices` in particular is not reconstructable
    // from the wire: it needs every route's full allowlist, and MIXER_ROUTE
    // carries only the route's current device.
    reply += Fmt(L"MIXER_VIEW_END|count=%d|shown=%d|hidden=%d|rev=%s"
                 L"|sortUnmutedFirst=%d|pinFailoverDevices=%d"
                 L"|showVirtualEndpoints=%d|showHiddenFaders=%d",
                 (int)rows.size(), shown, hidden, MixerOrderRev().c_str(),
                 MixerCfg().Get().sortUnmutedFirst ? 1 : 0,
                 MixerCfg().Get().pinFailoverDevices ? 1 : 0,
                 MixerCfg().Get().showVirtualEndpoints ? 1 : 0,
                 MixerCfg().Get().showHiddenFaders ? 1 : 0);
    return true;
  }

  if (msg == L"MIXER_ORDER_REV") {
    // The token on its own. A client holding a list can ask whether it is
    // still current for the price of one short message, instead of pulling
    // every row back to find out.
    reply = Fmt(L"MIXER_ORDER_REV|rev=%s|count=%d", MixerOrderRev().c_str(),
                (int)MixerOrderKeys(this).size());
    return true;
  }

  if (msg.rfind(L"MIXER_ORDER_MOVE=", 0) == 0) {
    const std::vector<std::wstring> a = SplitArgs(msg.substr(17));
    if (a.size() < 3) { reply = L"MIXER_ERR|reason=malformed"; return true; }
    const std::wstring key = a[0] + L"|" + a[1];
    const int delta = _wtoi(a[2].c_str());

    // An optional fourth argument: the rev the client was looking at.
    //
    // Omitted, this behaves exactly as it always has -- an unguarded move, for
    // a caller that has just read the list or does not care. Supplied, the
    // move is refused when the arrangement has changed since, and the reply
    // carries the current token so the client can re-fetch and re-ask. A phone
    // is showing a list it fetched some time ago, and a device arriving or a
    // view flag being toggled in the window can make the row under the user's
    // finger a different fader than the one the PC would move.
    if (a.size() >= 4 && !a[3].empty() && a[3] != MixerOrderRev()) {
      reply = Fmt(L"MIXER_ERR|reason=stale|rev=%s", MixerOrderRev().c_str());
      return true;
    }

    // Start from the full addressable list, not the saved one: moving a fader
    // that has never been arranged has to work, and it is the common case the
    // first time anyone touches this.
    std::vector<std::wstring> keys = MixerOrderKeys(this);

    const auto it = std::find(keys.begin(), keys.end(), key);
    if (it == keys.end()) { reply = L"MIXER_ERR|reason=unknown_fader"; return true; }
    const int from = (int)(it - keys.begin());
    int to = from + delta;
    if (to < 0) to = 0;
    if (to >= (int)keys.size()) to = (int)keys.size() - 1;
    if (to != from) {
      std::swap(keys[from], keys[to]);
      MixerSetFaderOrder(keys);
    }
    // rev BEFORE key, because a key contains '|' and every reader takes the
    // key as the rest of the line. Appending after it fed the token into the
    // fader id and the existing Android client would have looked up
    // "sonar:aux|monitoring|rev=1a2b3c4d".
    reply = Fmt(L"MIXER_ORDER|pos=%d|rev=%s|key=%s", to,
                MixerOrderRev().c_str(), key.c_str());
    return true;
  }

  // ── Per-fader preferences (forgejo#50, forgejo#52) ──────────────────────
  //
  // Both key on "<channel>|<faderId>", exactly as MIXER_ORDER reports it, so a
  // client that has parsed one key has parsed all of them. A channel id may
  // contain ':' and '{}' but never '|', and neither does a fader id, so the
  // first two '|'-separated tokens ARE the key and everything after the second
  // is the value. Taking the value as "the rest" rather than as one field also
  // lets a short name contain a '|', and lets it be empty.
  auto splitPrefKey = [](const std::wstring& rest, std::wstring* key,
                         std::wstring* value) -> bool {
    const size_t first = rest.find(L'|');
    if (first == std::wstring::npos || first == 0) return false;
    const size_t second = rest.find(L'|', first + 1);
    if (second == std::wstring::npos) return false;
    *key = rest.substr(0, second);
    *value = rest.substr(second + 1);
    return true;
  };

  if (msg.rfind(L"MIXER_HIDE=", 0) == 0) {
    std::wstring key, value;
    if (!splitPrefKey(msg.substr(11), &key, &value) || value.empty()) {
      reply = L"MIXER_ERR|reason=malformed";
      return true;
    }
    const std::vector<std::wstring> t = SplitArgs(key);
    // Refused rather than stored when nothing answers to the key. The stored
    // preference is keyed by string and deliberately survives a device being
    // switched off -- but SETTING one for a fader that does not exist is a
    // typo far more often than it is intent, and a silently stored typo is
    // invisible until someone reads the file.
    if (t.size() != 2 || !FaderExists(t[0], t[1])) {
      reply = L"MIXER_ERR|reason=unknown_fader";
      return true;
    }
    const bool hide = (value != L"0");
    MixerCfg().SetHidden(key, hide);
    MixerCfg().Save();
    // Hiding is a VIEW preference and touches nothing else: the fader keeps
    // its level and its mute state, keeps its place in the order so revealing
    // puts it back where it was, and still answers every write verb --
    // including through a volume-hotkey group that points at it. A group can
    // legitimately name a fader the user has hidden, and hiding must not
    // quietly stop that key working (forgejo#50).
    reply = Fmt(L"MIXER_HIDDEN|hidden=%d|key=%s", hide ? 1 : 0, key.c_str());
    return true;
  }

  if (msg == L"MIXER_HIDDEN") {
    // The hidden set on its own, so a client rendering a "show hidden" toggle
    // does not have to walk the whole state to populate it.
    for (const MixerFaderPref& fp : MixerCfg().Get().faderPrefs) {
      if (!fp.hidden) continue;
      if (!reply.empty()) reply += L"\n";
      reply += Fmt(L"MIXER_HIDDEN|key=%s", fp.key.c_str());
    }
    reply += (reply.empty() ? L"" : L"\n");
    reply += Fmt(L"MIXER_HIDDEN_END|rev=%s", MixerOrderRev().c_str());
    return true;
  }

  if (msg.rfind(L"MIXER_FADER_NAME=", 0) == 0) {
    std::wstring key, name;
    if (!splitPrefKey(msg.substr(17), &key, &name)) {
      reply = L"MIXER_ERR|reason=malformed";
      return true;
    }
    const std::vector<std::wstring> t = SplitArgs(key);
    if (t.size() != 2 || !FaderExists(t[0], t[1])) {
      reply = L"MIXER_ERR|reason=unknown_fader";
      return true;
    }
    // An empty name CLEARS rather than storing an empty label, so a fader goes
    // back to its default without a second verb (forgejo#52). Renaming touches
    // nothing else -- not the order, not the level.
    MixerCfg().SetShortName(key, name);
    MixerCfg().Save();
    reply = Fmt(L"MIXER_FADER_NAME|short=%s|key=%s", name.c_str(), key.c_str());
    return true;
  }

  if (msg.rfind(L"MIXER_ORDER_SET=", 0) == 0) {
    // A whole arrangement in one message, which is what a phone that lets you
    // DRAG rows actually produces -- expressing a drag as a run of swaps would
    // write the intermediate states to disk and leave a half-applied order
    // behind if the connection dropped mid-way.
    //
    // Guarded by the same token, and here it is not optional: a client sending
    // a complete list is asserting what the whole order should be, so it has
    // to be a list built from what the PC currently holds. Without that,
    // arranging on a phone whose view was minutes old would silently discard
    // any change made at the PC in between.
    // NOT SplitArgs. Every key is itself "<channel>|<fader>", so splitting the
    // whole payload on '|' would shred the list -- only the FIRST separator is
    // an argument boundary here. The keys after it are comma-separated for the
    // same reason: a comma appears in no key, and a bar appears in all of them.
    const std::wstring payload = msg.substr(16);
    const size_t bar = payload.find(L'|');
    if (bar == std::wstring::npos) {
      reply = L"MIXER_ERR|reason=malformed";
      return true;
    }
    if (payload.substr(0, bar) != MixerOrderRev()) {
      reply = Fmt(L"MIXER_ERR|reason=stale|rev=%s", MixerOrderRev().c_str());
      return true;
    }

    std::vector<std::wstring> want;
    {
      const std::wstring csv = payload.substr(bar + 1);
      for (size_t start = 0, i = 0; i <= csv.size(); i++) {
        if (i != csv.size() && csv[i] != L',') continue;
        const std::wstring k = csv.substr(start, i - start);
        start = i + 1;
        if (!k.empty()) want.push_back(k);
      }
    }
    if (want.empty()) { reply = L"MIXER_ERR|reason=malformed"; return true; }

    // Every key must be one this machine knows, and none may repeat. A typo
    // would otherwise be stored as a fader that will never exist, and a
    // duplicate would draw one channel twice under two headings -- the split
    // channel MoveOrderGroupKeys exists to repair.
    const std::vector<std::wstring> known = MixerOrderKeys(this);
    std::set<std::wstring> seen;
    for (const std::wstring& k : want) {
      if (std::find(known.begin(), known.end(), k) == known.end()) {
        reply = L"MIXER_ERR|reason=unknown_fader";
        return true;
      }
      if (!seen.insert(k).second) {
        reply = L"MIXER_ERR|reason=duplicate";
        return true;
      }
    }

    // Anything the client left out keeps its relative order, appended. A
    // partial list is not treated as a deletion: MixerSetFaderOrder already
    // holds a place for a device that is merely switched off, and the same
    // courtesy is owed to a client that only sent the rows it was showing.
    for (const std::wstring& k : known)
      if (!seen.count(k)) want.push_back(k);

    MixerSetFaderOrder(want);
    reply = Fmt(L"MIXER_ORDER_SET|count=%d|rev=%s", (int)want.size(),
                MixerOrderRev().c_str());
    return true;
  }

  if (msg == L"MIXER_GROUPS") {
    // Every group, one line each, so a client learns how many there are from
    // the same call that tells it what they hold.
    for (int i = 0; i < MixerGroupCount(); i++) {
      if (!reply.empty()) reply += L"\n";
      reply += MixerGroupLine(i);
    }
    if (reply.empty()) reply = L"MIXER_ERR|reason=no_groups";
    return true;
  }

  // MIXER_GROUP_SET=<n>|<ch>|<fader>|<ch>|<fader>...  replaces the membership
  // wholesale. Replace rather than add/remove: the window ticks boxes and
  // sends what is ticked, and an incremental verb would drift out of step with
  // it the moment one update went missing.
  if (msg.rfind(L"MIXER_GROUP_SET=", 0) == 0) {
    const std::vector<std::wstring> a = SplitArgs(msg.substr(16));
    if (a.empty()) { reply = L"MIXER_ERR|reason=malformed"; return true; }
    const int group = GroupIndex(a[0]);
    if (group < 0 || group >= MixerGroupCount()) {
      reply = L"MIXER_ERR|reason=unknown_group";
      return true;
    }
    // Pairs, so an odd tail is a malformed message rather than a half-named
    // fader silently dropped.
    if (((a.size() - 1) % 2) != 0) { reply = L"MIXER_ERR|reason=malformed"; return true; }
    std::vector<std::wstring> members;
    for (size_t i = 1; i + 1 < a.size(); i += 2) {
      if (a[i].empty() || a[i + 1].empty()) continue;
      members.push_back(a[i] + L"|" + a[i + 1]);
    }
    MixerSetGroupMembers(group, members);
    reply = MixerGroupLine(group);
    return true;
  }

  if (msg.rfind(L"MIXER_GROUP_NUDGE=", 0) == 0) {
    const std::vector<std::wstring> a = SplitArgs(msg.substr(18));
    if (a.size() < 2) { reply = L"MIXER_ERR|reason=malformed"; return true; }
    const int group = GroupIndex(a[0]);
    if (group < 0 || group >= MixerGroupCount()) {
      reply = L"MIXER_ERR|reason=unknown_group";
      return true;
    }
    if (!MixerIsEnabled()) { reply = L"MIXER_ERR|reason=disabled"; return true; }
    const int moved = MixerNudgeGroup(group, (float)_wtof(a[1].c_str()));
    if (moved <= 0) { reply = L"MIXER_ERR|reason=nothing_present"; return true; }
    // The count, not the levels: the members are at different values by
    // design, so there is no single number to report. A client that wants them
    // reads MIXER_FADERS, which the write broadcasts anyway.
    reply = MixerGroupLine(group, Fmt(L"|moved=%d", moved));
    return true;
  }

  if (msg.rfind(L"MIXER_GROUP_MUTE=", 0) == 0) {
    const std::vector<std::wstring> a = SplitArgs(msg.substr(17));
    const int group = a.empty() ? -1 : GroupIndex(a[0]);
    if (group < 0 || group >= MixerGroupCount()) {
      reply = L"MIXER_ERR|reason=unknown_group";
      return true;
    }
    if (!MixerIsEnabled()) { reply = L"MIXER_ERR|reason=disabled"; return true; }
    const int changed = MixerToggleGroupMute(group);
    if (changed <= 0) { reply = L"MIXER_ERR|reason=cannot_mute"; return true; }
    reply = MixerGroupLine(group, Fmt(L"|changed=%d", changed));
    return true;
  }

  if (msg == L"MIXER_SONAR_RESTART") {
    // Over IPC the remote guard is a two-step: the first call opens the window
    // and answers pending, a second inside it performs the restart. That makes
    // ConfirmRemote mean something on the wire rather than being advice a
    // client may ignore.
    if (MixerConfirmRequired(2) && !MixerConfirmPending()) {
      g_confirm.pending = true;
      g_confirm.what = L"restart SteelSeries";
      g_confirm.askedAt = GetTickCount();
      g_confirm.lastOutcome = L"none";
      reply = L"MIXER_SONAR_RESTART|pending=1|reason=confirm_required";
      return true;
    }
    if (MixerConfirmPending()) MixerConfirmAnswer(true);

    std::wstring detail;
    const bool ok = RestartSonar(detail);
    DebugLogW(ok ? L"SteelSeries restarted"
                 : L"SteelSeries restart incomplete", ok ? LOG_INFO : LOG_ERROR);
    // The Sonar provider's port moves with a restart, so drop what we knew and
    // let the next request rediscover rather than reporting a dead port.
    if (g_mixer) g_mixer->RefreshNow();
    reply = Fmt(L"MIXER_SONAR_RESTARTED|ok=%d|detail=%s", ok ? 1 : 0,
                detail.empty() ? L"-" : detail.c_str());
    return true;
  }

  if (msg == L"MIXER_CONFIRM_STATUS") {
    reply = Fmt(L"MIXER_CONFIRM|hotkey=%d|toolwindow=%d|remote=%d"
                L"|pending=%d|lastoutcome=%s|remotelocked=%d",
                MixerCfg().Get().confirmHotkeySeconds,
                MixerCfg().Get().confirmToolWindow ? 1 : 0,
                MixerCfg().Get().confirmRemote ? 1 : 0,
                g_confirm.pending ? 1 : 0, g_confirm.lastOutcome.c_str(),
                RemoteUnlockable() ? 0 : 1);
    return true;
  }

  if (msg.rfind(L"MIXER_CONFIRM_SET=", 0) == 0) {
    const std::vector<std::wstring> a = SplitArgs(msg.substr(18));
    if (a.size() < 2) { reply = L"MIXER_ERR|reason=malformed"; return true; }
    const int value = _wtoi(a[1].c_str());
    if (a[0] == L"hotkey") {
      MixerCfg().Get().confirmHotkeySeconds = value < 0 ? 0 : value;
      MixerCfg().Save();
    } else if (a[0] == L"toolwindow") {
      MixerCfg().Get().confirmToolWindow = value != 0;
      MixerCfg().Save();
    } else if (a[0] == L"remote") {
      if (value == 0 && !RemoteUnlockable()) {
        // Say why. Ignoring it silently would read as a setting that does not
        // work, rather than one that is deliberately gated.
        reply = L"MIXER_CONFIRM|remote=1|locked=1|reason=pin_required";
        return true;
      }
      MixerCfg().Get().confirmRemote = value != 0;
      MixerCfg().Save();
    } else {
      reply = L"MIXER_ERR|reason=unknown_surface";
      return true;
    }
    reply = Fmt(L"MIXER_CONFIRM|hotkey=%d|toolwindow=%d|remote=%d",
                MixerCfg().Get().confirmHotkeySeconds,
                MixerCfg().Get().confirmToolWindow ? 1 : 0,
                MixerCfg().Get().confirmRemote ? 1 : 0);
    return true;
  }

  if (msg.rfind(L"MIXER_CONFIRM_BEGIN=", 0) == 0) {
    const bool asked = MixerConfirmBegin(msg.substr(20));
    reply = Fmt(L"MIXER_CONFIRM|pending=%d", asked ? 1 : 0);
    return true;
  }

  if (msg.rfind(L"MIXER_CONFIRM_ANSWER=", 0) == 0) {
    const bool go = MixerConfirmAnswer(msg.substr(21) != L"0");
    reply = Fmt(L"MIXER_CONFIRM|proceed=%d", go ? 1 : 0);
    return true;
  }

  if (msg == L"DIAG_MIXER") {
    MixerSnapshot snap;
    if (MixerIsEnabled()) snap = g_mixer->Snapshot();
    reply = Fmt(L"DIAG_MIXER|enabled=%d|channels=%d|routes=%d|subscribers=%d",
                MixerIsEnabled() ? 1 : 0, (int)snap.channels.size(),
                (int)snap.routes.size(), g_subscribers);
    return true;
  }

  return false;  // not ours: fall through to the rest of the chain
}

bool Engine::MixerConfirmRequired(int surface) const {
  return ConfirmSecondsFor(surface) > 0;
}

bool Engine::MixerConfirmBegin(const std::wstring& what) {
  if (ConfirmSecondsFor(0) <= 0) return false;   // nothing to ask; act now
  g_confirm.pending = true;
  g_confirm.what = what;
  g_confirm.askedAt = GetTickCount();
  g_confirm.lastOutcome = L"none";
  AddNotification((wchar_t*)L"Press again to confirm");
  return true;
}

bool Engine::MixerConfirmAnswer(bool yes) {
  // An expired window answers no, whatever the caller says. Proceeding here
  // would be the delayed firing the whole design exists to prevent.
  if (!g_confirm.pending) return false;
  g_confirm.pending = false;
  g_confirm.lastOutcome = yes ? L"confirmed" : L"dropped";
  return yes;
}

bool Engine::MixerConfirmPending() const { return g_confirm.pending; }

void Engine::MixerConfirmTick() {
  if (!g_confirm.pending) return;
  const unsigned windowMs = (unsigned)ConfirmSecondsFor(0) * 1000u;
  if (windowMs == 0 || (GetTickCount() - g_confirm.askedAt) < windowMs) return;
  // Fails closed. A guard that fires after its window has passed would land
  // the action at a moment nobody asked for, which is worse than no guard.
  g_confirm.pending = false;
  g_confirm.lastOutcome = L"dropped";
}

void Engine::MixerLoadFailoverConfig() {
  if (!g_mixer) return;
  for (const RouteTarget& r : g_mixer->Snapshot().routes)
    g_failover.SetRule(LoadRule(r.id));
}

bool Engine::MixerHandleAndReply(const wchar_t* message) {
  // MIXER_STATE answers in CHUNKS, and the size is load-bearing in both
  // directions. One combined reply overruns the 8192-byte read the probe
  // harness does, and the caller silently keeps only the tail -- an answer that
  // parses cleanly while missing most of the state. One message per record goes
  // wrong the other way: two dozen back-to-back replies through a
  // request/response pipe lose records, which was measured, not feared.
  //
  // So: pack whole records into messages of at most kChunkChars, which is
  // comfortably inside the 4096-wide-character read and keeps the whole state
  // to two or three messages.
  if (message && std::wstring(message) == L"MIXER_STATE") {
    const size_t kChunkChars = 3000;
    std::wstring chunk;
    for (const std::wstring& line : MixerStateLines()) {
      if (!chunk.empty() && chunk.size() + line.size() + 1 > kChunkChars) {
        SendMessageToMDropDX12Remote(chunk.c_str(), true);
        chunk.clear();
      }
      if (!chunk.empty()) chunk += L"\n";
      chunk += line;
    }
    if (!chunk.empty()) SendMessageToMDropDX12Remote(chunk.c_str(), true);
    return true;
  }

  std::wstring reply;
  if (!MixerHandleCommand(message, reply)) return false;
  if (!reply.empty()) SendMessageToMDropDX12Remote(reply.c_str(), true);
  return true;
}


// ── The headset battery on the HUD ────────────────────────────────────────

int Engine::MixerBatteryPercent() const {
  return g_hudBattery.load(std::memory_order_relaxed);
}

void Engine::MixerBatteryRecompute() {
  int pick = -1;
  std::wstring from;
  const wchar_t* how = L"none";
  const int sim = g_hudBatterySim.load(std::memory_order_relaxed);
  if (sim != -2) {
    g_hudBattery.store(sim, std::memory_order_relaxed);
    std::lock_guard<std::mutex> lock(g_hudBatteryMutex);
    g_hudBatteryDevice.clear();
    g_hudBatterySource = L"sim";
    return;
  }
  if (g_watcher) {
    std::vector<std::wstring> ids;
    {
      std::lock_guard<std::mutex> lock(g_hudBatteryMutex);
      ids = g_hudBatteryIds;
    }
    for (const std::wstring& id : ids) {
      pick = g_watcher->BatteryFor(id);
      if (pick >= 0) { from = id; how = L"route"; break; }
    }
    if (pick < 0) {
      // Read live rather than cached with the route ids: it is one string
      // copy, and the default device can move without any route moving.
      const std::wstring def = g_watcher->DefaultRenderId();
      if (!def.empty()) pick = g_watcher->BatteryFor(def);
      if (pick >= 0) { from = def; how = L"default"; }
    }
  }
  g_hudBattery.store(pick, std::memory_order_relaxed);
  {
    std::lock_guard<std::mutex> lock(g_hudBatteryMutex);
    g_hudBatteryDevice.swap(from);
    g_hudBatterySource = how;
  }
}

void Engine::MixerBatteryTick() {
  if (!m_bShowBattery || !g_watcher) return;
  const DWORD now = GetTickCount();
  const DWORD last = g_hudBatteryLast.load();
  // Unsigned arithmetic, so the 49-day wrap costs one early read rather than
  // a poll that never fires again.
  if (last != 0 && (now - last) < 30000u) return;
  g_hudBatteryLast.store(now);
  g_watcher->RefreshBattery();
  MixerBatteryRecompute();
}

void Engine::SetShowBattery(bool show) {
  const bool wasOn = m_bShowBattery;
  m_bShowBattery = show;
  if (!show || wasOn) return;
  // Due immediately, so the first frame after the toggle carries a figure
  // read just now rather than one from up to thirty seconds ago.
  g_hudBatteryLast.store(0);
  // An empty corner is what a broken feature looks like, and with the mixer
  // subsystem off there is no device list to read at all -- so say which it
  // is. A headset that simply reports no charge is ordinary and stays quiet.
  if (!MixerIsEnabled())
    AddNotification(L"Battery: the audio mixer is off");
}

}  // namespace mdrop
