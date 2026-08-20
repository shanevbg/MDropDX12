#pragma once
/*
  render_tunables.h — engine fidelity knobs that used to be settings.ini only.

  One table drives all three surfaces, so a new knob is one row here plus an int
  member on Engine:

    * the Visual window sliders   (engine_visual_ui.cpp)
    * SET_TUNABLE / GET_TUNABLES  (engine_messages.cpp)
    * the settings.ini keys       (engine.cpp MyReadConfig / MyWriteConfig)

  The IPC name and the INI key are deliberately the same string, so a value seen
  in settings.ini can be set over the pipe without a lookup table in between.

  Same intent as fps_caps.h: one definition, no drift between UI and protocol.
*/

#include <Windows.h>

namespace mdrop {

class Engine;

// WARNING: only include this from a TU where Engine is COMPLETE (i.e. one
// that includes engine.h first).
//
// RenderTunable holds an `int Engine::*`. A forward declaration is enough to
// DECLARE that member, but not to lay it out: MSVC sizes a pointer-to-member
// of an incomplete class using the fully-general representation (12 bytes)
// and a complete single-inheritance one at 4. sizeof(RenderTunable) is then
// 56 in the incomplete TU against 48 in every other, so kRenderTunables[i]
// strides by the wrong amount and hands back a garbage name pointer -- an
// access violation that surfaces as "Unknown non-standard exception in
// render loop", nowhere near the include that caused it.
//
// If something outside the engine needs these, pass the values in (see
// VFXProfileStore::SetTunables) rather than sharing the table.

struct RenderTunable {
  const wchar_t* name;       // IPC name AND [Settings] INI key
  const wchar_t* label;      // Visual window label
  int Engine::* member;      // where it lives on the engine
  int   minValue;
  int   maxValue;
  int   defValue;           // must match the member initialiser in engine.h,
                            // which is what MyReadConfig falls back to
  bool  hundredths;          // display value/100 rather than the raw int
  int   controlID;           // Visual window slider
  int   labelID;             // Visual window value readout
};

// Declared here, defined in engine_visual_ui.cpp (the only TU that needs the
// Engine definition at namespace scope for the member pointers).
extern const RenderTunable kRenderTunables[];
extern const int kRenderTunableCount;

// Set a tunable on the engine, clamp it to range and persist that one INI key.
// Returns the value actually stored. Defined in engine_visual_ui.cpp alongside
// the table.
int ApplyRenderTunable(Engine* p, const RenderTunable& t, int value);

// Render a tunable for display (honours `hundredths`). The wire and INI formats
// are always the raw int; only the UI reads differently.
void FormatTunableValue(const RenderTunable& t, int value, wchar_t* buf, size_t cch);

// Clamp a raw value to a tunable's declared range.
inline int ClampTunable(const RenderTunable& t, int v) {
  if (v < t.minValue) return t.minValue;
  if (v > t.maxValue) return t.maxValue;
  return v;
}

}  // namespace mdrop
