/*
  Plugin module: IPC Command Index
  The self-documenting HELP/IDENTIFY surface of the named pipe.
  Contains: The IpcCommandDoc tables (Commands, ScriptCommands,
            SignalCommands), the feature list + identity self-report,
            index/detail/JSON reply builders (BuildIndex, BuildDetail,
            BuildJson), the Handle entry point, string helpers
            (EqualsNoCase, NameMatches, JsonEscape)
*/

#include "ipc_help.h"
#include "version.h"
#include "format_to.h"
#include "utility.h"   // BuildTimestampW -- the exe's own time, not __DATE__

#include <windows.h>
#include <string>
#include <vector>
#include <cwchar>
#include <cwctype>

namespace {

// Every keyword Engine::LaunchMessage matches explicitly. `match` is the
// literal the dispatcher compares against, character for character, so
// test_ipc_help.py can diff this table against the source and fail on drift.
const IpcCommandDoc kCommands[] = {

  // -- this index itself (dispatched by ipc_help::Handle, not the else-if
  //    chain in LaunchMessage) --------------------------------------------
  { L"HELP",                   L"HELP[=<command|group|JSON>]",     L"This index; with an argument, one entry, one group, or the whole thing as JSON", L"help" },
  { L"COMMANDS",               L"COMMANDS",                        L"Alias for HELP", L"help" },
  { L"LIST_COMMANDS",          L"LIST_COMMANDS",                   L"Alias for HELP", L"help" },
  { L"GET_VERSION",            L"GET_VERSION",                     L"Version, build, configuration, protocol and pid of this instance", L"help" },
  { L"VERSION",                L"VERSION",                         L"Alias for GET_VERSION; matched exactly, so the VERSION= script command still works", L"help" },

  // -- preset --------------------------------------------------------------
  { L"PRESET=",                L"PRESET=<path>",                   L"Load a preset; path is absolute or relative to the base directory", L"preset" },
  { L"CLEARPRESET",            L"CLEARPRESET",                     L"Clear the current preset state", L"preset" },
  { L"QUICKSAVE",              L"QUICKSAVE",                       L"Save the running preset into the Quicksave folder", L"preset" },
  { L"SET_DIR=",               L"SET_DIR=<path>",                  L"Change the preset directory; replies SET_DIR_RESULT=OK|<path>", L"preset" },
  { L"ENUM_LISTS",             L"ENUM_LISTS",                      L"List saved preset lists; replies ENUM_LISTS_RESULT=a|b|...", L"preset" },
  { L"LOAD_LIST=",             L"LOAD_LIST=<name>",                L"Load a saved preset list by name, without the .txt extension", L"preset" },
  { L"CLEAR_LIST",             L"CLEAR_LIST",                      L"Drop the active preset list and go back to scanning the directory", L"preset" },
  { L"LINK=",                  L"LINK=<index>",                    L"Point Milkwave Remote at preset index N", L"preset" },
  { L"GET_PRESET_STATUS",      L"GET_PRESET_STATUS",               L"One-message preset and load status: file, desc, loading, ready, shadertoy buffers", L"preset" },
  { L"GET_PRESET_HASH",        L"GET_PRESET_HASH",                 L"Content-hash identity of the running preset", L"preset" },
  { L"SET_PRESET_ORDER=",      L"SET_PRESET_ORDER=<0|1>",          L"Set preset order absolutely: 0 random, 1 sequential (RAND toggles instead)", L"preset" },
  { L"SET_PRESET_LOCK=",       L"SET_PRESET_LOCK=<0|1>",           L"Lock or unlock the preset absolutely (LOCK toggles instead)", L"preset" },
  { L"SET_TIME_BETWEEN_PRESETS=", L"SET_TIME_BETWEEN_PRESETS=<sec>", L"Seconds between automatic preset changes; 0 disables cycling", L"preset" },
  { L"GET_PRESET_CYCLE",       L"GET_PRESET_CYCLE",                L"Preset order, lock, interval and interval randomness", L"preset" },

  // -- annotations (presets.json) -------------------------------------------
  { L"RELOAD_ANNOTATIONS",     L"RELOAD_ANNOTATIONS",              L"Re-read presets.json from disk", L"annot" },
  { L"ANNOT_REMOVE_MISSING",   L"ANNOT_REMOVE_MISSING",            L"Purge entries whose preset file no longer exists", L"annot" },
  { L"GET_PRESET_RATING",      L"GET_PRESET_RATING",               L"Effective rating, and whether it came from this install or the file's fRating", L"annot" },
  { L"SET_PRESET_RATING=",     L"SET_PRESET_RATING=<0-5>",         L"Rate the running preset; written to presets.json, never to the .milk", L"annot" },
  { L"GET_PRESET_NOTE",        L"GET_PRESET_NOTE",                 L"The running preset's note text, empty if it has none", L"annot" },
  { L"SET_PRESET_NOTE=",       L"SET_PRESET_NOTE=<text>",          L"Set the running preset's note; keyed by content hash so it survives a rename", L"annot" },
  { L"SET_PRESET_FLAG=",       L"SET_PRESET_FLAG=<name>,<0|1>",    L"Set or clear one flag on the running preset; names match presets.json", L"annot" },
  { L"PRESET_OVERRIDE_STATUS", L"PRESET_OVERRIDE_STATUS",          L"What each per-preset slot resolved to, and whether a rule or the preset won", L"annot" },
  { L"SET_PRESET_SHADER_OVERRIDE=", L"SET_PRESET_SHADER_OVERRIDE=<name>", L"Pin a shader override to this preset; an empty value means explicitly none", L"annot" },
  { L"SET_PRESET_VFX_PROFILE=", L"SET_PRESET_VFX_PROFILE=<name>",  L"Pin a VFX profile to this preset; an empty value means explicitly none", L"annot" },
  { L"CLEAR_PRESET_OVERRIDE=", L"CLEAR_PRESET_OVERRIDE=<slot>",    L"Remove a per-preset slot entirely, so tag rules apply again", L"annot" },
  { L"DIAG_ANNOT_DUPES",       L"DIAG_ANNOT_DUPES",                L"Hash every preset under the preset dir; replies groups, redundant, hashes", L"annot" },
  { L"DIAG_ANNOT_IGNORED=",    L"DIAG_ANNOT_IGNORED=<path>",       L"Would a preset there be recorded? Replies ignored, dirmatch, testing", L"annot" },
  { L"DIAG_ANNOT_MISSING",     L"DIAG_ANNOT_MISSING",              L"Count entries whose preset file cannot be found", L"annot" },
  { L"DIAG_ANNOT_QUERY=",      L"DIAG_ANNOT_QUERY=<pattern>",      L"Count entries matching the Annotations search box's own predicate", L"annot" },
  { L"DIAG_ANNOT_REPAIR",      L"DIAG_ANNOT_REPAIR",               L"Drop alias paths that hash to a different preset than the entry holding them", L"annot" },
  { L"BROWSE_PRESETS=",        L"BROWSE_PRESETS=<offset>,<count>[,q=][,folder=][,tag=][,flag=][,minrating=][,sort=name|rating|used|time|lastused]", L"Page the annotation database; one PRESET_ROW| per entry then PRESET_ROWS_END with the filtered total", L"annot" },
  { L"GET_PRESET_FOLDERS",     L"GET_PRESET_FOLDERS",              L"Folders presets live in, with counts; a preset in two places is counted in both", L"annot" },
  { L"GET_PRESET_TAGS",        L"GET_PRESET_TAGS",                 L"Tags in use, with counts", L"annot" },
  { L"SET_PRESET_RATING_FOR=", L"SET_PRESET_RATING_FOR=<hash>,<0-5>", L"Rate a preset that is not running, by content hash", L"annot" },
  { L"SET_PRESET_FLAG_FOR=",   L"SET_PRESET_FLAG_FOR=<hash>,<name>,<0|1>", L"Flag a preset that is not running; favorite|error|skip|broken|canvas", L"annot" },
  { L"SET_PRESET_TAG_FOR=",    L"SET_PRESET_TAG_FOR=<hash>,<name>,<0|1>", L"Add or remove a tag on a preset that is not running", L"annot" },
  { L"SET_PRESET_NOTE_FOR=",   L"SET_PRESET_NOTE_FOR=<hash>,<text>", L"Set the note on a preset that is not running, by content hash", L"annot" },
  { L"GET_PRESET_NOTE_FOR=",   L"GET_PRESET_NOTE_FOR=<hash>",      L"A preset's note text by content hash, empty if it has none", L"annot" },
  { L"DIAG_ANNOT_RESOLVE=",    L"DIAG_ANNOT_RESOLVE=<file>",       L"Where an annotation's preset actually lives; an empty path means gone", L"annot" },

  // -- canvas and feedback mitigation ---------------------------------------
  { L"SET_CANVAS_MAX=",        L"SET_CANVAS_MAX=<px>",             L"Global feedback-canvas ceiling, long edge in pixels; 0 means no limit", L"canvas" },
  { L"GET_CANVAS_MAX",         L"GET_CANVAS_MAX",                  L"Global ceiling, the canvas actually in use, and what this preset asks for", L"canvas" },
  { L"SET_PRESET_CANVAS_MAX=", L"SET_PRESET_CANVAS_MAX=<px>",      L"Per-preset canvas limit for the running preset, capped by the global ceiling", L"canvas" },
  { L"PRESET_CANVAS_MAX_CLEAR", L"PRESET_CANVAS_MAX_CLEAR",        L"Clear the running preset's canvas limit", L"canvas" },
  { L"SET_PRESET_FPS_MAX=",    L"SET_PRESET_FPS_MAX=<fps>",        L"Per-preset frame cap for the running preset. It only ever LOWERS the active cap, so a value above the user's own cap does nothing; 0 clears it", L"canvas" },
  { L"PRESET_FPS_MAX_CLEAR",   L"PRESET_FPS_MAX_CLEAR",            L"Clear the running preset's frame cap", L"canvas" },
  { L"GET_PRESET_FPS_MAX",     L"GET_PRESET_FPS_MAX",              L"The running preset's own cap and the cap actually in force -- PRESET_FPS_MAX=<n>|active=<n>", L"canvas" },
  { L"SET_PRESET_DAMP=",       L"SET_PRESET_DAMP=<0..1>",          L"Feedback damp strength for the running preset; 0 clears it", L"canvas" },
  { L"PRESET_DAMP_CLEAR",      L"PRESET_DAMP_CLEAR",               L"Clear the running preset's feedback damp", L"canvas" },
  { L"SET_DAMP_OVERRIDE=",     L"SET_DAMP_OVERRIDE=<mult|clear>",  L"Raw per-frame feedback multiplier for this session only; nothing is persisted", L"canvas" },
  { L"SET_CANVAS_TRACE=",      L"SET_CANVAS_TRACE=<0|1>",          L"Write per-frame canvas metrics to log/diag_canvas_trace.csv", L"canvas" },
  { L"DIAG_CANVAS_METRIC",     L"DIAG_CANVAS_METRIC",              L"Mean luminance of an 8x8 reduction of the presented frame", L"canvas" },

  // -- shaders ---------------------------------------------------------------
  { L"SHADER_IMPORT=",         L"SHADER_IMPORT=<path>",            L"Load a shader_import JSON file, convert every pass GLSL to HLSL and apply it", L"shader" },
  { L"SHADER_GLSL=",           L"SHADER_GLSL=<source>",            L"Convert and apply inline GLSL as a single Image pass", L"shader" },
  { L"SHADER_CONVERT=",        L"SHADER_CONVERT=<source>",         L"Convert GLSL to HLSL and return it without applying anything", L"shader" },
  { L"SHADER_SAVE=",           L"SHADER_SAVE=<path>",              L"Save the imported shader passes as .milk3 or .milk", L"shader" },
  { L"SHADER_OVERRIDE_APPLY=", L"SHADER_OVERRIDE_APPLY=<name>",    L"Apply a shader override to the running preset, no tag rule needed", L"shader" },
  { L"SHADER_OVERRIDE_REVERT", L"SHADER_OVERRIDE_REVERT",          L"Restore the preset's own shaders", L"shader" },
  { L"SHADER_OVERRIDE_ENABLE=", L"SHADER_OVERRIDE_ENABLE=<0|1>",   L"Master enable for tag-selected shader overrides", L"shader" },
  { L"SHADER_OVERRIDE_RELOAD", L"SHADER_OVERRIDE_RELOAD",          L"Re-read shaderoverrides.json and every shader file", L"shader" },
  { L"SHADER_OVERRIDE_STATUS", L"SHADER_OVERRIDE_STATUS",          L"What resolved, from which rule or tag, and any compile failure", L"shader" },
  { L"DUMP_SHADER",            L"DUMP_SHADER",                     L"Dump the running shader sources to files under log/", L"shader" },

  // -- video effects ---------------------------------------------------------
  { L"SET_VFX=",               L"SET_VFX=<Name>=<value>",          L"Set one video-effect parameter; an unknown name replies VFX_SET=ERROR|unknown", L"vfx" },
  { L"GET_VFX",                L"GET_VFX",                         L"Dump every video-effect parameter and its value", L"vfx" },
  { L"GET_VFX_STATUS",         L"GET_VFX_STATUS",                  L"Which profile is loaded, and whether it has unsaved changes", L"vfx" },
  { L"VFX_RESET=",             L"VFX_RESET=<transform|effects|audio|all>", L"Reset one group of video-effect parameters to defaults", L"vfx" },
  { L"VFX_PROFILE_LIST",       L"VFX_PROFILE_LIST",                L"List saved video-effect profile names", L"vfx" },
  { L"VFX_PROFILE_LOAD=",      L"VFX_PROFILE_LOAD=<name>",         L"Load a video-effect profile", L"vfx" },
  { L"VFX_PROFILE_SAVE=",      L"VFX_PROFILE_SAVE=<name>",         L"Save the current parameters under that profile name, replacing it if it exists", L"vfx" },
  { L"VFX_PROFILE_IMPORT=",    L"VFX_PROFILE_IMPORT=<path>[|<name>]", L"Import profiles from a file this build did not write", L"vfx" },
  { L"VFX_SCOPED_KEEP=",       L"VFX_SCOPED_KEEP=<0|1>",           L"Answer a pending scoped-edit prompt without clicking it", L"vfx" },
  { L"VFX_SCOPED_STATUS",      L"VFX_SCOPED_STATUS",               L"Whether a scoped VFX edit is active, its profile, and whether it is dirty", L"vfx" },

  // -- audio -----------------------------------------------------------------
  { L"DEVICE=",                L"DEVICE=[IN|]<name>",              L"Switch the capture device; IN| selects an input rather than a loopback", L"audio" },
  { L"GET_AUDIO_DEVICES",      L"GET_AUDIO_DEVICES",               L"Enumerate active WASAPI render devices", L"audio" },
  { L"SET_DEVICE_VOLUME=",     L"SET_DEVICE_VOLUME=<0.0-1.0>",     L"Set the Windows device volume", L"audio" },
  { L"GET_DEVICE_VOLUME",      L"GET_DEVICE_VOLUME",               L"Query device volume and mute state", L"audio" },
  { L"SET_DEVICE_MUTE=",       L"SET_DEVICE_MUTE=<0|1>",           L"Mute or unmute the audio device", L"audio" },
  { L"TOGGLE_DEVICE_MUTE",     L"TOGGLE_DEVICE_MUTE",              L"Toggle the audio device's mute state", L"audio" },
  { L"MIXER_STATE",            L"MIXER_STATE",                     L"Every mixer channel, fader, route and device", L"audio" },
  { L"MIXER_ENABLE=",          L"MIXER_ENABLE=<0|1>",              L"Turn the audio mixer subsystem on or off (default off)", L"audio" },
  { L"MIXER_SUBSCRIBE=",       L"MIXER_SUBSCRIBE=<0|1>",           L"Ask for live mixer updates; gates all provider polling", L"audio" },
  { L"MIXER_SET=",             L"MIXER_SET=<ch>|<fader>|<0.0-1.0>", L"Set one fader's volume", L"audio" },
  { L"MIXER_MUTE=",            L"MIXER_MUTE=<ch>|<fader>|<0|1|toggle>", L"Mute, unmute or toggle one fader", L"audio" },
  { L"DIAG_MIXER",             L"DIAG_MIXER",                      L"Mixer subsystem state, for the probe harness", L"audio" },
  { L"DIAG_GLOBAL_PRESET_CYCLE", L"DIAG_GLOBAL_PRESET_CYCLE",      L"The preset cycle interval the shutdown save would write, which a loaded display profile must not change", L"presets" },
  { L"DIAG_STARTUP_PRESET_LOCK", L"DIAG_STARTUP_PRESET_LOCK",      L"The 'Preset Lock on Startup' preference, which toggling the live lock must not change", L"presets" },
  { L"MIXER_ROUTE_ARM=",       L"MIXER_ROUTE_ARM=<route>|<0|1>",   L"Opt a route in or out of failover (off by default)", L"audio" },
  { L"MIXER_ROUTE_SET=",       L"MIXER_ROUTE_SET=<route>|<deviceId>", L"Point a route at a device now; cancels any pending failover", L"audio" },
  { L"MIXER_ALLOW_ADD=",       L"MIXER_ALLOW_ADD=<route>|<deviceId>|[name]", L"Append a device to a route's ordered failover allowlist; name lists one that is not connected", L"audio" },
  { L"MIXER_ALLOW_REMOVE=",    L"MIXER_ALLOW_REMOVE=<route>|<deviceId>", L"Remove a device from a route's allowlist", L"audio" },
  { L"MIXER_ALLOW_MOVE=",      L"MIXER_ALLOW_MOVE=<route>|<deviceId|name>|<delta>", L"Reorder a route's allowlist; the order is the preference, first present entry wins", L"audio" },
  { L"MIXER_ALLOW",            L"MIXER_ALLOW[=<route>]",           L"One MIXER_ALLOW| row per allowed replacement, in priority order, with present and last seen", L"audio" },
  { L"DIAG_FAILOVER",          L"DIAG_FAILOVER[=<route>]",         L"Failover state for a route; bare reports endpoint:default-render", L"audio" },
  { L"MIXER_SIM_DEVICE=",      L"MIXER_SIM_DEVICE=<id>|<present|gone|current|forget>", L"Testing mode only: drive the failover watcher without hardware", L"audio" },
  { L"MIXER_CONFIRM_STATUS",   L"MIXER_CONFIRM_STATUS",            L"Confirmation settings per surface, and any pending window", L"audio" },
  { L"MIXER_CONFIRM_SET=",     L"MIXER_CONFIRM_SET=<hotkey|toolwindow|remote>|<value>", L"Set a surface's confirmation; remote needs a PIN to relax", L"audio" },
  { L"MIXER_CONFIRM_BEGIN=",   L"MIXER_CONFIRM_BEGIN=<what>",      L"Open a confirm window; replies pending=0 when none is wanted", L"audio" },
  { L"MIXER_CONFIRM_ANSWER=",  L"MIXER_CONFIRM_ANSWER=<0|1>",      L"Answer a pending confirm window", L"audio" },
  { L"UI_LIST",                L"UI_LIST",                          L"Every tool window you can name, and whether it is open", L"window" },
  { L"UI_CLOSE=",              L"UI_CLOSE=<window>",                L"Close a tool window by name", L"window" },
  { L"UI_CLICK=",              L"UI_CLICK=<window>|ctrl=ID",        L"Press a control in a tool window", L"window" },
  { L"UI_MOVE=",               L"UI_MOVE=<window>|render",          L"Move a tool window; 'render' centres it on the visualiser's display", L"window" },
  { L"UI_SHOW=",               L"UI_SHOW=<window>[|tab=N][|ctrl=ID]", L"Open a tool window by its hotkey name, at a page, on a control", L"window" },
  { L"MIXER_RENAME_DEVICE=",   L"MIXER_RENAME_DEVICE=<id>|<name>",  L"Rename an audio endpoint, as the Sound settings page does", L"audio" },
  { L"SET_PIPE_NAME=",         L"SET_PIPE_NAME=<base>",            L"Testing only: serve the pipe under another namespace, so a harness instance is distinguishable", L"system" },
  { L"MIXER_CFG_FILE=",       L"MIXER_CFG_FILE=<path>",           L"Testing only: point the mixer settings at a scratch json", L"audio" },
  { L"MIXER_CFG_PATH",        L"MIXER_CFG_PATH",                  L"Which mixer.json the settings are read from", L"audio" },
  { L"MIXER_ALLOW=",           L"MIXER_ALLOW=<route>",             L"A route's preferred devices, with whether each is connected now and its battery", L"audio" },
  { L"MIXER_BATTERY",          L"MIXER_BATTERY",                   L"Re-read Bluetooth battery levels for connected devices", L"audio" },
  { L"MIXER_REFRESH",          L"MIXER_REFRESH",                   L"Re-read every provider now, and report what came back", L"audio" },
  { L"MIXER_WINDOW=",          L"MIXER_WINDOW=<0|1>",              L"Open or close the Audio Mixer window", L"audio" },
  { L"MIXER_ORDER",            L"MIXER_ORDER",                     L"The STORED fader order, then MIXER_ORDER_END with the arrangement token", L"audio" },
  { L"MIXER_ORDER_MOVE=",      L"MIXER_ORDER_MOVE=<ch>|<fader>|<delta>[|<rev>]", L"Move a fader up (-1) or down (+1); with rev, refused if the list changed since", L"audio" },
  { L"MIXER_HIDE=",            L"MIXER_HIDE=<ch>|<fader>|<0|1>",   L"Hide or reveal one fader on every surface; a view preference only -- it keeps its level, its place and every write verb", L"audio" },
  { L"MIXER_HIDDEN",           L"MIXER_HIDDEN",                    L"The hidden set, so a client can render a 'show hidden' toggle without walking the state", L"audio" },
  { L"MIXER_FADER_NAME=",      L"MIXER_FADER_NAME=<ch>|<fader>|<name>", L"The user's short name for one fader; an empty name clears it back to the default", L"audio" },
  { L"MIXER_VIEW",             L"MIXER_VIEW",                      L"The fader list as DRAWN: pos, order, hidden, pinned, then the counts, token and view flags", L"audio" },
  { L"MIXER_ORDER_REV",        L"MIXER_ORDER_REV",                 L"The arrangement token alone, to check a held list is still current", L"audio" },
  { L"MIXER_ORDER_SET=",       L"MIXER_ORDER_SET=<rev>|<key>,<key>,...", L"Push a whole fader arrangement; rev is required, omitted keys keep their order", L"audio" },
  { L"MIXER_PROFILE_LIST",     L"MIXER_PROFILE_LIST",              L"The saved mixer profiles, as leaf and display name", L"audio" },
  { L"MIXER_PROFILE_SAVE=",    L"MIXER_PROFILE_SAVE=<name>",       L"Save the current mix and layout as a profile; an empty name banks a timestamp", L"audio" },
  { L"MIXER_PROFILE_LOAD=",    L"MIXER_PROFILE_LOAD=<name>",       L"Recall a mixer profile: levels and layout together", L"audio" },
  { L"MIXER_PROFILE_DELETE=",  L"MIXER_PROFILE_DELETE=<name>",     L"Delete a saved mixer profile", L"audio" },
  // The GROUP verbs, which REPLACED the four MIXER_SLOT ones -- a group is a
  // set of faders one key moves together, and a group of one is exactly what a
  // named slot used to be. The slot entries stayed here after the rename, so
  // the help documented four commands the engine no longer dispatches while
  // saying nothing about the four it does. Both halves of that are what
  // test_ipc_help checks for.
  { L"MIXER_GROUPS",           L"MIXER_GROUPS",                    L"Every volume-hotkey group: name, member count and keys", L"audio" },
  { L"MIXER_GROUP_SET=",       L"MIXER_GROUP_SET=<n>|<ch>|<fader>[|<ch>|<fader>...]", L"Replace group n's membership; no pairs empties it", L"audio" },
  { L"MIXER_GROUP_NUDGE=",     L"MIXER_GROUP_NUDGE=<n>|<delta>",   L"Move every fader in group n by delta, each clamped to 0..1", L"audio" },
  { L"MIXER_GROUP_MUTE=",      L"MIXER_GROUP_MUTE=<n>",            L"Toggle mute on every fader in group n", L"audio" },
  { L"MIXER_SONAR_RESTART",    L"MIXER_SONAR_RESTART",             L"Restart the SteelSeries process tree; confirms first unless disabled", L"audio" },
  { L"SET_AUDIO_GAIN=",        L"SET_AUDIO_GAIN=<0.01-256>",       L"Audio sensitivity multiplier applied to the captured signal", L"audio" },
  { L"GET_AUDIO_GAIN",         L"GET_AUDIO_GAIN",                  L"Configured and effective audio sensitivity", L"audio" },
  { L"GET_AUDIO_DIAG",         L"GET_AUDIO_DIAG",                  L"Gain globals and sampled PCM values, for checking capture levels", L"audio" },
  { L"DIAG_AUDIO",             L"DIAG_AUDIO",                      L"What the audio path is doing now, including the capture mode", L"audio" },
  { L"FFT_ATTACK=",            L"FFT_ATTACK=<0.0-1.0>",            L"FFT attack smoothing", L"audio" },
  { L"FFT_DECAY=",             L"FFT_DECAY=<0.0-1.0>",             L"FFT decay smoothing", L"audio" },
  { L"AMP|",                   L"AMP|<band>=<value>|...",          L"Set EQ band amplitudes", L"audio" },
  { L"AUDIO_PROFILE=",         L"AUDIO_PROFILE=<name>",            L"Pin an audio profile to the running preset; empty means explicitly the default", L"audio" },
  { L"AUDIO_PROFILE_CLEAR",    L"AUDIO_PROFILE_CLEAR",             L"Remove the preset's audio-profile slot so it inherits again", L"audio" },
  { L"AUDIO_PROFILE_LIST",     L"AUDIO_PROFILE_LIST",              L"List saved audio profile names", L"audio" },

  // -- colour ----------------------------------------------------------------
  { L"COL_HUE=",               L"COL_HUE=<-1.0..1.0>",             L"Hue shift", L"color" },
  { L"COL_SATURATION=",        L"COL_SATURATION=<-1.0..1.0>",      L"Saturation adjustment", L"color" },
  { L"COL_BRIGHTNESS=",        L"COL_BRIGHTNESS=<-1.0..1.0>",      L"Brightness adjustment", L"color" },
  { L"HUE_AUTO=",              L"HUE_AUTO=<0|1>",                  L"Enable automatic hue cycling", L"color" },
  { L"HUE_AUTO_SECONDS=",      L"HUE_AUTO_SECONDS=<seconds>",      L"Period of the automatic hue cycle", L"color" },

  // -- visual parameters -----------------------------------------------------
  { L"VAR_TIME=",              L"VAR_TIME=<factor>",               L"Time speed factor", L"visual" },
  { L"VAR_FRAME=",             L"VAR_FRAME=<factor>",              L"Frame factor", L"visual" },
  { L"VAR_FPS=",               L"VAR_FPS=<factor>",                L"FPS factor reported to presets", L"visual" },
  { L"VAR_INTENSITY=",         L"VAR_INTENSITY=<factor>",          L"Visual intensity multiplier (vis_intensity)", L"visual" },
  { L"VAR_SHIFT=",             L"VAR_SHIFT=<float>",               L"Visual shift value (vis_shift)", L"visual" },
  { L"VAR_VERSION=",           L"VAR_VERSION=<int>",               L"Vis version override (vis_version)", L"visual" },
  { L"VAR_QUALITY=",           L"VAR_QUALITY=<0.01-1.0>",          L"Render quality scale", L"visual" },
  { L"VAR_AUTO=",              L"VAR_AUTO=<0|1>",                  L"Automatic quality adjustment", L"visual" },
  { L"SET_MESH_SIZE=",         L"SET_MESH_SIZE=<gridX>",           L"Warp mesh width; height follows at 3/4, applied on the render thread", L"visual" },
  { L"SET_FPS=",               L"SET_FPS=<0|5-720>",               L"Primary window frame cap; 0 means unlimited", L"visual" },
  { L"GET_FPS",                L"GET_FPS",                         L"Query the primary frame cap", L"visual" },
  { L"SET_VSYNC=",             L"SET_VSYNC=<0|1>",                 L"Wait for vblank when presenting; on by default, and it overrides every frame cap", L"visual" },
  { L"GET_VSYNC",              L"GET_VSYNC",                       L"VSYNC=<0|1>, then a VSYNC_OUT line per display output, then VSYNC_END", L"visual" },
  { L"WAVE|",                  L"WAVE|<key>=<value>|...",          L"Set waveform parameters", L"visual" },

  // -- window ----------------------------------------------------------------
  { L"SET_WINDOW=",            L"SET_WINDOW=<x>,<y>,<w>,<h>",      L"Set render window position and size; w=0,h=0 moves without resizing", L"window" },
  // These four are handled by HandleWindowDiagIPC, which runs long before the
  // strict fallback -- so they WORKED while missing from this table, and were
  // therefore absent from HELP and undiscoverable. Registration here is what
  // makes a verb findable, not what makes it dispatch.
  { L"GET_WINDOW",             L"GET_WINDOW[=<title>]",            L"Where a window is: bare gives the render window, else a title match. Screen x/y/w/h, client size, display device, visible/minimized/maximized/topmost, dpi", L"window" },
  { L"GET_WINDOWS",            L"GET_WINDOWS",                     L"One WINDOW| line per top-level window this process owns, then WINDOWS_END. Hidden ones listed with visible=0, since a closed tool window still has a position a layout must avoid", L"window" },
  { L"GET_HUD_STATE",          L"GET_HUD_STATE",                   L"What the HUD is set to show -- HUD_STATE|primaryOnly=..|presetInfo=..|fps=..|debug=..|child=.. -- so a remote can draw the toggles in the right position instead of guessing", L"window" },
  { L"DIAG_WINDOW=",           L"DIAG_WINDOW=<title>",             L"Every control on one of our tool windows: class, id, rect, visibility, clipping, whether the text fits, and its codepoints", L"window" },
  { L"CAPTURE_WINDOW=",        L"CAPTURE_WINDOW=<title>",          L"PNG of one tool window under capture\\. Taken in-process with PW_RENDERFULLCONTENT, which is the only way owner-drawn buttons render at all", L"window" },
  { L"OPACITY=",               L"OPACITY=<0.0-1.0>",               L"Set window opacity", L"window" },
  { L"ALWAYS_ON_TOP",          L"ALWAYS_ON_TOP",                   L"Toggle always-on-top", L"window" },
  { L"GET_ALWAYS_ON_TOP",      L"GET_ALWAYS_ON_TOP",               L"Query always-on-top state", L"window" },
  { L"SET_ALWAYS_ON_TOP=",     L"SET_ALWAYS_ON_TOP=<0|1>",         L"Set always-on-top on the primary and every mirror", L"window" },
  { L"MOVE_TO_DISPLAY=",       L"MOVE_TO_DISPLAY=<n>",             L"Centre the render window on \\\\.\\DISPLAYn; replies MOVE_TO_DISPLAY_ERR if no such display is attached", L"window" },
  { L"RAISE_WINDOW",           L"RAISE_WINDOW",                    L"Bring this instance's window to the top of the z-order, without taking focus", L"window" },
  { L"LOWER_WINDOW",           L"LOWER_WINDOW",                    L"Send this instance's window to the bottom of the z-order, without taking focus. Drops always-on-top first, or the move stays inside the topmost band and does nothing visible. Relayable, so DISPLAY|<N>|LOWER_WINDOW reaches a child", L"window" },
  { L"SET_BORDERLESS_FS=",     L"SET_BORDERLESS_FS=<0|1>",         L"Borderless fullscreen absolutely; SIGNAL|BORDERLESS_FS toggles instead", L"window" },
  // Dispatched since the Mirror/Children toggle landed, but never documented,
  // so HELP claimed the instance could not be cleared off a screen without
  // retiring it -- which is the whole reason the command exists.
  { L"SET_MINIMIZED=",         L"SET_MINIMIZED=<0|1>",             L"Minimise or restore this instance; clears a screen without retiring the child that owns it", L"window" },
  { L"SET_ALL_FULLSCREEN=",    L"SET_ALL_FULLSCREEN=<0|1|2>",      L"1: foreground+fullscreen the primary and every ready child; 0: put each back exactly as found; 2: leave fullscreen regardless of what 0 would restore", L"window" },
  { L"GET_ALL_FULLSCREEN",     L"GET_ALL_FULLSCREEN",              L"Whether the primary, mirrors and every enabled child are currently up, read from the actual windows", L"window" },
  { L"CAPTURE",                L"CAPTURE",                         L"Write a screenshot to the capture path", L"window" },

  // -- displays and mirrors --------------------------------------------------
  { L"MIRROR_INDEPENDENT",     L"MIRROR_INDEPENDENT",              L"Toggle independent per-display rendering", L"display" },
  { L"GET_MIRROR_INDEPENDENT", L"GET_MIRROR_INDEPENDENT",          L"Query independent per-display rendering", L"display" },
  { L"SET_MIRROR_INDEPENDENT=", L"SET_MIRROR_INDEPENDENT=<0|1|N,val>", L"Set independent render globally, or for display N", L"display" },
  { L"SET_MIRROR_ENABLED=",    L"SET_MIRROR_ENABLED=<0|1|N,val>",  L"Enable or disable non-primary outputs, or display N", L"display" },
  { L"SET_MIRROR_OPACITY=",    L"SET_MIRROR_OPACITY=<0-100|N,val>", L"Opacity for every mirror, or display N", L"display" },
  { L"SET_MIRROR_CLICKTHRU=",  L"SET_MIRROR_CLICKTHRU=<0|1|N,val>", L"Click-through for every mirror, or display N", L"display" },
  { L"SET_MIRROR_WATERMARK=",  L"SET_MIRROR_WATERMARK=<0|1>",       L"Watermark mode for every mirror at once, absolutely; MIRROR_WATERMARK toggles", L"display" },
  { L"SET_MIRROR_WIPE",        L"SET_MIRROR_WIPE[=reinit]",        L"Re-arm the flip-chain wipe; =reinit force-recreates the swapchains", L"display" },
  { L"SET_MIRROR_MAXFPS=",     L"SET_MIRROR_MAXFPS=<0|5-240>",     L"Independent-mirror worker rate cap; 0 means parity with the primary", L"display" },
  { L"GET_MIRROR_MAXFPS",      L"GET_MIRROR_MAXFPS",               L"Query the mirror rate cap", L"display" },
  { L"DIAG_MIRRORS",           L"DIAG_MIRRORS",                    L"Full mirror dump: sizes, ports, sampling, per-display state", L"display" },
  // Registering it here is not documentation, it is DISPATCH: strict mode --
  // on by default for -test and -child -- validates against this table, so a
  // verb missing from it is silently refused. Adding DIAG_GPUTIME to
  // engine_messages.cpp alone produced a handler that never ran and a reply
  // that never came, which read as a broken feature rather than an
  // unregistered one.
  { L"DIAG_GPUTIME=",          L"DIAG_GPUTIME=<0|1>[,<ms>]",       L"Per-stage GPU timing; logs frames where a stage exceeds <ms> (default 50)", L"display" },
  { L"DIAG_VRAMMARK=",         L"DIAG_VRAMMARK=<0|1>",             L"Log LOCAL VRAM and the delta since the last mark at each allocating checkpoint, to attribute a jump to the call that caused it", L"display" },
  { L"SET_TEXCAP=",            L"SET_TEXCAP=<0|64-16384>",         L"Longest edge a texture loaded from disk keeps; 0 removes the cap. Default 1024. Applies to the NEXT load, not to textures already resident", L"system" },
  { L"GET_TEXCAP",             L"GET_TEXCAP",                      L"Query the texture dimension cap", L"system" },
  { L"DIAG_BALANCE",           L"DIAG_BALANCE[=<0|1>]",            L"Adaptive frame-rate balancing on/off, and its state: idle/probing/holding/ruledout, the main rate, the slowest display, the ratio between them, the caps in force, and the distress ladder's rung", L"display" },
  { L"DIAG_LADDER",            L"DIAG_LADDER[=<0|1>]",             L"The distress ladder on/off, and its rung: 1 adaptive frame rate, 2 cheapest copy mirrors. Reports whether the frame is GPU-bound, the achievable rate rung 1 tracks, the trigger in force, and what rung 2 has taken", L"display" },
  { L"SET_LADDER_TRIGGER=",    L"SET_LADDER_TRIGGER=<-1|fps>",     L"The rate below which the ladder falls back to copy mirrors. -1 derives it from the frame cap (34 when that is high or unset, two thirds of it below 35); a value pins it. Persisted", L"display" },
  { L"GET_LADDER_TRIGGER",     L"GET_LADDER_TRIGGER",              L"The mirror-fallback trigger in force, the setting behind it, what the derivation would choose, and the frame cap it is relative to", L"display" },
  { L"DIAG_MIRRORPACE=",       L"DIAG_MIRRORPACE=<0|1>",           L"Closed-loop mirror pacer on/off; off restores the pre-#230 behaviour that removes the device on an expensive preset", L"display" },
  { L"DIAG_MIRRORCAP=",        L"DIAG_MIRRORCAP=<-1|0|N>",         L"Mirror swap-chain long-edge cap, unpersisted: N forces it, 0 removes it, -1 hands it back to the derivation. Reports the applied value, its source, the setting and what the VRAM budget alone implies", L"display" },
  { L"SET_MIRRORCAP=",         L"SET_MIRRORCAP=<-1|0|N>",          L"The PERSISTED mirror swap-chain cap: -1 derive it from the VRAM budget (default), 0 no cap, N a fixed long edge for a machine under contention", L"display" },
  { L"DIAG_BATTERY",           L"DIAG_BATTERY",                    L"HUD headset battery: toggle, percent, and which device it came from", L"system" },
  // Match field carries the '=', because that is what the dispatcher matches
  // (engine_mixer.cpp: rfind(L"DIAG_BATTERY_SIM=", 0)). Without it HELP
  // advertised a bare DIAG_BATTERY_SIM that falls through to the script
  // engine, and test_ipc_help failed both ways at once -- the '=' form
  // undocumented and the bare form undispatched.
  { L"DIAG_BATTERY_SIM=",      L"DIAG_BATTERY_SIM=<0-100|off>",    L"Force the HUD battery figure so its colour bands can be seen; in memory only", L"diag" },
  { L"DIAG_GPU",               L"DIAG_GPU",                        L"This process's GPU and VRAM cost, keyed by pid. Asking turns the sampler on for about 8s, and because the counter is a rate the first reply after a quiet period carries gpu3d=-1 -- poll twice about a second apart and read the second", L"diag" },
  // The old text claimed child "gives the display its OWN PRESET in this
  // process, not its own instance". It never did -- that branch calls
  // RequestChildSpawn -- and the machinery for the in-process version sat
  // unreachable behind the same flag. Now there is a word for each tier.
  { L"SET_DISPLAY_MODE=",      L"SET_DISPLAY_MODE=<N>,<off|mirror|preset|child>", L"How DISPLAY N renders, cheapest first: mirror follows the primary; preset gives it its own preset IN THIS PROCESS; child gives it its own preset in its own instance. 'own' is accepted for preset", L"display" },
  { L"GET_DISPLAY_MODE",       L"GET_DISPLAY_MODE[=<N>]",          L"Which tier each display is on, plus auto=1 where the cost gate promoted it to a child rather than the user asking", L"display" },
  { L"GET_CHILDREN",           L"GET_CHILDREN",                    L"One CHILD| line per per-display child instance, then CHILDREN_END; state is pending|starting|ready|failed|absent", L"display" },
  { L"DIAG_CHILDREN",          L"DIAG_CHILDREN",                   L"Child worker health: thread, event, job, queue depth, pending and failed spawns", L"display" },
  // Per-display preset control (forgejo#32). The four SET_DISPLAY_* settings
  // are parent-owned and persist; PRESET/NEXT/PREV and the relay are transient,
  // because a child pins the config write shield and stores nothing itself.
  { L"SET_DISPLAY_PRESET=",    L"SET_DISPLAY_PRESET=<N>,<path>",   L"Show a preset on display N now; transient, does not become its startup preset", L"display" },
  { L"SET_DISPLAY_STARTUP_PRESET=", L"SET_DISPLAY_STARTUP_PRESET=<N>,<path>", L"Pin the preset display N returns to when its instance restarts", L"display" },
  { L"SET_DISPLAY_PRESET_DIR=", L"SET_DISPLAY_PRESET_DIR=<N>,<path>", L"Preset folder display N cycles through; empty inherits the parent's", L"display" },
  { L"SET_DISPLAY_ORDER=",     L"SET_DISPLAY_ORDER=<N>,<0|1>",     L"Random (0) or sequential (1) order on display N", L"display" },
  { L"SET_DISPLAY_LOCK=",      L"SET_DISPLAY_LOCK=<N>,<-1|0|1>",   L"Preset lock for display N: -1 inherits the main window's, 0 unlocked, 1 locked", L"display" },
  { L"SET_DISPLAY_MSGMAX=",    L"SET_DISPLAY_MSGMAX=<N>,<-1|0..n>", L"How many messages display N shows at once; -1 inherits the main window's, which cannot itself inherit. The supertext slots are one shared pool, so without this a busy panel takes them all and the others fall silent", L"display" },
  { L"SET_DISPLAY_WATERMARK=", L"SET_DISPLAY_WATERMARK=<N>,<0|1>", L"Watermark ONE display: the primary, an in-process mirror, or a child. Persisted per display; SET_MIRROR_WATERMARK still does every mirror at once", L"display" },
  { L"GET_DISPLAY_WATERMARK",  L"GET_DISPLAY_WATERMARK[=<N>]",     L"Watermark state per display; bare lists every monitor with its device and whether a child owns it", L"display" },
  { L"SET_DISPLAY_ENABLED=",   L"SET_DISPLAY_ENABLED=<N>,<0|1>",   L"Enable/disable display N; stops or starts its child if it owns one", L"display" },
  { L"SET_WATERMARK=", L"SET_WATERMARK=<0|1>",     L"Set single-window watermark mode absolutely, on this instance", L"window" },
  { L"GET_WATERMARK",  L"GET_WATERMARK",           L"Query single-window watermark state on this instance", L"window" },
  { L"GET_INPUT_MIX",  L"GET_INPUT_MIX",           L"Query video input-mix state: source, webcam device, opacity, onTop, luma key", L"video" },
  { L"GET_COVER_STATUS", L"GET_COVER_STATUS",      L"Query whether the track-artwork sprite is currently shown, and its image file", L"sprite" },
  { L"GET_NUMERIC_MODE", L"GET_NUMERIC_MODE",      L"Query whether digit-key input selects a sprite or a custom message", L"sprite" },
  { L"SET_INJECT_EFFECT=",      L"SET_INJECT_EFFECT=<0-4>",         L"Post-process effect: 0 off, 1 brighten, 2 darken, 3 solarize, 4 invert (F11 cycles the same thing)", L"shader" },
  { L"CHILD_TEST_WINDOWS=",    L"CHILD_TEST_WINDOWS=<auto|off|tiny|hidden>", L"How a child's window comes up; auto follows testing mode. Applies to the next spawn", L"display" },
  { L"SET_DISPLAY_CHILD_RECT=", L"SET_DISPLAY_CHILD_RECT=<N>,<x>,<y>,<w>,<h>", L"Where display N's child window goes in tiny mode; w=0 restores the corner default", L"display" },
  { L"SET_DISPLAY_TIME_BETWEEN=", L"SET_DISPLAY_TIME_BETWEEN=<N>,<sec>", L"Cycle interval for display N; 0 never cycles, -1 inherits the parent's", L"display" },
  { L"DISPLAY_NEXT=",          L"DISPLAY_NEXT=<N>",                L"Advance display N only, leaving every other display where it is", L"display" },
  { L"DISPLAY_PREV=",          L"DISPLAY_PREV=<N>",                L"Step display N back only", L"display" },
  { L"DISPLAY|",               L"DISPLAY|<N>|<command>",           L"Relay any command to display N's child instance; a few are denied", L"display" },
  { L"SAVE_DISPLAY_PROFILE=",  L"SAVE_DISPLAY_PROFILE=<path>[|primary=1][|messaging=1]", L"Write the display state to a profile, optionally with primary preset settings and messaging", L"display" },
  { L"LOAD_DISPLAY_PROFILE=",  L"LOAD_DISPLAY_PROFILE=<path>",     L"Replace the whole display state with a saved profile", L"display" },
  { L"GET_DISPLAY_PROFILE",    L"GET_DISPLAY_PROFILE",             L"Path of the display profile last saved or loaded", L"display" },
  { L"DISPLAY_PROFILE_LIST",   L"DISPLAY_PROFILE_LIST",            L"Saved display profiles, as leaf and name", L"display" },
  { L"DISPLAY_PROFILE_SAVE=",  L"DISPLAY_PROFILE_SAVE=<name>[|primary=1][|messaging=1]", L"Save by name in the profiles folder; empty name banks a timestamp. Does not change the startup default", L"display" },
  { L"DISPLAY_PROFILE_LOAD=",  L"DISPLAY_PROFILE_LOAD=<name>",     L"Load a saved display profile by name", L"display" },
  { L"DISPLAY_PROFILE_DELETE=", L"DISPLAY_PROFILE_DELETE=<name>",  L"Delete a saved display profile", L"display" },
  { L"DISPLAY_PROFILE_STARTUP", L"DISPLAY_PROFILE_STARTUP[=<name>]", L"Query or set which profile loads at startup, and whether one does; empty name disables it", L"display" },
  { L"SAVE_DISPLAY_SNAPSHOT",  L"SAVE_DISPLAY_SNAPSHOT",           L"Bank every screen's CURRENT preset as a timestamped profile in resources/displayprofiles", L"display" },
  { L"LIST_DISPLAY_PROFILES",  L"LIST_DISPLAY_PROFILES",           L"One DISPLAY_PROFILE| line per saved profile, then DISPLAY_PROFILES_END", L"display" },
  { L"NEXT_DISPLAY_PROFILE",   L"NEXT_DISPLAY_PROFILE",            L"Load the next saved profile, wrapping", L"display" },
  { L"PREV_DISPLAY_PROFILE",   L"PREV_DISPLAY_PROFILE",            L"Load the previous saved profile, wrapping", L"display" },
  { L"DIAG_DISPLAY_MODE=",     L"DIAG_DISPLAY_MODE=<0|1|2>",       L"Show the raw VS[0] or VS[1] render target instead of the composed frame", L"display" },

  // -- messages and sprites --------------------------------------------------
  { L"MSG|",                   L"MSG|text=<s>|font=..|size=..",    L"Show an animated text message; docs/Scripts.md lists every parameter", L"message" },
  { L"TOGGLE_MESSAGES",        L"TOGGLE_MESSAGES",                 L"Toggle custom message playback", L"message" },
  { L"SET_MESSAGES=",          L"SET_MESSAGES=<0|1>",              L"Enable or disable custom message playback", L"message" },
  { L"GET_MESSAGES",           L"GET_MESSAGES",                    L"Query whether custom messages are enabled", L"message" },
  { L"CLEARTEXTS",             L"CLEARTEXTS",                      L"Remove every active text message", L"message" },
  { L"TESTFONTS",              L"TESTFONTS",                       L"Show font and animation test messages", L"message" },
  { L"DIAG_MESSAGES",          L"DIAG_MESSAGES",                   L"Every term of the autoplay guard: enabled, testing, autoplay, count, nextAt", L"message" },
  { L"CLEARSPRITES",           L"CLEARSPRITES",                    L"Remove every active sprite", L"sprite" },

  // -- Spout -----------------------------------------------------------------
  { L"SPOUT_ACTIVE=",          L"SPOUT_ACTIVE=<0|1>",              L"Enable or disable Spout output", L"spout" },
  { L"SPOUT_FIXEDSIZE=",       L"SPOUT_FIXEDSIZE=<0|1>",           L"Toggle a fixed Spout output resolution", L"spout" },
  { L"SPOUT_RESOLUTION=",      L"SPOUT_RESOLUTION=<WxH>",          L"Set the Spout output resolution", L"spout" },
  { L"SPOUTINPUT=",            L"SPOUTINPUT=<sender>",             L"Enable Spout input from a named sender", L"spout" },
  { L"SPOUT_SENDER=",          L"SPOUT_SENDER=<name>",             L"Which Spout sender to take INPUT from. Stores the name only -- SPOUTINPUT= or SIGNAL|ENABLESPOUTMIX is what switches the input on, which is the order Milkwave Remote sends them in. Answered by the pipe server, which posts it to the render window rather than routing it through the dispatcher", L"spout" },

  // -- remote and transport --------------------------------------------------
  { L"STATE",                  L"STATE",                           L"Full state batch: opacity, preset, settings, track, volume; ends with END_BATCH", L"remote" },
  { L"SIGNAL|",                L"SIGNAL|<name>",                   L"Post a window signal; the SIGNAL section of this index lists the names", L"remote" },
  { L"TRACK|",                 L"TRACK|artist=..|title=..|album=..", L"Supply track info, honoured when the track source is set to IPC", L"remote" },
  { L"LIST_DEVICES",           L"LIST_DEVICES",                    L"List devices authorised for the TCP remote", L"remote" },
  { L"DEAUTH_DEVICE|",         L"DEAUTH_DEVICE|<id>",              L"Revoke a TCP remote device's authorisation and disconnect it", L"remote" },

  // -- driving a script file over the pipe (#303) ----------------------------
  //
  // Three verbs and not one, deliberately: loading and playing are separate
  // acts in the UI too, and a caller wanting to see what parsed before
  // anything executes needs the gap.
  { L"SCRIPT_LOAD=",           L"SCRIPT_LOAD=<path>",              L"Parse a script file WITHOUT running it. Replies SCRIPT_LOADED=<lines>|<path>, so a caller can check it parsed before anything moves", L"script" },
  { L"SCRIPT_PLAY",            L"SCRIPT_PLAY",                     L"Start the loaded script. Replies SCRIPT_PLAYING", L"script" },
  { L"SCRIPT_STOP",            L"SCRIPT_STOP",                     L"Stop the running script. Replies SCRIPT_STOPPED|errors=<n> -- the count STRICT accumulated, which is the script's verdict", L"script" },

  // -- system ----------------------------------------------------------------
  { L"STRICT",                 L"STRICT[=<1|0|on|off|true|false|yes|no>]", L"Per CONNECTION: unknown commands answer ERR|UNKNOWN instead of being drawn on screen as a message. Bare queries. Sticky for the life of the connection; a new one starts at the instance default. Answered by the pipe server, so it never reaches the dispatcher -- an unrecognised value is an error, because _wtoi(\"ON\") is 0 and would silently turn the feature off", L"system" },
  { L"CONFIG",                 L"CONFIG",                          L"Reload configuration and rebuild fonts", L"system" },
  { L"SETTINGS",               L"SETTINGS",                        L"Reload timing settings from the INI", L"system" },
  { L"CONFIG_EXPORT_INI",      L"CONFIG_EXPORT_INI",               L"Copy registry-held settings back into settings.ini and friends", L"system" },
  { L"SET_LOGLEVEL=",          L"SET_LOGLEVEL=<0-4>",              L"Log level: 0 Off, 1 Error, 2 Warn, 3 Info, 4 Verbose", L"system" },
  { L"GET_LOGLEVEL",           L"GET_LOGLEVEL",                    L"Query the log level and the log directory", L"system" },
  { L"CLEAR_LOGS",             L"CLEAR_LOGS",                      L"Delete everything under log/ and reopen debug.log", L"system" },
  { L"SHUTDOWN",               L"SHUTDOWN",                        L"Clean shutdown: saves settings, stops the render thread, exits", L"system" },
  { L"RESTART_DEVICE",         L"RESTART_DEVICE",                  L"Recreate the D3D12 device, the same path a TDR takes", L"system" },
  { L"TESTING_MODE",           L"TESTING_MODE[=<0|1>]",            L"Freeze auto preset changes, the idle timer and the keyboard; shields settings writes", L"system" },
  { L"SET_IDLE_TIMER=",        L"SET_IDLE_TIMER=<0|1>",            L"Enable or disable the idle timer at runtime", L"system" },
  { L"GET_IDLE_TIMER",         L"GET_IDLE_TIMER",                  L"Query idle timer enabled state, timeout and action", L"system" },
  { L"SET_IDLE_ACTIVE=",       L"SET_IDLE_ACTIVE=<0|1>",           L"Trigger the idle action now, as if the timer fired; independent of SET_IDLE_TIMER=", L"system" },
  { L"GET_IDLE_ACTIVE",        L"GET_IDLE_ACTIVE",                 L"Whether the idle action is on screen right now", L"system" },
  { L"SET_IDLE_TIMEOUT=",      L"SET_IDLE_TIMEOUT=<1-60>",         L"Minutes of no input before the idle action fires. Persisted", L"system" },
  { L"SET_IDLE_ACTION=",       L"SET_IDLE_ACTION=<0-3>",           L"0 Fullscreen, 1 Stretch/Mirror, 2 Mirror all, 3 Load display profile. Persisted", L"system" },
  { L"SET_IDLE_PROFILE=",      L"SET_IDLE_PROFILE=<name>",         L"The display profile action 3 loads; empty clears it. An unknown name is refused now rather than silently at idle time", L"system" },
  { L"SET_IDLE_FULL_OPACITY=", L"SET_IDLE_FULL_OPACITY=<0|1>",     L"Take every display to 100% opacity while idle, whichever action is set. Persisted", L"system" },
  { L"RESET_IDLE_TIMER",       L"RESET_IDLE_TIMER",                L"Restart the countdown, exactly as a keypress would -- and wake an active idle session when auto-restore is on", L"system" },
  { L"SET_FALLBACK_TEX=",      L"SET_FALLBACK_TEX=<0-5>[|<path>]", L"Missing-texture fallback: 0 hue, 1 white, 2 black, 3 and 4 random, 5 custom file", L"system" },
  { L"GET_FALLBACK_TEX",       L"GET_FALLBACK_TEX",                L"Query the fallback texture style, file and SRV index", L"system" },

  // -- diagnostics -----------------------------------------------------------
  { L"DIAG_CONFIG",            L"DIAG_CONFIG[=flush]",             L"Settings-layer counters; =flush writes the cache out first", L"diag" },
  { L"DIAG_UI_MODE",           L"DIAG_UI_MODE",                    L"Which full-frame overlay is up (regular/menu/load/settings...) and the settings cursor", L"diag" },
  { L"GET_RENDER_DIAG",        L"GET_RENDER_DIAG",                 L"Rendering values (blur scales and biases) for cross-engine comparison", L"diag" },
  { L"DIAG_BINDINGS",          L"DIAG_BINDINGS",                   L"Side-by-side SRV binding slots for the primary and the mirror", L"diag" },
  { L"DIAG_HOTKEYS",           L"DIAG_HOTKEYS",                    L"One HOTKEY| line per GLOBAL binding, then HOTKEYS_END; held=0 means another application owns the combination", L"diag" },
  { L"DIAG_HOTKEY_BIND=",      L"DIAG_HOTKEY_BIND=<action>,<mod>,<vk>[,<scope>]", L"Set an action's binding -- scope 1 (default) global and re-registered, 0 local; in memory only, never saved to hotkeys.json. vk=0 unbinds", L"diag" },
  { L"DIAG_HOTKEY_CONFLICTS=", L"DIAG_HOTKEY_CONFLICTS=<action>,<mod>,<vk>,<scope>", L"What assigning that combination would unbind; scope 0=local 1=global", L"diag" },
  { L"DIAG_HOTKEY_SCAN",       L"DIAG_HOTKEY_SCAN",                 L"One HOTKEY_SCAN| line per candidate combination, then HOTKEY_SCAN_END; status is free|taken|ours", L"diag" },
  { L"GET_EEL_STATE",          L"GET_EEL_STATE [megabuf=S,C] [gmegabuf=S,C] [reg=S,C]", L"Dump EEL megabuf, gmegabuf and reg values", L"diag" },
  { L"TEST_EEL_ISOLATION",     L"TEST_EEL_ISOLATION",              L"Self-test that per-context EEL storage really is isolated", L"diag" },
  { L"DIAG_SIMULATE_DEVICE_REMOVED", L"DIAG_SIMULATE_DEVICE_REMOVED", L"Force DXGI_ERROR_DEVICE_REMOVED to exercise TDR handling; DESTROYS the device", L"diag" },
};

// Reached through the fallback at the end of LaunchMessage, which hands
// anything unmatched to ExecuteScriptLine. These work over the pipe exactly
// like the explicit commands above -- see the note in ipc_help.h.
const IpcCommandDoc kScriptCommands[] = {
  { L"NEXT",           L"NEXT",                    L"Soft cut to the next preset", L"script" },
  { L"PREV",           L"PREV",                    L"Go back to the previous preset", L"script" },
  { L"RAND",           L"RAND",                    L"Toggle random or sequential preset order", L"script" },
  { L"LOCK",           L"LOCK",                    L"Toggle the preset lock", L"script" },
  { L"PRESET=",        L"PRESET=<path>",           L"Load a preset (same as the IPC command)", L"script" },
  { L"PRESETINFO",     L"PRESETINFO",              L"Toggle the preset info display", L"script" },
  { L"STOP",           L"STOP",                    L"Stop the running script", L"script" },
  { L"RESET",          L"RESET",                   L"Reset the beat timer", L"script" },
  { L"BPM=",           L"BPM=<n>",                 L"Set beats per minute", L"script" },
  { L"BEATS=",         L"BEATS=<n>",               L"Set beats per script line", L"script" },
  { L"LINE=",          L"LINE=<n|CURR|NEXT|PREV>", L"Jump to a script line and execute it", L"script" },
  { L"FILE=",          L"FILE=<path>",             L"Load and start a different script file", L"script" },

  // -- driving a TEST from a script file (#303) ------------------------------
  //
  // Without STRICT a test script cannot be trusted at all: an unrecognised
  // token is DRAWN ON SCREEN as a message, so a typo, or a verb a build does
  // not have, becomes scenery and the script reports success having done
  // nothing.
  { L"STRICT",         L"STRICT",                  L"Unknown commands from here on are counted as errors instead of being drawn on screen. Sticky for the rest of the file. Distinct from the pipe's STRICT, which is per connection and never reaches the script engine", L"script" },
  { L"CHECK",          L"CHECK",                   L"Judge the preset that is up and tally it: shader errors and MISSING TEXTURES counted separately, because a preset with a missing texture compiles, renders, and is still wrong", L"script" },
  { L"WAITREADY",      L"WAITREADY",               L"Hold the script until the preset has actually finished loading, or 10s. A timeout is logged and counted rather than waited on forever -- a repro that hangs tells its reader nothing", L"script" },
  { L"REPEAT=",        L"REPEAT=<count>,<line>",   L"Jump back to <line> at most <count> times -- the one BOUNDED backward jump. LINE= is unconditional, so a sweep written with it never ends and so never prints a summary. The budget is keyed on the target line, and re-armed once spent", L"script" },
  { L"LOG=",           L"LOG=<text>",              L"Write a marker into debug.log, at ERROR level so it survives every LogLevel above Off -- a public repro's only output", L"script" },
  { L"LOGSTATE",       L"LOGSTATE",                L"Log what the engine is actually doing: preset, fps, mesh, loading, blending, mirrors, display count", L"script" },
  { L"LOGSUMMARY",     L"LOGSUMMARY",              L"Log the run's verdict: checks, shader failures, texture failures, unknown commands, elapsed, and seconds per preset", L"script" },
  { L"ACTION=",        L"ACTION=<TagName>",        L"Trigger any hotkey action by tag name; the broadest command there is", L"script" },
  { L"LAUNCH=",        L"LAUNCH=<path> | LAUNCH=\"<path>\" <args>", L"Launch an external application, focusing it if already running. Quote the path to pass arguments, which also forces a NEW process rather than focusing an old one", L"script" },
  { L"EXEC=",          L"EXEC=<command>",          L"Run a command line", L"script" },
  { L"BTN=",           L"BTN=<n>",                 L"Press a Button Board button", L"script" },
  { L"SEND=",          L"SEND=<0xVK|text>",        L"Post a virtual key code or a string of characters to the render window", L"script" },
  { L"MSG=",           L"MSG=<k>=<v>;<k>=<v>",     L"Show a message; semicolons separate parameters", L"script" },
  { L"STYLE=",         L"STYLE=<name>",            L"Select a message animation profile", L"script" },
  { L"FONT=",          L"FONT=<name>",             L"Default message font", L"script" },
  { L"SIZE=",          L"SIZE=<n>",                L"Default message size", L"script" },
  { L"COLOR=",         L"COLOR=<r>,<g>,<b>",       L"Default message colour", L"script" },
  { L"CLEARPARAMS",    L"CLEARPARAMS",             L"Reset the message defaults", L"script" },
  { L"CLEARSPRITES",   L"CLEARSPRITES",            L"Remove every active sprite", L"script" },
  { L"CLEARTEXTS",     L"CLEARTEXTS",              L"Remove every active text message", L"script" },
  { L"TIME=",          L"TIME=<factor>",           L"Time speed factor (VAR_TIME)", L"script" },
  { L"FRAME=",         L"FRAME=<factor>",          L"Frame factor (VAR_FRAME)", L"script" },
  { L"FPS=",           L"FPS=<factor>",            L"FPS factor (VAR_FPS)", L"script" },
  { L"INTENSITY=",     L"INTENSITY=<factor>",      L"Visual intensity (VAR_INTENSITY)", L"script" },
  { L"SHIFT=",         L"SHIFT=<float>",           L"Visual shift (VAR_SHIFT)", L"script" },
  { L"QUALITY=",       L"QUALITY=<0.01-1.0>",      L"Render quality scale (VAR_QUALITY)", L"script" },
  { L"VERSION=",       L"VERSION=<int>",           L"Vis version override (VAR_VERSION) -- NOT the app version, see GET_VERSION", L"script" },
  { L"HUE=",           L"HUE=<-1.0..1.0>",         L"Hue shift (COL_HUE)", L"script" },
  { L"SATURATION=",    L"SATURATION=<-1.0..1.0>",  L"Saturation (COL_SATURATION)", L"script" },
  { L"BRIGHTNESS=",    L"BRIGHTNESS=<-1.0..1.0>",  L"Brightness (COL_BRIGHTNESS)", L"script" },
  { L"FULLSCREEN",     L"FULLSCREEN",              L"Toggle fullscreen", L"script" },
  { L"STRETCH",        L"STRETCH",                 L"Toggle multi-monitor stretch", L"script" },
  { L"MIRROR",         L"MIRROR",                  L"Toggle multi-monitor mirror", L"script" },
  { L"MIRROR_WM",      L"MIRROR_WM",               L"Toggle mirror watermark mode", L"script" },
  { L"MIRROR_WATERMARK", L"MIRROR_WATERMARK",      L"Toggle mirror watermark mode", L"script" },
  { L"WATERMARK",      L"WATERMARK",               L"Toggle single-window watermark mode", L"script" },
  { L"MIRROR_INDEPENDENT", L"MIRROR_INDEPENDENT",  L"Toggle independent per-display render", L"script" },
  { L"INDEPENDENT",    L"INDEPENDENT",             L"Toggle independent per-display render", L"script" },
  { L"ALWAYS_ON_TOP",  L"ALWAYS_ON_TOP",           L"Toggle always-on-top", L"script" },
  { L"AOT",            L"AOT",                     L"Toggle always-on-top", L"script" },
  { L"SONGINFO",       L"SONGINFO",                L"Open the Song Info window", L"script" },
  { L"SOUNDINFO",      L"SOUNDINFO",               L"Toggle the sound debug display", L"script" },
  { L"MEDIA_PLAY",     L"MEDIA_PLAY",              L"Send the media play/pause key", L"script" },
  { L"MEDIA_STOP",     L"MEDIA_STOP",              L"Send the media stop key", L"script" },
};

// SIGNAL|<name> -- posted to the render window by PipeServer::DispatchSignal.
const IpcCommandDoc kSignalCommands[] = {
  { L"NEXT_PRESET",          L"SIGNAL|NEXT_PRESET",          L"Advance to the next preset", L"signal" },
  { L"PREV_PRESET",          L"SIGNAL|PREV_PRESET",          L"Go back to the previous preset", L"signal" },
  { L"CAPTURE",              L"SIGNAL|CAPTURE",              L"Take a screenshot", L"signal" },
  { L"FULLSCREEN",           L"SIGNAL|FULLSCREEN",           L"Toggle fullscreen", L"signal" },
  { L"BORDERLESS_FS",        L"SIGNAL|BORDERLESS_FS",        L"Toggle borderless fullscreen", L"signal" },
  { L"STRETCH",              L"SIGNAL|STRETCH",              L"Toggle multi-monitor stretch", L"signal" },
  { L"MIRROR",               L"SIGNAL|MIRROR",               L"Toggle multi-monitor mirror", L"signal" },
  { L"MIRROR_WM",            L"SIGNAL|MIRROR_WM",            L"Toggle mirror watermark mode", L"signal" },
  { L"MIRROR_INDEPENDENT",   L"SIGNAL|MIRROR_INDEPENDENT",   L"Toggle independent per-display render", L"signal" },
  { L"WATERMARK",            L"SIGNAL|WATERMARK",            L"Toggle watermark mode", L"signal" },
  { L"ALWAYS_ON_TOP",        L"SIGNAL|ALWAYS_ON_TOP",        L"Toggle always-on-top", L"signal" },
  { L"SPRITE_MODE",          L"SIGNAL|SPRITE_MODE",          L"Switch numeric input to sprite mode", L"signal" },
  { L"MESSAGE_MODE",         L"SIGNAL|MESSAGE_MODE",         L"Switch numeric input to custom message mode", L"signal" },
  { L"SHOW_COVER",           L"SIGNAL|SHOW_COVER",           L"Show the track artwork", L"signal" },
  { L"COVER_CHANGED",        L"SIGNAL|COVER_CHANGED",        L"Tell the engine the artwork file changed", L"signal" },
  { L"ENABLEVIDEOMIX",       L"SIGNAL|ENABLEVIDEOMIX",       L"Enable video input mixing", L"signal" },
  { L"ENABLESPOUTMIX",       L"SIGNAL|ENABLESPOUTMIX",       L"Enable Spout input mixing", L"signal" },
  { L"SETVIDEODEVICE",       L"SIGNAL|SETVIDEODEVICE",       L"Select the video input device", L"signal" },
  { L"SET_INPUTMIX_LUMAKEY", L"SIGNAL|SET_INPUTMIX_LUMAKEY", L"Set the input-mix luma key", L"signal" },
  { L"SET_INPUTMIX_ONTOP",   L"SIGNAL|SET_INPUTMIX_ONTOP",   L"Composite the input mix over the visualisation", L"signal" },
  { L"SET_INPUTMIX_OPACITY", L"SIGNAL|SET_INPUTMIX_OPACITY", L"Set the input-mix opacity", L"signal" },
};

// ---------------------------------------------------------------------------

bool EqualsNoCase(const wchar_t* a, const wchar_t* b) {
  return _wcsicmp(a, b) == 0;
}

// Compare a query against a table entry, tolerating the trailing '=' or '|'
// that the dispatcher matches on but a human would not type.
bool NameMatches(const IpcCommandDoc& d, const wchar_t* query) {
  std::wstring m(d.match);
  while (!m.empty() && (m.back() == L'=' || m.back() == L'|')) m.pop_back();
  return EqualsNoCase(m.c_str(), query);
}

std::wstring JsonEscape(const wchar_t* s) {
  std::wstring out;
  for (const wchar_t* p = s; *p; ++p) {
    switch (*p) {
      case L'"':  out += L"\\\""; break;
      case L'\\': out += L"\\\\"; break;
      case L'\n': out += L"\\n";  break;
      case L'\r': out += L"\\r";  break;
      case L'\t': out += L"\\t";  break;
      default:    out += *p;      break;
    }
  }
  return out;
}

}  // namespace

