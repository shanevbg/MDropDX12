// midi_input.cpp — MIDI input device management via winmm.dll
//
// Opens MIDI input devices and forwards NoteOn/CC messages via PostMessage
// to a target window (render HWND or MidiWindow HWND for Learn mode).

#include "midi_input.h"

#pragma comment(lib, "winmm.lib") // already linked for joyGetPosEx

namespace mdrop {

// ---------------------------------------------------------------------------
// MidiInProc — system callback (runs on MIDI driver thread, must be fast)
// ---------------------------------------------------------------------------
void CALLBACK MidiInput::MidiInProc(HMIDIIN hmi, UINT wMsg,
    DWORD_PTR dwInstance, DWORD_PTR dwParam1, DWORD_PTR dwParam2)
{
    if (wMsg != MIM_DATA) return;
    MidiInput* self = (MidiInput*)dwInstance;
    if (!self || !self->m_hNotifyWnd) return;

    // dwParam1 contains packed MIDI bytes: [status:8][data1:8][data2:8][unused:8]
    // Filter to NoteOn (0x90-0x9F) and CC (0xB0-0xBF) only
    BYTE status = (BYTE)(dwParam1 & 0xFF);
    BYTE cmd = status & 0xF0;
    if (cmd != 0x90 && cmd != 0xB0) return;

    PostMessage(self->m_hNotifyWnd, self->m_nNotifyMsg, 0, (LPARAM)dwParam1);
}

// ---------------------------------------------------------------------------
// Open — open a MIDI input device and start receiving messages
// ---------------------------------------------------------------------------
bool MidiInput::Open(int deviceId, HWND hNotifyWnd, UINT notifyMsg)
{
    Close();
    m_hNotifyWnd = hNotifyWnd;
    m_nNotifyMsg = notifyMsg;
    m_nDeviceId = deviceId;

    MMRESULT res = midiInOpen(&m_hMidiIn, (UINT)deviceId,
        (DWORD_PTR)MidiInProc, (DWORD_PTR)this, CALLBACK_FUNCTION);
    if (res != MMSYSERR_NOERROR) {
        m_hMidiIn = nullptr;
        m_nDeviceId = -1;
        return false;
    }

    midiInStart(m_hMidiIn);
    return true;
}

// ---------------------------------------------------------------------------
// Close — stop and close the MIDI input device
// ---------------------------------------------------------------------------
void MidiInput::Close()
{
    if (m_hMidiIn) {
        midiInStop(m_hMidiIn);
        midiInReset(m_hMidiIn);
        midiInClose(m_hMidiIn);
        m_hMidiIn = nullptr;
    }
    m_nDeviceId = -1;
}

// ---------------------------------------------------------------------------
// GetNumDevices — return the number of MIDI input devices
// ---------------------------------------------------------------------------
int MidiInput::GetNumDevices()
{
    return (int)midiInGetNumDevs();
}

// ---------------------------------------------------------------------------
// GetDeviceName — get the friendly name of a MIDI input device
// ---------------------------------------------------------------------------
bool MidiInput::GetDeviceName(int id, wchar_t* buf, int bufLen)
{
    if (!buf || bufLen <= 0) return false;
    MIDIINCAPSW caps = {};
    if (midiInGetDevCapsW((UINT)id, &caps, sizeof(caps)) != MMSYSERR_NOERROR)
        return false;
    wcsncpy_s(buf, bufLen, caps.szPname, _TRUNCATE);
    return true;
}

} // namespace mdrop
