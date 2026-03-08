# MDropDX12 v1.7.0

## What's New

- **FFT EQ smoothing & peak hold** — Smoothed FFT spectrum with configurable attack/decay and peak hold for shader `get_fft()` / `get_fft_peak()` functions. Milkwave EQ visualization presets now work out of the box.
- **Milkwave Remote FFT control** — `FFT_ATTACK=` and `FFT_DECAY=` IPC commands from Milkwave Remote, with values reported back in status response and persisted to INI.
- **Bootstrap upgrade** — First-run dialog now uses TaskDialogIndirect with four options: choose workspace, import settings from existing install, continue here, or exit.
- **Error Display Settings** — New ToolWindow for configuring shader error notification appearance (Normal/LOUD modes, font size, duration, colors).
- **DPI-aware dialogs** — ModalDialog base class with BaseLayout for consistent metrics across display scaling.

## Bug Fixes

- Fixed DX9 caps garbage causing "PSVersion=0x2" shader model error on DX12
- Fixed `sampler_audio` declaration stripped by shader preprocessor, breaking `get_fft()` functions
- Fixed `UpdateAudioTexture()` not called in regular render path (only Shadertoy)
- Fixed FFT scaling factor producing near-zero values (0.00035f → 2.0f for MDropDX12's FFT range)
- Fixed TaskDialogIndirect crash (ordinal 345) — Common Controls v6 manifest now embedded
- Fixed ModalDialog DPI sizing

## Installation

Download `MDropDX12-v1.7.0-Portable.zip`, extract to any folder with write access, and run `MDropDX12.exe`. No installer or admin privileges required. No VC++ Redistributable needed.

Press **F8** to open Settings. Press **F1** for keyboard shortcuts.

## Full Changelog

See [docs/Changes.md](https://github.com/shanevbg/MDropDX12/blob/v1.7/docs/Changes.md) for the complete list of changes.
