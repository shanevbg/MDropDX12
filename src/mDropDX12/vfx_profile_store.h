// vfx_profile_store.h — named Video Effects profiles on disk.
//
// Every profile lives in ONE file, resources/vfxprofiles.json, keyed by name:
//
//   { "version": 1,
//     "profiles": { "<name>": { transform, color, effects,
//                               rendering, blendMode, audio } } }
//
// A profile is a NAME, not a path. There is no file per profile and no live
// state file. Nothing here is written unless a profile is explicitly saved,
// and nothing is read back unless something asks for a profile by name --
// which is the rule the previous design broke, restoring parameters at every
// startup with no setting governing it and no profile in play.
//
// This is deliberately NOT part of Engine, for the same reason
// shader_overrides.h is not: Engine is already ~1,900 lines of header with
// ~1,000 members spread over 23 translation units, and a store that only
// touches a file has no business being in it. It lived in
// engine_spout_input.cpp because the video effects code already did.
//
// The store has no Engine dependency at all, and deliberately does not include
// render_tunables.h either. RenderTunable holds an `int Engine::*`, and MSVC
// sizes a pointer-to-member of an INCOMPLETE class using the fully-general
// representation: 12 bytes where a complete Engine gives 4, making
// sizeof(RenderTunable) 56 here and 48 in every TU that includes engine.h.
// Indexing the shared array then strides by the wrong amount and reads
// garbage. So the caller hands the tunable names in through SetTunables()
// rather than this file ever naming the table.
//
// Every read parses the file and every write re-emits it. That is a few
// hundred bytes on a user action, and it keeps the file on disk as the single
// source of truth rather than a cache that can drift from a copy held in
// memory. A rewrite puts back any member it does not recognise, so the format
// can gain sections later without an older build erasing them.

#pragma once

#include <string>
#include <utility>
#include <vector>

#include "video_effect_params.h"

// A VFX profile is a key inside vfxprofiles.json rather than a filename, so
// what it needs to hold is a name, not a path.
#define MAX_VFX_PROFILE_NAME 128

namespace mdrop {

// One profile's worth of settings, as they travel between the engine's live
// state and the file.
//
// The render tunables come along because the same look usually needs the same
// glow and rib widths, and the profile's Save button is the only place they
// are saved from. They are carried as values with a presence flag rather than
// applied here, because applying one also writes its INI key -- an engine-side
// side effect the store should not own.
// One render tunable, as the store needs to see it: a key to read and write,
// and the default to fall back on. Deliberately no pointer-to-member -- see
// the note above.
struct VFXTunableSpec {
    const wchar_t* name;
    int            defValue;
};

struct VFXProfileData {
    static const int kMaxTunables = 8;   // sized past the current three

    VideoEffectParams fx;
    int  tunables[kMaxTunables] = {};
    bool tunablePresent[kMaxTunables] = {};
};

class VFXProfileStore {
public:
    // resourceDir is the directory holding vfxprofiles.json (m_szMilkdrop2Path).
    // Set once at startup, after the base directory is known.
    void SetResourceDir(const wchar_t* dir);

    // The render tunables this build has, in the order VFXProfileData indexes
    // them. Copied, so the caller's table need not outlive the call. Must be
    // set before any profile is read or written, or tunables are silently
    // absent from every profile.
    void SetTunables(const VFXTunableSpec* specs, int count);
    void GetStorePath(wchar_t* out, size_t len) const;

    void Names(std::vector<std::wstring>& out) const;
    bool Exists(const wchar_t* name) const;

    bool Save(const wchar_t* name, const VFXProfileData& d);

    // Reads a profile INTO an existing VFXProfileData, and only overwrites
    // what the stored profile actually contains.
    //
    // Seed `inout` with the caller's current values before calling. A profile
    // written before a section existed leaves that section's parameters where
    // they were rather than snapping them to defaults -- which is the whole
    // reason this is an in/out parameter and not a return value.
    //
    // tunablePresent[] is cleared first and set only for tunables the file
    // names, so the caller can tell "absent, leave alone" from "stored as
    // this value". blendMode is the one exception to leave-alone: absent means
    // 0, as it always has.
    bool Load(const wchar_t* name, VFXProfileData& inout) const;

    bool Delete(const wchar_t* name);

    // ── Importing files this build did not write ────────────────────────
    //
    // Compatibility is a feature. A visualiser that reads only the exact file
    // its own version produced makes every upgrade a small loss, so this
    // accepts every shape video effect settings have been stored in here:
    //
    //   * settings.ini with a [VideoFX] section  (before the parameters left the INI)
    //   * videofx/<name>.json, videofx/current.json  (one profile per file)
    //   * vfxprofiles.json  (a whole store, from another install)
    //   * either of the above wrapped in a "videoFX" object
    //
    // identified by CONTENT, not by filename, so a renamed or hand-edited file
    // still comes in.

    // How many profiles are in there, without changing anything. Suggests a
    // name for the single-profile case (those files carry parameters but no
    // name). Returns -1 if the file cannot be read.
    int PeekImport(const wchar_t* path, std::wstring& suggestedName) const;

    // Stores them. Named profiles keep their names and are given a unique
    // " (2)" suffix rather than overwriting anything; a single unnamed one
    // goes in under nameForSingle, replacing it if it exists -- by then the
    // user has been asked. Returns the count, or -1 if the file cannot be read.
    int Import(const wchar_t* path, const wchar_t* nameForSingle);

    // Folds a pre-existing videofx/ directory into the store, once. Each
    // <name>.json becomes a profile of that name; current.json is dropped
    // rather than imported, because it held live state and not something
    // anybody chose to keep. The directory is read, never deleted.
    //
    // "once" is recorded in the store as importedFromVideoFXFolder rather than
    // inferred from whether the names are already present: deleting an
    // imported profile would otherwise bring it straight back on the next
    // start, since the file it came from is still there.
    void MigrateFolder();

private:
    std::wstring m_resourceDir;
    std::vector<std::pair<std::wstring, int>> m_tunables;   // name, default
};

}  // namespace mdrop
