# MDropDX12 v1.7.1

## What's New

- **FFT accuracy fix** — Separated shader FFT from beat detection FFT. EQ visualization presets no longer over-accentuate high frequencies. Shader `get_fft()` / `get_fft_hz()` now match Milkwave's output.
- **iDate shader support** — Wall clock time available in shaders as `_c19` (year, month, day, seconds since midnight). Works in both .milk comp shaders and .milk3 Shadertoy presets.
- **Simple preset name display** — New default mode shows preset name using decorative font settings (adjustable size and color). No animation effects — clean and readable.
- **Shadertoy supertexts** — Preset name, song title, and user sprites now render in .milk3 Shadertoy mode.
- **Preset browser navigation** — Single-click on directory entries navigates in/out.
- **HUD song title auto-shrink** — Long titles auto-shrink to fit the overlay width.
- **Stability** — Suppressed "no presets found" nag when a preset is already playing.

## Bug Fixes

- Fixed FFT over-accentuating high frequencies (was using equalized beat-detection FFT for shader texture)
- Fixed `sampler_rand` stripping producing false positives when presets reference sampler by name only
- Fixed "no presets found" error appearing during preset list rebuild while a preset is already playing
- Fixed animation profiles not persisting when Text Animations window is closed

## Installation

Download `MDropDX12-v1.7.1-Portable.zip`, extract to any folder with write access, and run `MDropDX12.exe`. No installer or admin privileges required. No VC++ Redistributable needed.

Press **F8** to open Settings. Press **F1** for keyboard shortcuts.

## Full Changelog

See [docs/Changes.md](https://github.com/shanevbg/MDropDX12/blob/v1.8/docs/Changes.md) for the complete list of changes.
