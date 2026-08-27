# Settings

Everything MDropDX12 remembers between runs goes through one class,
`mdrop::ConfigStore` (`src/mDropDX12/config_store.h`). This page is about why it
looks the way it does, because the shape of it is the interesting part.

## The problem it replaced

MDropDX12 inherited MilkDrop's approach to configuration: call the Win32 profile
API wherever a value is needed.

```cpp
WritePrivateProfileStringW(L"Settings", L"bShowFPS", L"1", pIni);
```

That is fine at ten call sites. It had grown to roughly nine hundred, spread over
thirty files, in three different argument orders — because the project had added
its own `Int` and `Float` helpers, and they took their arguments **backwards**
from the Win32 function they wrapped:

```cpp
WritePrivateProfileStringW(section, key, value, file);   // Win32 order
WritePrivateProfileIntW   (value, key, file, section);   // ours — backwards
WritePrivateProfileFloatW (value, key, file, section);   // ...also backwards
```

Mixing them up compiles cleanly and writes a key named after the value.

The deeper problem was that nine hundred doors mean no doorman. There was
nowhere to stand to answer:

- *Is this write allowed right now?* (a test run was rewriting the user's
  configuration as a side effect of what it was testing)
- *Have we already written exactly this?* (`WritePrivateProfileString` rewrites
  the **whole file** on every call)
- *Can this wait a moment?* (it could not; the writes were blocking)

## The shape

```cpp
Config().SetInt(L"Settings", L"bShowFPS", m_bShowFPS);
m_bShowFPS = Config().GetBool(L"Settings", L"bShowFPS", false);
```

Section first, then key, then the value or its fallback — the same order the
file reads in. One order, everywhere.

| Call | What it gets you |
|---|---|
| `Config()` | the app's own `settings.ini` |
| `ConfigFile(path)` | any other `.ini` — messages, sprites, an exported profile |
| `GetInt` / `GetBool` / `GetFloat` / `GetString` / `GetStringTo` | reads, fallback last |
| `SetInt` / `SetBool` / `SetFloat` / `SetString` / `Remove` | writes |
| `store.Flush()` / `ConfigFlushAll()` / `ConfigShutdown()` | make it real on disk, now |

`GetStringTo` has an overload that takes the destination **array** rather than a
pointer and a count, because nine call sites used to pass `sizeof(buf)` — and
`sizeof` a `wchar_t` array is twice its length in characters. The overload works
the count out from the type, so the mistake is no longer expressible.

## It is a write-back cache

This is the part that matters most, and the reason an earlier attempt at
unifying `.ini` access had to be abandoned.

`Set*` does not touch the file. It updates the value in memory, marks it dirty,
and returns. A single background thread pushes dirty keys out about once a
second. A value written a hundred times inside that window — a slider being
dragged, a window being moved, a loop of IPC commands — costs **one** file
write, and no caller ever waits on disk.

Reads come from the same in-memory mirror, so a read straight after a write sees
the new value; the buffering is invisible to the rest of the app. The mirror is
dropped whenever the file's timestamp or size changes underneath us, so an
`.ini` edited by something else is still picked up.

The buffer window is also a data-loss window. It is deliberately short, it is
flushed on the way out (`ConfigShutdown()`, from `WinMain`), and anything that
hands a file to another process should call `Flush()` first.

## The write shield, and testing mode

Automated tests drive the app over its named pipe, and they change settings —
that is usually the point of the test. Those changes used to land in the user's
real `settings.ini`, so a test run quietly rewrote the configuration of the
machine it ran on. The worst case was the frame-rate pin: entering testing mode
calls `SetFPSCap(60)`, which persists the cap, and a sweep normally ends by
killing the process, so the value that was supposed to be restored never was.
The user stayed capped at 60 permanently.

`Engine::SetTestingMode(true)` now raises a **write shield** before it does
anything else. While it is up:

- every write goes into a separate in-memory overlay;
- reads consult the overlay first, so code under test behaves exactly as it
  would normally;
- nothing is marked dirty, so nothing reaches disk;
- lowering the shield discards the overlay — there is nothing to undo.

Two deliberate ways out, because "no writes at all" is too blunt:

- `mdrop::ConfigWriteOverride` — an RAII guard for a write the user genuinely
  asked for. Thread-scoped, so one thread opting in never lets another thread's
  incidental writes through.
- `[Milkwave] TestingModeWritesSettings=1`, or `TESTING_MODE=1,persist` over the
  pipe, which tells the engine not to raise the shield at all.

A session that has used testing mode also **skips the shutdown auto-save**. That
save is the app deciding on its own to persist 117 keys read off live state,
including the render window's geometry — which every harness moves. Nothing a
person did is lost by skipping it, because settings controls persist when they
are changed, not at shutdown.

`DIAG_CONFIG` over the pipe reports `sets`, `writes`, `elided` and `shielded`,
so a test can prove a run left the file alone.

## Where settings live: file or registry

`ConfigStore` is the API; `ConfigBackend` (`config_backend.h`) is the storage
under it, and there are two.

| | |
|---|---|
| **`.ini` files** | beside the executable. Portable: the app is a folder you can copy, settings included. The default. |
| **the registry** | `HKEY_CURRENT_USER\Software\MDropDX12\<file>\<section>`, values as `REG_SZ`. Settings live with the user rather than with the build directory. |

The choice is one flag in `useregistry.ini` beside the executable:

```ini
[Settings]
UseRegistry=1
```

A separate file on purpose — it has to be read before the settings system
exists, so it cannot be a setting itself. The app writes it out, commented, on
first run.

Switching to the registry **copies the existing `.ini` across the first time**,
so flipping the flag never silently resets a configuration. Going the other way,
`CONFIG_EXPORT_INI` over the pipe writes the registry back into the files.

Only the app's own three files follow the flag — `settings.ini`, `messages.ini`,
`sprites.ini`, declared with `RegisterManagedConfigFile()` where their paths are
built. A file the user picked in a save dialog is always a real file, because
the point of exporting a message profile is to have a file to hand to somebody.

## One inherited quirk worth knowing

`GetPrivateProfileIntW` is **documented** to report zero for a negative value in
the file. It does not: it returns a `UINT` whose bit pattern is the negative
number, which every caller assigns into an `int` and gets back correctly. That
is worth measuring rather than believing, because a "fix" for the documented
behaviour changes how settings like `[Settings] MaxPSVersion=-1` are read, and
that one decides whether pixel shaders are used at all.

## Adding a setting

1. Read it in `Engine::MyReadConfig` (`engine.cpp`) with a sensible fallback.
2. Write it wherever the user changes it — immediately, not at shutdown.
3. That is all. Buffering, thread safety, testing mode and the registry are the
   store's problem, not yours.
