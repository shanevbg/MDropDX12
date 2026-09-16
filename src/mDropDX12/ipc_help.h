#pragma once
// ipc_help.h — the IPC command index, so a running copy can describe itself.
//
// forgejo#21. Every external consumer — Milkwave Remote, the MCP server, the
// milk2-probe harness, third-party scripts — used to learn the command set from
// docs/Scripts.md and then hope the binary it was talking to was the one those
// docs described. Two thirds of the dispatched keywords were undocumented when
// this was written, and nothing reported the app's own version over the pipe.
//
// The table below is the answer to both. It is hand-maintained, and
// tools/milk2-probe/test_ipc_help.py fails the moment the dispatcher and the
// table disagree, so it cannot quietly rot the way the document did.
//
// NOTE ON UNKNOWN COMMANDS: LaunchMessage deliberately passes anything it does
// not match to ExecuteScriptLine, because Milkwave Remote depends on it. So
// "absent from this table" NEVER means "invalid" — the script vocabulary
// reachable through that fallback is listed here too, in kScriptCommands.

#ifndef IPC_HELP_H
#define IPC_HELP_H

#include <string>
#include <vector>
#include <cstddef>

// ── What a client is allowed to gate on ──────────────────────────────────
//
// BUMP THIS FOR ANY CHANGE A CLIENT CAN DEPEND ON, INCLUDING AN ADDITIVE ONE.
//
// It used to read "bumped when an existing command's contract changes", and by
// that rule it was correctly 1 for the whole of the v3 work -- new commands
// change no existing contract, an added field breaks no tolerant parser, and an
// optional trailing argument is backwards compatible by construction. So the
// number stood still while the surface grew, and nobody forgot: the rule said
// not to move it (forgejo#74).
//
// That is backwards. A client does not need to detect BREAKAGE -- a breaking
// change is the case where the old client is already broken and no number saves
// it. What a client needs to detect is ADDITION: has this build got the thing I
// want to use? So the number moves whenever the answer to that question
// changes.
//
//   1  the original surface, through the v2.11.0 release
//   2  the mixer view, the arrangement token, and both fader fields
//   3  the hotkey diagnostics: what is bound globally, and what is HELD
//   4  groups= on MIXER_FADER (forgejo#77)
//   5  named display-profile IPC, and DISPLAY_PROFILE_STARTUP (forgejo#95)
//   6  SET_ALL_FULLSCREEN=/GET_ALL_FULLSCREEN, fs= on GET_CHILDREN (forgejo#96)
//   7  SET_IDLE_ACTIVE=/GET_IDLE_ACTIVE -- trigger the idle action on demand
//   8  SET_VSYNC=/GET_VSYNC -- the one render setting with no IPC form, which
//      made every automated frame-rate measurement read the monitor rather
//      than the engine, because it defaults on (#121)
//   9  SET_MIRROR_WATERMARK= -- the absolute form for mirror watermark, so a
//      remote can drive a switch instead of reading, comparing and toggling
//      (#196)
//  10  the idle timer is configurable and resettable from a remote:
//      SET_IDLE_TIMEOUT=, SET_IDLE_ACTION= (now 0-3, the fourth loading a
//      display profile), SET_IDLE_PROFILE=, SET_IDLE_FULL_OPACITY= and
//      RESET_IDLE_TIMER. IDLE_TIMER= carries two more fields, appended
//      (#334)
//
//  11  SET_TEXCAP=/GET_TEXCAP -- the longest edge a texture loaded from disk
//      keeps. Bumped rather than slipped in quietly because it is a SETTING a
//      client can offer, not a diagnostic: it changes what the engine puts on
//      the GPU and a remote can reasonably draw a control for it (#349)
//
#define MDROP_IPC_PROTOCOL 11

// ── Named capabilities, for a client that degrades per FEATURE ───────────
//
// A single number forces a client to treat a build as all-or-nothing, which is
// not how they behave: MilkDrop Remote has an independent fallback for the
// drawn order, the token, the battery field and the push, and will happily use
// three of the four. A list also says what it means, where `protocol=7` does
// not.
//
// Rules for this list, because a list that cannot be trusted is worse than no
// list at all:
//
//   * It is APPEND-ONLY. A name, once shipped, keeps its meaning forever --
//     clients in the wild are testing for that exact string. Retiring a
//     capability means shipping a new name, never redefining an old one.
//   * A name means "this build answers these verbs and fields". It says
//     nothing about whether the subsystem is enabled or whether hardware is
//     present, which are runtime questions with runtime answers.
//   * ABSENCE means not declared, which is not the same as not present. Only
//     capabilities a client is expected to gate on are listed; the rest of the
//     surface is discovered from HELP as it always was.
//
// Add a name in the same commit that adds the thing it names, and bump
// MDROP_IPC_PROTOCOL alongside it.

struct IpcCommandDoc {
  const wchar_t* match;  // EXACTLY the literal the dispatcher matches on
  const wchar_t* usage;  // display form including arguments
  const wchar_t* desc;   // one line, no trailing period
  const wchar_t* group;  // category, for grouping the index
};

namespace ipc_help {

// The capability names this build declares, in table order.
const wchar_t* const* Features(size_t& count);

// Them, comma-separated, as they appear in Identity().
std::wstring FeatureList();

// The three vocabularies a caller can reach over the pipe.
const IpcCommandDoc* Commands(size_t& count);        // explicitly dispatched
const IpcCommandDoc* ScriptCommands(size_t& count);  // reached via the fallback
const IpcCommandDoc* SignalCommands(size_t& count);  // SIGNAL|<name>

// One line: version, build, protocol, pid.
std::wstring Identity(bool bChildMode);

// Handles HELP, HELP=<name>, HELP=<group>, HELP=JSON, COMMANDS,
// LIST_COMMANDS, GET_VERSION and an exact-match VERSION.
//
// Returns false if `msg` is none of those, leaving `reply` untouched, so the
// caller falls through to the rest of the dispatcher unchanged. On true,
// `reply` holds the messages to send in order; the caller appends END_BATCH.
bool Handle(const wchar_t* msg, bool bChildMode, std::vector<std::wstring>& reply);

}  // namespace ipc_help

#endif  // IPC_HELP_H