namespace ipc_help {

const IpcCommandDoc* Commands(size_t& count) {
  count = _countof(kCommands);
  return kCommands;
}

const IpcCommandDoc* ScriptCommands(size_t& count) {
  count = _countof(kScriptCommands);
  return kScriptCommands;
}

const IpcCommandDoc* SignalCommands(size_t& count) {
  count = _countof(kSignalCommands);
  return kSignalCommands;
}

// See MDROP_IPC_PROTOCOL in ipc_help.h for the rules this list follows. The
// short version: append-only, and a name ships with the thing it names.
static const wchar_t* const kFeatures[] = {
  // The list the Mixer tab actually draws, as distinct from the stored order:
  // MIXER_VIEW, terminated by MIXER_VIEW_END, and MIXER_ORDER_END on
  // MIXER_ORDER. Without it a client can only render the stored order, which
  // disagrees with the screen (forgejo#65).
  L"mixerview",
  // The arrangement token: MIXER_ORDER_REV, the optional rev= guard on
  // MIXER_ORDER_MOVE, and MIXER_ORDER_SET for pushing a whole arrangement.
  L"mixerrev",
  // MIXER_VIEW_CHANGED pushed to subscribers when the arrangement moves on its
  // own -- a failover device connecting, say.
  L"mixerpush",
  // battery= on MIXER_FADER, so a fader row can show a charge without joining
  // back to MIXER_DEVICE.
  L"mixerbattery",
  // chwindowsName= on MIXER_FADER: the Windows name when a short name has been
  // given, so a client has both and can tell that one was given at all.
  L"mixernames",
  // MIXER_GROUPS, MIXER_GROUP_SET, MIXER_GROUP_NUDGE and MIXER_GROUP_MUTE:
  // one key or one call moves every fader in a group. Announced as a feature
  // because it REPLACED the MIXER_SLOT verbs -- a client that finds this token
  // must not go on sending MIXER_SLOT_NUDGE and conclude the mixer is broken
  // when nothing moves.
  L"mixergroups",
  // DIAG_HOTKEYS, DIAG_HOTKEY_BIND and DIAG_HOTKEY_CONFLICTS: what is bound
  // globally and, separately, what Windows actually GRANTED. Worth gating on
  // because held=0 is the only way a client can tell a working global hotkey
  // from one another application owns -- the configured combination looks
  // identical either way (forgejo#72).
  L"hotkeydiag",
  // groups= on MIXER_FADER: which volume-hotkey groups (1-based, comma-
  // separated) this fader belongs to. MIXER_GROUPS already gives a group's
  // member count and its raw "channel|fader" keys, but not which fader on a
  // client's own screen those keys resolve to, so a remote could not mark
  // the faders its volume hotkeys will move (forgejo#77).
  L"mixerfadergroups",
  // DISPLAY_PROFILE_LIST/_SAVE=/_LOAD=/_DELETE=/_STARTUP[=]: display
  // profiles addressed by name instead of a full path, and a way to set the
  // startup default without loading or saving anything (forgejo#95).
  L"displayprofiles",
  // SET_ALL_FULLSCREEN=/GET_ALL_FULLSCREEN and fs= on GET_CHILDREN: the
  // walk-away control, and its restore point (forgejo#96).
  L"allfullscreen",
  // SET_IDLE_ACTIVE=/GET_IDLE_ACTIVE: trigger the idle timer's own action on
  // demand, independent of whether the automatic countdown is enabled.
  L"idleactive",
  // SET_MIRROR_WATERMARK=<0|1>: the absolute form, alongside the toggles.
  // Named because a client gates on it directly -- MilkDrop Remote drives its
  // "Watermark (all mirrors)" switch by reading the state and sending the
  // toggle only when the two disagree, which is a race it drops the moment it
  // finds this token (#196).
  L"mirrorwatermark",
  // LOWER_WINDOW exists alongside RAISE_WINDOW, so a client can offer "bring
  // to front" and "send to back" as a pair (#237). Announced because the raise
  // shipped long before the lower did: a client that finds only the raise must
  // hide the other button rather than offer one the PC will refuse.
  L"windowzorder",
  // Per-display watermark: SET_DISPLAY_WATERMARK addresses ONE display and
  // GET_DISPLAY_WATERMARK reads it back. Before this the verb refused anything
  // but the primary and there was no getter at all.
  L"displaywatermark",
  // SET_DISPLAY_MODE=<N>,preset -- a display holds its own preset IN THIS
  // PROCESS, between mirror and child. GET_DISPLAY_MODE reads the tier back
  // and reports auto=1 when the cost gate promoted a display rather than the
  // user asking. A client that finds this token can offer three tiers.
  L"displaytiers",
  // The idle timer is configurable from a remote rather than only switchable:
  // SET_IDLE_TIMEOUT=, SET_IDLE_ACTION=, SET_IDLE_PROFILE=,
  // SET_IDLE_FULL_OPACITY= and RESET_IDLE_TIMER, with the last two IDLE_TIMER=
  // fields appended for the profile and the opacity option. A client that finds
  // this token can offer the whole walk-away setup; one that does not still
  // reads the first three fields, which have not moved (#334).
  L"idleconfig",
  // The texture dimension cap is readable and settable: SET_TEXCAP= /
  // GET_TEXCAP. A client that finds this token can offer the control and show
  // the current value; one that does not simply never mentions it, and the
  // engine still applies whatever is in settings.ini (#349).
  L"texcap",
};

const wchar_t* const* Features(size_t& count) {
  count = _countof(kFeatures);
  return kFeatures;
}

std::wstring FeatureList() {
  std::wstring out;
  for (const wchar_t* f : kFeatures) {
    if (!out.empty()) out += L",";
    out += f;
  }
  return out;
}

std::wstring Identity(bool bChildMode) {
  wchar_t buf[512];
// The feature list is appended as a string, never formatted into the fixed
// buffer above. kFeatures is APPEND-ONLY by design, so its serialised length
// only ever grows, and formatting it with %s into wchar_t[N] means some future
// name silently crosses N and truncates. That is not a cosmetic clipping: it
// cut "mirrorwatermark" to "m" and took the closing quote with it, so HELP=JSON
// stopped being parseable JSON at all (#196). Appending cannot truncate.
  FormatTo(buf,
    L"VERSION=%s|build=%s|config=%s|arch=%s|protocol=%d|pid=%lu|child=%d"
    L"|commands=%d|script=%d|signals=%d",
    MDROP_VERSION_STRW,
    BuildTimestampW(),
#ifdef _DEBUG
    L"Debug",
#else
    L"Release",
#endif
#ifdef _WIN64
    L"x64",
#else
    L"x86",
#endif
    MDROP_IPC_PROTOCOL,
    (unsigned long)GetCurrentProcessId(),
    bChildMode ? 1 : 0,
    (int)_countof(kCommands),
    (int)_countof(kScriptCommands),
    (int)_countof(kSignalCommands));
  return std::wstring(buf) + L"|features=" + FeatureList();
}

namespace {

// One readable line per command, padded so the descriptions line up.
void AppendLines(std::wstring& block, const IpcCommandDoc* tbl, size_t n,
                 const wchar_t* group) {
  for (size_t i = 0; i < n; i++) {
    if (group && !EqualsNoCase(tbl[i].group, group)) continue;
    std::wstring usage(tbl[i].usage);
    if (usage.size() < 38) usage.append(38 - usage.size(), L' ');
    block += usage;
    block += L"  ";
    block += tbl[i].desc;
    block += L"\n";
  }
}

// Group the index into one message per category. A single message holding all
// ~215 lines would be ~40KB, and the client read buffer is 64KB -- close enough
// that it is not worth finding out. Every message is one whole category.
void BuildIndex(std::vector<std::wstring>& out, bool bChildMode) {
  out.push_back(Identity(bChildMode));
  out.push_back(std::wstring(
    L"HELP|MDropDX12 IPC commands. HELP=<command> for one entry, "
    L"HELP=<group> for one group, HELP=JSON for the machine-readable form."));

  // Categories in table order, without sorting the table itself.
  std::vector<std::wstring> groups;
  for (size_t i = 0; i < _countof(kCommands); i++) {
    const std::wstring g(kCommands[i].group);
    bool seen = false;
    for (auto& s : groups) if (s == g) { seen = true; break; }
    if (!seen) groups.push_back(g);
  }

  for (auto& g : groups) {
    std::wstring block = L"HELP|group=" + g + L"\n";
    AppendLines(block, kCommands, _countof(kCommands), g.c_str());
    out.push_back(block);
  }

  {
    std::wstring block =
      L"HELP|group=signal  (SIGNAL|<name>)\n";
    AppendLines(block, kSignalCommands, _countof(kSignalCommands), nullptr);
    out.push_back(block);
  }
  {
    std::wstring block =
      L"HELP|group=script  (anything this dispatcher does not match is passed "
      L"to the script engine, so these work over the pipe too)\n";
    AppendLines(block, kScriptCommands, _countof(kScriptCommands), nullptr);
    out.push_back(block);
  }
}

void BuildDetail(std::vector<std::wstring>& out, const wchar_t* query) {
  struct Src { const IpcCommandDoc* tbl; size_t n; const wchar_t* kind; };
  const Src srcs[] = {
    { kCommands,       _countof(kCommands),       L"ipc"    },
    { kSignalCommands, _countof(kSignalCommands), L"signal" },
    { kScriptCommands, _countof(kScriptCommands), L"script" },
  };

  bool found = false;
  for (const auto& s : srcs) {
    for (size_t i = 0; i < s.n; i++) {
      if (!NameMatches(s.tbl[i], query)) continue;
      found = true;
      std::wstring block = L"HELP|kind=";
      block += s.kind;
      block += L"|group=";
      block += s.tbl[i].group;
      block += L"\n";
      block += s.tbl[i].usage;
      block += L"\n  ";
      block += s.tbl[i].desc;
      block += L"\n";
      out.push_back(block);
    }
  }

  if (found) return;

  // A group name is the other thing a caller plausibly typed.
  std::wstring block = L"HELP|group=";
  block += query;
  block += L"\n";
  size_t before = block.size();
  AppendLines(block, kCommands, _countof(kCommands), query);
  AppendLines(block, kSignalCommands, _countof(kSignalCommands), query);
  AppendLines(block, kScriptCommands, _countof(kScriptCommands), query);
  if (block.size() > before) {
    out.push_back(block);
    return;
  }

  // Not a command and not a group. This is NOT the same as "invalid": the
  // dispatcher passes anything it does not match to the script engine, so a
  // caller must not read this as a rejection.
  std::wstring miss = L"HELP|unknown=";
  miss += query;
  miss += L"\n  Not in the command index. Note that unmatched commands are "
          L"passed to the script engine rather than rejected, so this does not "
          L"mean the command does nothing.";

  // Nearest matches by shared prefix, longest first.
  std::wstring best;
  size_t bestLen = 0;
  for (const auto& s : srcs) {
    for (size_t i = 0; i < s.n; i++) {
      size_t k = 0;
      while (query[k] && s.tbl[i].match[k] &&
             towupper(query[k]) == towupper(s.tbl[i].match[k])) k++;
      if (k >= 3 && k > bestLen) { bestLen = k; best = s.tbl[i].usage; }
    }
  }
  if (!best.empty()) {
    miss += L"\n  Closest entry: ";
    miss += best;
  }
  out.push_back(miss);
}

void BuildJson(std::vector<std::wstring>& out, bool bChildMode) {
  // Chunked: the whole document is ~35KB of wchar_t and one message must stay
  // clear of the 64KB client read buffer.
  //
  // Every chunk carries its own index and the header states how many to
  // expect, because a client's queue can hold messages from an EARLIER batch
  // -- a stale STATE reply landed in the middle of a HELP read during
  // testing. Concatenating blindly would have produced silently corrupt JSON;
  // this way a caller filters on the prefix, sorts by index, and can tell that
  // it has all the parts.

  std::wstring doc;
  doc += L"{\"version\":\"" MDROP_VERSION_STRW L"\"";

  wchar_t head[256];
// The feature list is appended as a string, never formatted into the fixed
// buffer above. kFeatures is APPEND-ONLY by design, so its serialised length
// only ever grows, and formatting it with %s into wchar_t[N] means some future
// name silently crosses N and truncates. That is not a cosmetic clipping: it
// cut "mirrorwatermark" to "m" and took the closing quote with it, so HELP=JSON
// stopped being parseable JSON at all (#196). Appending cannot truncate.
  FormatTo(head,
    // BuildTimestampW, not __DATE__/__TIME__: those report when THIS file was
    // compiled, and this one had not been rebuilt for two days while the exe
    // was minutes old.
    L",\"buildDate\":\"%s\",\"config\":\"%s\",\"arch\":\"%s\""
    L",\"protocol\":%d,\"pid\":%lu,\"child\":%s",
    BuildTimestampW(),
#ifdef _DEBUG
    L"Debug",
#else
    L"Release",
#endif
#ifdef _WIN64
    L"x64",
#else
    L"x86",
#endif
    MDROP_IPC_PROTOCOL,
    (unsigned long)GetCurrentProcessId(),
    // Reports the truth again (#230). While the child subsystem was deleted
    // this was hardcoded "false"; children are back for flagged presets, and a
    // client that cannot tell a child from the primary cannot tell which
    // instance is showing which preset -- which is the whole point of bringing
    // them back.
    //
    // features stays appended below rather than being a format argument: the
    // pre-deletion version passed it here, and this format string no longer
    // has a slot for it.
    bChildMode ? L"true" : L"false");
  doc += head;
  doc += L",\"features\":\"";
  doc += FeatureList();
  doc += L"\"";

  struct Src { const IpcCommandDoc* tbl; size_t n; const wchar_t* key; };
  const Src srcs[] = {
    { kCommands,       _countof(kCommands),       L"commands" },
    { kSignalCommands, _countof(kSignalCommands), L"signals"  },
    { kScriptCommands, _countof(kScriptCommands), L"script"   },
  };
  for (const auto& s : srcs) {
    doc += L",\"";
    doc += s.key;
    doc += L"\":[";
    for (size_t i = 0; i < s.n; i++) {
      if (i) doc += L",";
      doc += L"{\"match\":\"";
      doc += JsonEscape(s.tbl[i].match);
      doc += L"\",\"usage\":\"";
      doc += JsonEscape(s.tbl[i].usage);
      doc += L"\",\"desc\":\"";
      doc += JsonEscape(s.tbl[i].desc);
      doc += L"\",\"group\":\"";
      doc += JsonEscape(s.tbl[i].group);
      doc += L"\"}";
    }
    doc += L"]";
  }
  doc += L"}";

  const size_t kChunk = 8000;
  const size_t parts = (doc.size() + kChunk - 1) / kChunk;

  wchar_t begin[64];
  FormatTo(begin, L"HELP_JSON_BEGIN|parts=%d", (int)parts);
  out.push_back(begin);

  for (size_t i = 0; i < parts; i++) {
    wchar_t hdr[48];
    FormatTo(hdr, L"HELP_JSON|%d|", (int)i);
    out.push_back(std::wstring(hdr) + doc.substr(i * kChunk, kChunk));
  }
}

}  // namespace

bool Handle(const wchar_t* msg, bool bChildMode,
            std::vector<std::wstring>& reply) {
  if (!msg || !*msg) return false;

  // GET_VERSION, and a bare VERSION by EXACT match only. VERSION= is the
  // script command that sets vis_version, and a prefix match here would eat
  // it -- claiming a new keyword narrows the script fallback.
  if (_wcsicmp(msg, L"GET_VERSION") == 0 || _wcsicmp(msg, L"VERSION") == 0) {
    reply.push_back(Identity(bChildMode));
    return true;
  }

  const wchar_t* arg = nullptr;
  if (_wcsnicmp(msg, L"HELP", 4) == 0) {
    if (msg[4] == L'\0')      arg = L"";
    else if (msg[4] == L'=')  arg = msg + 5;
    else return false;                       // HELPFUL... is not ours
  } else if (_wcsicmp(msg, L"COMMANDS") == 0 ||
             _wcsicmp(msg, L"LIST_COMMANDS") == 0) {
    arg = L"";
  } else {
    return false;
  }

  if (!*arg)                             BuildIndex(reply, bChildMode);
  else if (_wcsicmp(arg, L"JSON") == 0)  BuildJson(reply, bChildMode);
  else                                   BuildDetail(reply, arg);
  return true;
}

}  // namespace ipc_help
