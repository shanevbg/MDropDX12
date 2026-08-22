// preset_hash.h — content identity for preset files.
//
// A preset's identity is the hash of its NORMALIZED body, not its filename and
// not its raw bytes.  That lets the same preset keep its annotations wherever
// it sits on disk, and keeps its identity stable across the edits that other
// programs make to preset files without changing what the preset renders.
//
// THE NORMALIZATION RULE IS A FILE FORMAT, NOT AN IMPLEMENTATION DETAIL.
//
// The hash is the key into presets.json.  Changing the rule below -- even by
// one excluded key -- changes every hash and silently detaches every stored
// entry from the preset it describes: ratings, tags, notes and play counts
// stop being found.  Recovery means re-reading every preset file that is still
// reachable on disk, and anything since deleted or moved to another drive is
// unrecoverable.  If it ever must change, the old algorithm has to stay in the
// code permanently.
//
// This is why the rule is five steps and no more.  Every plausible refinement
// (also ignore comments, also ignore intra-line whitespace, also sort keys) is
// defensible on its own, changes every hash, and adds a place where two
// implementations can silently disagree -- which matters, because the Python
// test harness pins this behaviour over IPC.
//
// The rule, in full:
//
//   1. Split the bytes on '\n'; drop a trailing '\r' from each line.
//   2. Drop any line whose key -- the text before the first '=', ignoring
//      leading whitespace, compared case-insensitively -- is fRating.
//   3. Strip trailing spaces and tabs from every line.
//   4. Drop leading and trailing blank lines.  Interior blank lines are kept.
//   5. Join with '\n' and hash the result with FNV-1a 64.
//
// fRating is excluded because other MilkDrop-family programs write it back
// into preset files; without the exclusion, rating a preset elsewhere would
// mint a new identity here.  It is the only exclusion.
//
// MD31/MD32 were excluded until 2026-08-20, on the reasoning that the
// measurement harness strips them to force MilkDrop 3 down the standard
// rendering path and that stripping should not move identity.  They now count.
// The value selects which cached shader MilkDrop 3 PRO renders, so two files
// with one body and two MD31 values are two different presets, and the harness
// wanting a stripped copy to look like the original is the harness's problem to
// solve -- it can keep the original path, which it already does.  There were no
// releases between the hash landing and this change, so nothing shipped was
// keyed the old way; the exclusion is gone rather than kept as a fallback.

#pragma once

#include <string>

namespace mdrop {

// Returns 16 lowercase hex digits, or "" if the input is empty.
std::string ComputePresetHashFromBytes(const char* data, size_t len);

// Reads the file and hashes it.  Returns "" if the file cannot be read.
std::string ComputePresetHashFile(const wchar_t* path);

}  // namespace mdrop
