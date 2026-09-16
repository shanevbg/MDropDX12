// version.h — Single source of truth for MDropDX12 version number.
// Safe for both C++ and RC compiler (no C++ constructs).
#ifndef MDROP_VERSION_H
#define MDROP_VERSION_H

#define MDROP_VERSION_MAJOR  3
#define MDROP_VERSION_MINOR  2
#define MDROP_VERSION_PATCH  0

// The strings are BUILT from the numbers above, so bumping a version means
// editing one place rather than three that must agree (issue 19). Cheap
// insurance rather than a live bug -- issue 1 records that these never actually
// drifted across 20 bumps -- but the failure mode is a release that reports the
// wrong version, which is exactly the sort of thing nobody checks.
//
// Two-step expansion is required: the helper macro is what forces the argument
// to be expanded before it is stringified, otherwise MDROP_VERSION_STR comes
// out as the literal text "MDROP_VERSION_MAJOR".
#define MDROP_STR_(x)   #x
#define MDROP_STR(x)    MDROP_STR_(x)
#define MDROP_WSTR_(x)  L ## #x
#define MDROP_WSTR(x)   MDROP_WSTR_(x)

#define MDROP_VERSION_STR   MDROP_STR(MDROP_VERSION_MAJOR)  "." MDROP_STR(MDROP_VERSION_MINOR)  "." MDROP_STR(MDROP_VERSION_PATCH)
#define MDROP_VERSION_STRW  MDROP_WSTR(MDROP_VERSION_MAJOR) L"." MDROP_WSTR(MDROP_VERSION_MINOR) L"." MDROP_WSTR(MDROP_VERSION_PATCH)

#endif
