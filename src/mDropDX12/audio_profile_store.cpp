// audio_profile_store.cpp — read/write audioprofiles.json.
//
// One file, keyed by profile name, mirroring vfxprofiles.json. Load is an
// in/out merge rather than a fetch, for the same reason VFXProfileStore::Load
// is: a profile written before a field existed must leave that field where the
// caller had it instead of snapping it to zero.

#include "audio_profile_store.h"

#include <Windows.h>

#include "json_utils.h"
#include "utility.h"

namespace mdrop {

static const wchar_t* kStoreName = L"audioprofiles.json";
static const wchar_t* kProfiles = L"profiles";

AudioProfileStore& AudioProfiles() {
  static AudioProfileStore s;
  return s;
}

void AudioProfileStore::SetResourceDir(const wchar_t* dir) {
  m_resourceDir = dir ? dir : L"";
}

void AudioProfileStore::GetStorePath(wchar_t* out, size_t len) const {
  const bool needSlash =
      !m_resourceDir.empty() && m_resourceDir.back() != L'\\' &&
      m_resourceDir.back() != L'/';
  swprintf_s(out, len, L"%ls%ls%ls", m_resourceDir.c_str(),
             needSlash ? L"\\" : L"", kStoreName);
}

// ─── enum <-> string ────────────────────────────────────────────────────
//
// An unrecognised spelling leaves the seeded value alone. Mapping it to the
// first enumerator instead would turn a typo in a hand-edited profile into a
// silent behaviour change, which is exactly the class of bug this file's
// leave-alone contract exists to avoid.

static void ReadBandMode(const JsonValue& v, BandMode& out) {
  const std::wstring s = v.asString();
  if (s == L"custom") out = BandMode::Custom;
  else if (s == L"milkdrop") out = BandMode::MilkDrop;
}

static void ReadBandEdges(const JsonValue& v, BandEdges& out) {
  const std::wstring s = v.asString();
  if (s == L"linear") out = BandEdges::Linear;
  else if (s == L"octave") out = BandEdges::Octave;
}

static void ReadBandEnergy(const JsonValue& v, BandEnergy& out) {
  const std::wstring s = v.asString();
  if (s == L"sum") out = BandEnergy::Sum;
  else if (s == L"mean") out = BandEnergy::Mean;
}

static const wchar_t* BandModeName(BandMode m) {
  return m == BandMode::MilkDrop ? L"milkdrop" : L"custom";
}
static const wchar_t* BandEdgesName(BandEdges m) {
  return m == BandEdges::Octave ? L"octave" : L"linear";
}
static const wchar_t* BandEnergyName(BandEnergy m) {
  return m == BandEnergy::Mean ? L"mean" : L"sum";
}

// ─── read ───────────────────────────────────────────────────────────────

static void ReadProfile(const JsonValue& p, AudioProfile& io) {
  if (p.has(L"description")) io.description = p[L"description"].asString();
  if (p.has(L"_fftGroupIsFitted"))
    io.fftGroupIsFitted = p[L"_fftGroupIsFitted"].asBool(io.fftGroupIsFitted);

  if (p.has(L"bandMode"))   ReadBandMode(p[L"bandMode"], io.bandMode);
  if (p.has(L"bandEdges"))  ReadBandEdges(p[L"bandEdges"], io.bandEdges);
  if (p.has(L"bandEnergy")) ReadBandEnergy(p[L"bandEnergy"], io.bandEnergy);

  if (p.has(L"bandNormalise")) {
    const JsonValue& a = p[L"bandNormalise"];
    // A short array fills what it has: a hand-written profile naming only the
    // bass divisor should not zero the other two.
    for (size_t i = 0; i < a.size() && i < 3; i++)
      io.bandNormalise[i] = a.at(i).asFloat(io.bandNormalise[i]);
  }

  if (p.has(L"fpsRef"))       io.fpsRef = p[L"fpsRef"].asFloat(io.fpsRef);
  if (p.has(L"avgAttack"))    io.avgAttack = p[L"avgAttack"].asFloat(io.avgAttack);
  if (p.has(L"avgDecay"))     io.avgDecay = p[L"avgDecay"].asFloat(io.avgDecay);
  if (p.has(L"longMix"))      io.longMix = p[L"longMix"].asFloat(io.longMix);
  if (p.has(L"medMix"))       io.medMix = p[L"medMix"].asFloat(io.medMix);
  if (p.has(L"silenceValue")) io.silenceValue = p[L"silenceValue"].asFloat(io.silenceValue);
  if (p.has(L"inputDamp"))    io.inputDamp = p[L"inputDamp"].asBool(io.inputDamp);

  if (p.has(L"fftAttack"))       io.fftAttack = p[L"fftAttack"].asFloat(io.fftAttack);
  if (p.has(L"fftDecay"))        io.fftDecay = p[L"fftDecay"].asFloat(io.fftDecay);
  if (p.has(L"fftScale"))        io.fftScale = p[L"fftScale"].asFloat(io.fftScale);
  if (p.has(L"fftNoiseGate"))    io.fftNoiseGate = p[L"fftNoiseGate"].asFloat(io.fftNoiseGate);
  if (p.has(L"fftVisibleFloor")) io.fftVisibleFloor = p[L"fftVisibleFloor"].asFloat(io.fftVisibleFloor);
  if (p.has(L"fftLowRolloff"))   io.fftLowRolloff = p[L"fftLowRolloff"].asBool(io.fftLowRolloff);
  if (p.has(L"fftPeakHoldFrames")) io.fftPeakHoldFrames = p[L"fftPeakHoldFrames"].asInt(io.fftPeakHoldFrames);
  if (p.has(L"fftPeakDecay"))    io.fftPeakDecay = p[L"fftPeakDecay"].asFloat(io.fftPeakDecay);
  if (p.has(L"fftHzRef"))        io.fftHzRef = p[L"fftHzRef"].asFloat(io.fftHzRef);
  if (p.has(L"fftSqrt"))         io.fftSqrt = p[L"fftSqrt"].asBool(io.fftSqrt);

  if (p.has(L"pcmGain")) io.pcmGain = p[L"pcmGain"].asFloat(io.pcmGain);
}

bool AudioProfileStore::Load(const wchar_t* name, AudioProfile& inout) const {
  if (!name || !name[0]) return false;
  wchar_t path[MAX_PATH];
  GetStorePath(path, MAX_PATH);
  JsonValue root = JsonLoadFile(path);
  if (root.isNull()) return false;
  const JsonValue& profiles = root[kProfiles];
  if (!profiles.isObject() || !profiles.has(name)) return false;
  inout.name = name;
  ReadProfile(profiles[name], inout);
  return true;
}

// ─── write ──────────────────────────────────────────────────────────────

static void WriteProfile(JsonWriter& w, const wchar_t* key,
                         const AudioProfile& d) {
  w.BeginObject(key);
  w.String(L"description", d.description);
  w.Bool(L"_fftGroupIsFitted", d.fftGroupIsFitted);

  w.String(L"bandMode", BandModeName(d.bandMode));
  w.String(L"bandEdges", BandEdgesName(d.bandEdges));
  w.String(L"bandEnergy", BandEnergyName(d.bandEnergy));
  // Precise: these three are transcribed out of MilkDrop 3's source, and the
  // file is a record of what that engine does.
  w.BeginArray(L"bandNormalise");
  for (int i = 0; i < 3; i++) w.FloatPreciseAnon(d.bandNormalise[i]);
  w.EndArray();
  w.Float(L"fpsRef", d.fpsRef);
  w.Float(L"avgAttack", d.avgAttack);
  w.Float(L"avgDecay", d.avgDecay);
  w.FloatPrecise(L"longMix", d.longMix);
  w.Float(L"medMix", d.medMix);
  w.Float(L"silenceValue", d.silenceValue);
  w.Bool(L"inputDamp", d.inputDamp);

  w.Float(L"fftAttack", d.fftAttack);
  w.Float(L"fftDecay", d.fftDecay);
  w.FloatPrecise(L"fftScale", d.fftScale);
  w.FloatPrecise(L"fftNoiseGate", d.fftNoiseGate);
  w.FloatPrecise(L"fftVisibleFloor", d.fftVisibleFloor);
  w.Bool(L"fftLowRolloff", d.fftLowRolloff);
  w.Int(L"fftPeakHoldFrames", d.fftPeakHoldFrames);
  w.FloatPrecise(L"fftPeakDecay", d.fftPeakDecay);
  w.Float(L"fftHzRef", d.fftHzRef);
  w.Bool(L"fftSqrt", d.fftSqrt);

  w.Float(L"pcmGain", d.pcmGain);
  w.EndObject();
}

bool AudioProfileStore::Save(const wchar_t* name, const AudioProfile& d) {
  if (!name || !name[0]) return false;
  wchar_t path[MAX_PATH];
  GetStorePath(path, MAX_PATH);

  // Rewrite the whole file, carrying every profile this build did not touch
  // through verbatim -- including any a newer build wrote with fields we do
  // not understand. Dropping them would make an older build destructive.
  JsonValue root = JsonLoadFile(path);
  const JsonValue& existing = root[kProfiles];

  JsonWriter w;
  w.BeginObject();
  w.BeginObject(kProfiles);
  bool replaced = false;
  if (existing.isObject()) {
    for (const auto& [key, val] : existing.members) {
      if (_wcsicmp(key.c_str(), name) == 0) {
        WriteProfile(w, key.c_str(), d);
        replaced = true;
      } else {
        w.Value(key.c_str(), val);
      }
    }
  }
  if (!replaced) WriteProfile(w, name, d);
  w.EndObject();
  w.EndObject();
  return w.SaveToFile(path);
}

// ─── built-ins ──────────────────────────────────────────────────────────

AudioProfile AudioProfileStore::BuiltInMDropDX12() {
  AudioProfile d;   // the member initialisers ARE this build's constants
  d.name = L"MDropDX12";
  d.description = L"This build's own audio. The default; selecting it changes nothing.";
  return d;
}

AudioProfile AudioProfileStore::BuiltInMilkDrop3() {
  AudioProfile d;
  d.name = L"MilkDrop 3";
  d.description =
      L"MilkDrop 3's audio. The band group is transcribed from MilkDrop 3's own "
      L"source (pluginshell.cpp); fftHzRef and fftSqrt are transcribed from the "
      L"MD3 PRO decompile. The remaining FFT values are FITTED by measurement -- "
      L"MD3's texFFT fill was never located -- so treat them as an approximation "
      L"to improve, not as ground truth.";
  d.fftGroupIsFitted = true;

  // Transcribed: MilkDrop3/code/vis_milk2/pluginshell.cpp, AnalyzeNewSound.
  d.bandMode  = BandMode::MilkDrop;
  d.bandEdges = BandEdges::Octave;
  d.bandEnergy = BandEnergy::Mean;
  d.bandNormalise[0] = 0.326781557f;
  d.bandNormalise[1] = 0.380873770f;
  d.bandNormalise[2] = 0.199888934f;
  d.fpsRef       = 14.0f;
  d.avgAttack    = 0.2f;
  d.avgDecay     = 0.5f;
  d.longMix      = 0.96f;
  d.medMix       = 0.91f;
  d.silenceValue = 1.0f;    // MilkDrop substitutes 1.0, not 0.0, on silence
  d.inputDamp    = true;    // temp_wave = 0.5*(w[i] + w[i-1]) before the FFT

  // Transcribed: the shader preamble in the MD3 PRO decompile --
  //   #define get_fft(pos) tex2D(sampler_fft, float2(clamp(pos,0,1),0.5)).r
  //   #define get_fft_hz(freq) get_fft(clamp((freq)/24000.0, 0.0, 1.0))
  d.fftHzRef = 24000.0f;
  d.fftSqrt  = false;

  // Fitted. MD3's spectrum is unsmoothed, hotter, and has a noise floor
  // rather than a hard zero, so smoothing and both floors come off and the
  // scale goes up. Tune these against tools/milk2-probe, not by guessing.
  // Unsmoothed, which is what the two-engine spectrum capture shows MD3 to
  // be. UpdateAudioTexture computes decayFactor = (1 - decay)^2, so decay 0
  // means "fall instantly" -- i.e. track the instantaneous spectrum. decay 1
  // is the opposite: factor 0, a value that can rise and never fall. That is
  // a LATCH, and it is what these numbers said at first: the ring grew
  // monotonically (extent 201, 419, 475, 535) and looked like reactivity.
  d.fftAttack       = 1.0f;
  d.fftDecay        = 0.0f;
  d.fftScale        = 0.0028f;
  d.fftNoiseGate    = 0.0f;
  d.fftVisibleFloor = 0.0f;
  d.fftLowRolloff   = false;
  // The part scale could not buy. With the texture peak-normalised the
  // absolute level stops mattering, which is the whole point -- fftScale
  // survives only to keep the pre-normalise numbers in a sane range.
  d.fftPeakNormalise = true;
  d.fftRelGate       = 0.2f;   // MilkDroprev.c:57132
  d.fftSpreadTaps    = 3;      // MilkDroprev.c:57136-57152, weights 1 - 0.25*|d|
  return d;
}

AudioProfile AudioProfileStore::BuiltInMilkwave() {
  AudioProfile d;   // identical to MDropDX12 today; see the description
  d.name = L"Milkwave";
  d.description =
      L"Milkwave Visualizer's audio. Its include.fx declares the identical "
      L"512x2 texture layout, row assignment, sqrt and 22050 divisor, so this "
      L"matches MDropDX12 exactly today. Kept as its own name so a preset can "
      L"say Milkwave and keep meaning it if the two ever diverge.";
  return d;
}

void AudioProfileStore::EnsureBuiltIns() {
  const AudioProfile builtins[] = {
      BuiltInMDropDX12(), BuiltInMilkDrop3(), BuiltInMilkwave() };
  for (const AudioProfile& d : builtins) {
    // Only when absent. Overwriting would silently discard a user's edit
    // every launch, which is the opposite of what a store is for.
    if (!Exists(d.name.c_str()))
      Save(d.name.c_str(), d);
  }
}

void AudioProfileStore::Names(std::vector<std::wstring>& out) const {
  out.clear();
  wchar_t path[MAX_PATH];
  GetStorePath(path, MAX_PATH);
  JsonValue root = JsonLoadFile(path);
  const JsonValue& profiles = root[kProfiles];
  if (!profiles.isObject()) return;
  for (const auto& [key, val] : profiles.members) {
    (void)val;
    out.push_back(key);
  }
}

bool AudioProfileStore::Exists(const wchar_t* name) const {
  if (!name || !name[0]) return false;
  wchar_t path[MAX_PATH];
  GetStorePath(path, MAX_PATH);
  JsonValue root = JsonLoadFile(path);
  const JsonValue& profiles = root[kProfiles];
  return profiles.isObject() && profiles.has(name);
}

}  // namespace mdrop
