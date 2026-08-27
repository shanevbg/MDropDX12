// audio_profile_store.h — named audio behaviour profiles.
//
// A profile says how raw audio becomes the numbers a preset sees. Three
// engines disagree about that, and a preset recovered from one of them wants
// its own engine's answer, so the answer is selectable per preset rather than
// global. See docs/superpowers/specs/2026-08-22-audio-profiles-design.md.
//
// The band group is NOT new maths. MDropDX12 already runs both analyses every
// frame: EngineShell::AnalyzeNewSound fills m_sound and is a faithful port of
// MilkDrop 3's stock code, while Engine::DoCustomSoundAnalysis fills mysound
// with the "pre-vms" analysis -- and only the second reaches presets, through
// var_pf_bass = mysound.imm_rel[0]. bandMode picks which one they are fed.
//
// bass/mid/treb are scale-invariant on both paths (imm_rel = imm / long_avg),
// so a gain field would do nothing to them. Only rates, band edges and the
// sum-vs-mean choice move those. The FFT texture is the opposite: shaders read
// its values directly, so its scale matters and its units are absolute.
#pragma once

#include <string>
#include <vector>

namespace mdrop {

enum class BandMode   { Custom, MilkDrop };
enum class BandEdges  { Linear, Octave };
enum class BandEnergy { Sum, Mean };

struct AudioProfile {
  std::wstring name;
  std::wstring description;

  // True when the FFT group was fitted by measurement rather than transcribed
  // from a source or a decompile. Carried so a profile cannot quietly claim
  // more authority than it has.
  bool fftGroupIsFitted = false;

  // ── Band analysis (relative; gain cancels through imm_rel) ──
  BandMode   bandMode        = BandMode::Custom;
  BandEdges  bandEdges       = BandEdges::Linear;
  BandEnergy bandEnergy      = BandEnergy::Sum;
  float      bandNormalise[3] = { 1.0f, 1.0f, 1.0f };
  float      fpsRef          = 30.0f;
  float      avgAttack       = 0.2f;
  float      avgDecay        = 0.5f;
  float      longMix         = 0.992f;
  float      medMix          = 0.91f;
  // 1.0, matching both engines: DoCustomSoundAnalysis substitutes 1.0 when
  // long_avg is ~0, and so does MilkDrop. Kept as a field because it is a
  // real fork point that was once changed and reverted, not because the
  // two disagree today.
  float      silenceValue    = 1.0f;
  bool       inputDamp       = false;

  // ── FFT texture (absolute; shaders read these values directly) ──
  float      fftAttack       = 0.5f;
  float      fftDecay        = 0.5f;
  float      fftScale        = 0.00035f;
  float      fftNoiseGate    = 5e-5f;
  float      fftVisibleFloor = 2.5e-4f;
  bool       fftLowRolloff   = true;
  int        fftPeakHoldFrames = 30;
  float      fftPeakDecay    = 0.97f;

  // MD3 PRO does not write absolute magnitude into the FFT texture. It
  // PEAK-NORMALISES every frame (MilkDroprev.c:57120-57131), gates at a
  // fraction of that peak (:57132), then max-spreads each bin over its
  // neighbours (:57136-57152), so its texels always reach 1.0 no matter how
  // loud the music is. Presets recovered from MD3 -- the Equalizer family --
  // have their constants calibrated for that 0..1 texel, and at our absolute
  // scale their terms never fire: the ring measured 322/321/322/322 px across
  // four different pieces of music, i.e. not responding to audio at all.
  //
  // Off by default: this changes what every get_fft() shader sees, so it is
  // opted into by the MilkDrop 3 profile only. Scale alone does not substitute
  // -- fftScale 0.0028 (8x) was tried and the ring stayed pinned.
  bool       fftPeakNormalise = false;   // v /= max(eps, frame peak)
  float      fftRelGate       = 0.0f;    // drop v below this FRACTION of peak
  int        fftSpreadTaps    = 0;       // max-spread +/- N bins (0 = off)
  float      fftHzRef        = 22050.0f;
  bool       fftSqrt         = true;

  // ── Waveform ──
  float      pcmGain         = 1.0f;
};

class AudioProfileStore {
public:
  // Directory holding audioprofiles.json. Set once at startup, after the base
  // directory is known.
  void SetResourceDir(const wchar_t* dir);
  void GetStorePath(wchar_t* out, size_t len) const;

  void Names(std::vector<std::wstring>& out) const;
  bool Exists(const wchar_t* name) const;

  // Reads a profile INTO an existing AudioProfile and overwrites only what the
  // stored profile actually names. Seed `inout` with Defaults() first: a
  // profile written before a field existed must leave that field alone rather
  // than snap it to zero. Same contract as VFXProfileStore::Load, and the same
  // reason -- see its header.
  bool Load(const wchar_t* name, AudioProfile& inout) const;
  bool Save(const wchar_t* name, const AudioProfile& d);

  // Today's shipping behaviour, field for field. This is the struct's own
  // member initialisers, which is what makes "the default profile changes
  // nothing" checkable rather than merely asserted.
  static AudioProfile Defaults() { return AudioProfile(); }

  // Write any built-in profile the store is missing.
  //
  // The built-ins are defined in code, not shipped as a file: Release_x64 is
  // gitignored, so a file-only profile would exist on the machine that made it
  // and nowhere else. This follows the same self-bootstrapping rule as the
  // embedded shaders. A profile the user has edited is left alone -- only
  // absent ones are written.
  void EnsureBuiltIns();

  static AudioProfile BuiltInMDropDX12();
  static AudioProfile BuiltInMilkDrop3();
  static AudioProfile BuiltInMilkwave();

private:
  std::wstring m_resourceDir;
};

AudioProfileStore& AudioProfiles();

}  // namespace mdrop
