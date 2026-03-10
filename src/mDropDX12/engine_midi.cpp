/*
  Engine MIDI functions — JSON persistence, INI settings, device lifecycle,
  and button/knob dispatch.

  Separated from engine_midi_ui.cpp (which contains the MidiWindow ToolWindow).
*/

#include "engine.h"
#include "engine_helpers.h"
#include "utility.h"
#include <fstream>
#include <sstream>
#include <algorithm>

namespace mdrop {

//----------------------------------------------------------------------
// Device lifecycle
//----------------------------------------------------------------------

void Engine::OpenMidiDevice() {
  if (m_nMidiDeviceID < 0) return;
  HWND hRender = GetPluginWindow();
  if (!hRender) return;
  m_midiInput.Open(m_nMidiDeviceID, hRender, WM_MW_MIDI_DATA);
}

void Engine::CloseMidiDevice() {
  m_midiInput.Close();
}

//----------------------------------------------------------------------
// LoadMidiJSON — read midi.json from disk
//----------------------------------------------------------------------

void Engine::LoadMidiJSON()
{
  wchar_t szPath[MAX_PATH];
  swprintf(szPath, MAX_PATH, L"%smidi.json", m_szBaseDir);

  std::ifstream file(szPath);
  if (!file.is_open()) {
    // No file — initialize empty 50 slots
    m_midiRows.assign(MIDI_NUM_ROWS, MidiRow{});
    for (int i = 0; i < MIDI_NUM_ROWS; i++)
      m_midiRows[i].nRow = i + 1;
    return;
  }

  std::ostringstream ss;
  ss << file.rdbuf();
  file.close();
  ParseMidiJSON(ss.str());
}

//----------------------------------------------------------------------
// SaveMidiJSON — write midi.json to disk
//----------------------------------------------------------------------

void Engine::SaveMidiJSON()
{
  std::string json = SerializeMidiJSON();

  wchar_t szPath[MAX_PATH];
  swprintf(szPath, MAX_PATH, L"%smidi.json", m_szBaseDir);

  std::ofstream file(szPath);
  if (file.is_open()) {
    file << json;
    file.close();
  }
}

//----------------------------------------------------------------------
// ParseMidiJSON — parse JSON array of row objects
//----------------------------------------------------------------------

void Engine::ParseMidiJSON(const std::string& json)
{
  m_midiRows.assign(MIDI_NUM_ROWS, MidiRow{});
  for (int i = 0; i < MIDI_NUM_ROWS; i++)
    m_midiRows[i].nRow = i + 1;

  // Simple line-by-line parser (same approach as ParseControllerJSON)
  std::istringstream stream(json);
  std::string line;
  int currentRow = -1;
  bool inObject = false;

  while (std::getline(stream, line)) {
    size_t start = line.find_first_not_of(" \t\r\n");
    if (start == std::string::npos) continue;
    line = line.substr(start);

    if (line[0] == '{') { inObject = true; currentRow = -1; continue; }
    if (line[0] == '}') {
      inObject = false;
      continue;
    }
    if (!inObject) continue;

    // Parse "key": value
    size_t q1 = line.find('"');
    if (q1 == std::string::npos) continue;
    size_t q2 = line.find('"', q1 + 1);
    if (q2 == std::string::npos) continue;
    std::string key = line.substr(q1 + 1, q2 - q1 - 1);

    // Find the value after the colon
    size_t colon = line.find(':', q2 + 1);
    if (colon == std::string::npos) continue;
    std::string valStr = line.substr(colon + 1);
    // Trim whitespace and trailing comma
    size_t vs = valStr.find_first_not_of(" \t");
    if (vs == std::string::npos) continue;
    valStr = valStr.substr(vs);
    while (!valStr.empty() && (valStr.back() == ',' || valStr.back() == '\r' || valStr.back() == '\n'))
      valStr.pop_back();

    // Check if value is a string (quoted)
    std::string strVal;
    int intVal = 0;
    float floatVal = 0.0f;
    bool boolVal = false;
    bool isString = false;

    if (!valStr.empty() && valStr.front() == '"') {
      size_t sq1 = valStr.find('"');
      size_t sq2 = valStr.find('"', sq1 + 1);
      if (sq2 != std::string::npos)
        strVal = valStr.substr(sq1 + 1, sq2 - sq1 - 1);
      isString = true;
    } else if (valStr == "true") {
      boolVal = true;
    } else if (valStr == "false") {
      boolVal = false;
    } else {
      try { intVal = std::stoi(valStr); } catch (...) {}
      try { floatVal = std::stof(valStr); } catch (...) {}
    }

    // Apply values
    if (key == "row") {
      currentRow = intVal - 1;  // 1-based in JSON, 0-based in array
    }
    else if (currentRow >= 0 && currentRow < MIDI_NUM_ROWS) {
      MidiRow& row = m_midiRows[currentRow];
      row.nRow = currentRow + 1;

      if (key == "active") row.bActive = boolVal;
      else if (key == "label" && isString) strncpy_s(row.szLabel, strVal.c_str(), _TRUNCATE);
      else if (key == "channel") row.nChannel = intVal;
      else if (key == "value") row.nValue = intVal;
      else if (key == "controller") row.nController = intVal;
      else if (key == "actionType") row.actionType = (MidiActionType)intVal;
      else if (key == "knobAction") row.knobAction = (MidiKnobAction)intVal;
      else if (key == "actionText" && isString) strncpy_s(row.szActionText, strVal.c_str(), _TRUNCATE);
      else if (key == "increment") row.fIncrement = floatVal;
    }
  }
}

//----------------------------------------------------------------------
// SerializeMidiJSON — generate JSON string from m_midiRows
//----------------------------------------------------------------------

std::string Engine::SerializeMidiJSON() const
{
  std::ostringstream ss;
  ss << "[\r\n";

  bool first = true;
  for (const auto& row : m_midiRows) {
    // Only write active rows or rows with any assignment
    if (!row.bActive && row.actionType == MIDI_TYPE_UNDEFINED) continue;

    if (!first) ss << ",\r\n";
    first = false;

    ss << "  {\r\n";
    ss << "    \"row\": " << row.nRow << ",\r\n";
    ss << "    \"active\": " << (row.bActive ? "true" : "false") << ",\r\n";
    ss << "    \"label\": \"" << row.szLabel << "\",\r\n";
    ss << "    \"channel\": " << row.nChannel << ",\r\n";
    ss << "    \"value\": " << row.nValue << ",\r\n";
    ss << "    \"controller\": " << row.nController << ",\r\n";
    ss << "    \"actionType\": " << (int)row.actionType << ",\r\n";
    ss << "    \"knobAction\": " << (int)row.knobAction << ",\r\n";
    ss << "    \"actionText\": \"" << row.szActionText << "\",\r\n";

    // Format increment with fixed precision
    char incBuf[32];
    snprintf(incBuf, 32, "%.3f", row.fIncrement);
    ss << "    \"increment\": " << incBuf << "\r\n";
    ss << "  }";
  }

  ss << "\r\n]\r\n";
  return ss.str();
}

//----------------------------------------------------------------------
// LoadMidiSettings / SaveMidiSettings — INI persistence
//----------------------------------------------------------------------

void Engine::LoadMidiSettings()
{
  wchar_t* pIni = GetConfigIniFile();
  m_bMidiEnabled = GetPrivateProfileIntW(L"MIDI", L"Enabled", 0, pIni) != 0;
  m_nMidiDeviceID = GetPrivateProfileIntW(L"MIDI", L"DeviceID", -1, pIni);
  GetPrivateProfileStringW(L"MIDI", L"DeviceName", L"", m_szMidiDeviceName, 256, pIni);
  m_nMidiBufferDelay = GetPrivateProfileIntW(L"MIDI", L"BufferDelay", 30, pIni);
}

void Engine::SaveMidiSettings()
{
  wchar_t* pIni = GetConfigIniFile();
  wchar_t buf[32];

  WritePrivateProfileStringW(L"MIDI", L"Enabled",
    m_bMidiEnabled ? L"1" : L"0", pIni);
  swprintf(buf, 32, L"%d", m_nMidiDeviceID);
  WritePrivateProfileStringW(L"MIDI", L"DeviceID", buf, pIni);
  WritePrivateProfileStringW(L"MIDI", L"DeviceName", m_szMidiDeviceName, pIni);
  swprintf(buf, 32, L"%d", m_nMidiBufferDelay);
  WritePrivateProfileStringW(L"MIDI", L"BufferDelay", buf, pIni);
}

//----------------------------------------------------------------------
// LoadMidiDefaultActions — read midi-default.txt for button presets
//----------------------------------------------------------------------

void Engine::LoadMidiDefaultActions(std::vector<std::string>& out)
{
  out.clear();
  wchar_t szPath[MAX_PATH];
  swprintf(szPath, MAX_PATH, L"%smidi-default.txt", m_szBaseDir);

  std::ifstream file(szPath);
  if (!file.is_open()) return;

  std::string line;
  while (std::getline(file, line)) {
    // Trim
    size_t s = line.find_first_not_of(" \t\r\n");
    if (s == std::string::npos) continue;
    size_t e = line.find_last_not_of(" \t\r\n");
    line = line.substr(s, e - s + 1);
    // Skip comments and empty
    if (line.empty() || line[0] == '#' || line[0] == '/' ) continue;
    out.push_back(line);
  }
}

//----------------------------------------------------------------------
// ExecuteMidiButton — dispatch a button action (NoteOn)
//----------------------------------------------------------------------

void Engine::ExecuteMidiButton(const MidiRow& row)
{
  if (row.szActionText[0] == '\0') return;
  ExecuteControllerCommand(std::string(row.szActionText));
}

//----------------------------------------------------------------------
// ExecuteMidiKnob — dispatch a knob action (CC value 0-127)
//----------------------------------------------------------------------

void Engine::ExecuteMidiKnob(const MidiRow& row, int midiValue)
{
  float v;
  switch (row.knobAction) {
  case MIDI_KNOB_HUE:
    v = (midiValue - 64) * row.fIncrement;
    m_ColShiftHue = std::clamp(v, -1.0f, 1.0f);
    break;
  case MIDI_KNOB_SATURATION:
    v = (midiValue - 64) * row.fIncrement;
    m_ColShiftSaturation = std::clamp(v, -1.0f, 1.0f);
    break;
  case MIDI_KNOB_BRIGHTNESS:
    v = (midiValue - 64) * row.fIncrement;
    m_ColShiftBrightness = std::clamp(v, -1.0f, 1.0f);
    break;
  case MIDI_KNOB_INTENSITY:
    v = 1.0f + (midiValue - 64) * row.fIncrement;
    m_VisIntensity = std::clamp(v, 0.0f, 10.0f);
    break;
  case MIDI_KNOB_SHIFT:
    v = (midiValue - 64) * row.fIncrement;
    m_VisShift = std::clamp(v, -10.0f, 10.0f);
    break;
  case MIDI_KNOB_QUALITY: {
    float q = midiValue / 127.0f;
    m_fRenderQuality = std::clamp(q, 0.1f, 1.0f);
    break;
  }
  case MIDI_KNOB_TIME:
    v = 1.0f + (midiValue - 64) * row.fIncrement;
    m_timeFactor = std::clamp(v, 0.0f, 10.0f);
    break;
  case MIDI_KNOB_FPS:
    v = 1.0f + (midiValue - 64) * row.fIncrement;
    m_fpsFactor = std::clamp(v, 0.0f, 10.0f);
    break;
  case MIDI_KNOB_AMP_L:
    v = midiValue / 127.0f * 2.0f;
    // Amp is set via IPC command
    {
      wchar_t cmd[64];
      swprintf(cmd, 64, L"AMP_L=%.3f", v);
      RenderCommand rc;
      rc.cmd = RenderCmd::IPCMessage;
      rc.sParam = cmd;
      EnqueueRenderCmd(std::move(rc));
    }
    break;
  case MIDI_KNOB_AMP_R:
    v = midiValue / 127.0f * 2.0f;
    {
      wchar_t cmd[64];
      swprintf(cmd, 64, L"AMP_R=%.3f", v);
      RenderCommand rc;
      rc.cmd = RenderCmd::IPCMessage;
      rc.sParam = cmd;
      EnqueueRenderCmd(std::move(rc));
    }
    break;
  case MIDI_KNOB_OPACITY: {
    float op = midiValue / 127.0f;
    HWND hRender = GetPluginWindow();
    if (hRender)
      PostMessage(hRender, WM_MW_SET_OPACITY, 0, (LPARAM)(int)(op * 100.0f));
    break;
  }
  default:
    break;
  }
}

} // namespace mdrop
