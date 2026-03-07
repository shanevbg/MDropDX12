// midi_input.h — MIDI input device management via winmm.dll
//
// Provides data structures for MIDI mapping (50 slots, buttons + knobs)
// and a MidiInput class wrapping midiInOpen/midiInStart/midiInClose.

#pragma once
#include <windows.h>
#include <mmsystem.h>
#include <string>
#include <vector>

namespace mdrop {

// ── MIDI mapping types ──

enum MidiActionType {
    MIDI_TYPE_UNDEFINED = 0,
    MIDI_TYPE_BUTTON = 1,   // NoteOn → command string
    MIDI_TYPE_KNOB = 2,     // CC → continuous parameter
};

enum MidiKnobAction {
    MIDI_KNOB_NONE        = 0,
    MIDI_KNOB_AMP_L       = 100,
    MIDI_KNOB_AMP_R       = 101,
    MIDI_KNOB_TIME        = 600,
    MIDI_KNOB_FPS         = 601,
    MIDI_KNOB_INTENSITY   = 610,
    MIDI_KNOB_SHIFT       = 611,
    MIDI_KNOB_QUALITY     = 620,
    MIDI_KNOB_HUE         = 630,
    MIDI_KNOB_SATURATION  = 631,
    MIDI_KNOB_BRIGHTNESS  = 632,
    MIDI_KNOB_OPACITY     = 640,
};

// One MIDI mapping slot (50 total, matching Milkwave Remote's 5 rows x 10 banks)
struct MidiRow {
    int   nRow = 0;
    bool  bActive = false;
    char  szLabel[64] = {};
    int   nChannel = 0;        // MIDI channel (1-16, 0 = any)
    int   nValue = 0;          // Note number (for buttons)
    int   nController = 0;     // CC number (0 = button/note, >0 = knob)
    MidiActionType actionType = MIDI_TYPE_UNDEFINED;
    MidiKnobAction knobAction = MIDI_KNOB_NONE;
    char  szActionText[256] = {};  // Command string for button actions
    float fIncrement = 0.02f;      // Step per MIDI value for knob actions
};

#define MIDI_NUM_ROWS 50

// ── MIDI device wrapper ──

class MidiInput {
public:
    bool Open(int deviceId, HWND hNotifyWnd, UINT notifyMsg);
    void Close();
    bool IsOpen() const { return m_hMidiIn != nullptr; }
    int  GetDeviceId() const { return m_nDeviceId; }

    // Redirect MIDI callbacks to a different window (e.g., MidiWindow for Learn)
    void SetNotifyWnd(HWND hWnd) { m_hNotifyWnd = hWnd; }
    HWND GetNotifyWnd() const { return m_hNotifyWnd; }

    static int  GetNumDevices();
    static bool GetDeviceName(int id, wchar_t* buf, int bufLen);

private:
    HMIDIIN  m_hMidiIn = nullptr;
    int      m_nDeviceId = -1;
    HWND     m_hNotifyWnd = nullptr;
    UINT     m_nNotifyMsg = 0;

    static void CALLBACK MidiInProc(HMIDIIN hmi, UINT wMsg,
        DWORD_PTR dwInstance, DWORD_PTR dwParam1, DWORD_PTR dwParam2);
};

} // namespace mdrop
