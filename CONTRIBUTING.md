# Contributing to MDropDX12

Contributions are welcome, and this file exists because the repository's shape
is unusual enough that you deserve to know what you are looking at before you
spend an evening on it.

## What this repository is

MDropDX12 is developed in a private repository and **published here as a
snapshot, one commit per release.** So:

- The history here is one commit per version, not per change. The development
  repository's commit-by-commit history is not published.
- That history is **append-only**. Each release is parented on the one before
  it, so a commit you fork from stays reachable, `git diff` between releases
  works, and a pull request has a stable base.

### Why the history starts at v3.0.0

Because everything before it had to be destroyed, and that was the right call.

The repository carried a large number of preset files contributed by the
MilkDrop community over two decades. They are other people's work, most under
no license that permits redistribution, and they were sitting in the published
history where anyone could fetch them at any older commit. Removing them from
the tip would not have helped — git keeps every version of every file, so they
stayed one `git checkout` away.

The fix was to stop publishing history at all: each release replaced `main`
with a single parentless commit, so nothing older than the current release
existed here to fetch. That solved the redistribution problem completely, and
it is why v3.0.0 has no ancestors.

It also made the repository hostile to contributors, which was not intended and
was not noticed for a while. A fork's base commit ceased to exist at the next
release; `git log` showed one commit; a pull request had nothing stable to
target, and anything merged was erased by the next publish.

From v3.1 onward both hold at once. Each release is a cleaned snapshot parented
on the release before it, so history accumulates going forward while nothing
before v3.0.0 is reachable. The presets are gone and they are staying gone; the
history is real from here.

If you looked at this project before and concluded it was not open to
contributions, that was a reasonable conclusion. It is no longer true.

## Pull requests

Open them against `main` here. They are read.

Because development happens upstream, a merged pull request is **imported into
the development repository** rather than flowing backwards automatically. Your
commit, message and authorship are preserved through that import, and the
change appears in the next published release. The publishing job refuses to run
while there is a merged pull request it has not imported, so nothing you
contribute can be quietly overwritten.

Practical consequences worth knowing:

- A pull request may sit until the next release cycle. That is the pipeline,
  not disinterest.
- If a change is later modified upstream, the published snapshot reflects the
  upstream version. The history here will not show that as a revert.
- For anything large, open an issue first. It is a rebuild of MilkDrop2 with
  specific compatibility obligations, and a big change that cuts against one of
  them is painful to unwind for everyone.

## Building

`docs/Development.md` has the full setup: prerequisites, first-time setup, the
build command, and the output layout. The short version:

```powershell
powershell -ExecutionPolicy Bypass -File build.ps1 Release x64
```

Windows, x64 only, C++17, MSVC. `.vscode/` is checked in with a working C/C++
configuration.

## Reporting bugs

Open an issue. What helps most, roughly in order:

1. The preset that triggers it, attached. "A preset with a comp shader" is not
   reproducible; the `.milk` file is.
2. Your display arrangement, if it involves more than one monitor. A great many
   of this project's bugs have turned out to be mirror-related.
3. `log/debug.log`, with `LogLevel=3` or `4` in `settings.ini` — it records
   preset loads, shader compile failures and GPU device removals.
4. GPU and driver version for anything that looks like corruption or a hang.

## What the code expects of you

The house style is visible in any file you open, and matching it matters more
than any rule this file could list. Two things that are not obvious:

- **Comments explain why, not what.** Several comments in this codebase exist
  specifically to stop a future change from re-introducing a bug that was
  expensive to find. If you are changing code under one of those, read it
  first — and if it turns out to be wrong, correct it rather than working
  around it.
- **Measured, not assumed.** Where a comment cites a measurement, the
  measurement was taken. If you change the behaviour it describes, either
  re-measure or say plainly that you did not.

## License

CC-BY-NC 4.0 (Creative Commons Attribution-NonCommercial 4.0 International).
Third-party components keep their own licenses — see
`THIRD-PARTY-LICENSES.txt`. By contributing you agree your contribution is
licensed on the same terms.
