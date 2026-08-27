#pragma once
// Transforms between a .milk file's on-disk text and the Preset Editor's
// "block" form.
//
// A .milk stores every code line as its own numbered key:
//
//     per_frame_init_1=n = 0;
//     per_frame_init_2=loop (100000, megabuf(n)=0; n=n+1);
//
// which is unreadable and unwritable by hand.  Block form lifts each run of
// those into a named section and drops the prefix:
//
//     [per_frame_init]
//     n = 0;
//     loop (100000, megabuf(n)=0; n=n+1);
//
// Everything else -- the loose header lines, [preset00], and every scalar
// parameter -- passes through untouched, so the two forms carry exactly the
// same information and BlocksToMilk(MilkToBlocks(x)) == x.
//
// Free of Win32 so tools/lexer-test can round-trip the whole preset library.
#include <string>

namespace mdrop {

// .milk text -> block form.
std::string MilkToBlocks(const std::string& milk);

// Block form -> .milk text.  Exact inverse of MilkToBlocks.
std::string BlocksToMilk(const std::string& blocks);

} // namespace mdrop
