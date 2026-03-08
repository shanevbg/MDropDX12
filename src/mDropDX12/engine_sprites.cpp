// Engine sprite functions — lifecycle, INI persistence, property management.
// Separated from engine_messages.cpp and engine_settings_ui.cpp.

#include "engine.h"
#include "engine_helpers.h"
#include "tool_window.h"
#include "utility.h"
#include "AutoCharFn.h"
#include "resource.h"
#include "wasabi.h"
#include <commctrl.h>
#include <wincodec.h>
#pragma comment(lib, "windowscodecs.lib")

namespace mdrop {

void Engine::LoadSpritesFromINI() {
  m_spriteEntries.clear();
  m_nSpriteSelected = -1;

  // Enumerate all sections to discover sprite entries (supports any index, not just 0-99)
  std::vector<wchar_t> secBuf(32768);
  DWORD len = GetPrivateProfileSectionNamesW(secBuf.data(), (DWORD)secBuf.size(), m_szImgIniFile);
  if (len == 0) return;

  // Collect all img* section indices
  std::vector<int> indices;
  const wchar_t* p = secBuf.data();
  while (*p) {
    if (wcsncmp(p, L"img", 3) == 0) {
      const wchar_t* numPart = p + 3;
      if (*numPart >= L'0' && *numPart <= L'9') {
        int idx = _wtoi(numPart);
        indices.push_back(idx);
      }
    }
    p += wcslen(p) + 1;
  }
  std::sort(indices.begin(), indices.end());

  for (int i : indices) {
    // Try the exact section name that's in the file
    // For backward compat: try both img%02d and img%d formats
    wchar_t section[64];
    FormatSpriteSection(section, 64, i);

    wchar_t img[512] = {};
    GetPrivateProfileStringW(section, L"img", L"", img, 511, m_szImgIniFile);
    if (img[0] == 0) continue;

    SpriteEntry entry = {};
    entry.nIndex = i;
    wcscpy_s(entry.szImg, img);

    // Read colorkey (try colorkey_lo first for backwards compat, then colorkey)
    entry.nColorkey = (unsigned int)GetPrivateProfileIntW(section, L"colorkey_lo", 0, m_szImgIniFile);
    entry.nColorkey = (unsigned int)GetPrivateProfileIntW(section, L"colorkey", entry.nColorkey, m_szImgIniFile);

    // Read init_N and code_N lines (same pattern as LaunchSprite)
    char sectionA[64];
    FormatSpriteSectionA(sectionA, 64, i);
    char szTemp[8192];

    for (int pass = 0; pass < 2; pass++) {
      std::string& dest = (pass == 0) ? entry.szInitCode : entry.szFrameCode;
      dest.clear();
      for (int line = 1; ; line++) {
        char key[32];
        sprintf(key, pass == 0 ? "init_%d" : "code_%d", line);
        GetPrivateProfileStringA(sectionA, key, "~!@#$", szTemp, 8192, AutoCharFn(m_szImgIniFile));
        if (strcmp(szTemp, "~!@#$") == 0) break;
        if (!dest.empty()) dest += "\r\n";
        dest += szTemp;
      }
    }
    m_spriteEntries.push_back(entry);
  }
}

void Engine::SaveSpritesToINI() {
  // Delete all existing img* sections by scanning the INI
  {
    std::vector<wchar_t> secBuf(32768);
    DWORD len = GetPrivateProfileSectionNamesW(secBuf.data(), (DWORD)secBuf.size(), m_szImgIniFile);
    if (len > 0) {
      const wchar_t* p = secBuf.data();
      while (*p) {
        if (wcsncmp(p, L"img", 3) == 0 && p[3] >= L'0' && p[3] <= L'9')
          WritePrivateProfileStringW(p, NULL, NULL, m_szImgIniFile);
        p += wcslen(p) + 1;
      }
    }
  }

  // Write current entries
  for (auto& e : m_spriteEntries) {
    wchar_t section[64];
    FormatSpriteSection(section, 64, e.nIndex);

    WritePrivateProfileStringW(section, L"img", e.szImg, m_szImgIniFile);

    if (e.nColorkey != 0) {
      wchar_t ckBuf[32];
      swprintf(ckBuf, 32, L"0x%06X", e.nColorkey);
      WritePrivateProfileStringW(section, L"colorkey", ckBuf, m_szImgIniFile);
    }

    // Write init_N / code_N lines
    char sectionA[64];
    FormatSpriteSectionA(sectionA, 64, e.nIndex);

    auto writeLines = [&](const std::string& code, const char* prefix) {
      std::istringstream ss(code);
      std::string line;
      int n = 1;
      while (std::getline(ss, line)) {
        // Strip trailing \r if present (from \r\n in edit control)
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.empty()) continue;
        char key[32];
        sprintf(key, "%s_%d", prefix, n++);
        WritePrivateProfileStringA(sectionA, key, line.c_str(), AutoCharFn(m_szImgIniFile));
      }
    };
    writeLines(e.szInitCode, "init");
    writeLines(e.szFrameCode, "code");
  }

  // Flush INI cache to disk
  WritePrivateProfileStringW(NULL, NULL, NULL, m_szImgIniFile);
}

HBITMAP Engine::LoadThumbnailWIC(const wchar_t* szPath, int cx, int cy) {
  // Resolve relative path
  wchar_t fullPath[MAX_PATH] = {};
  if (szPath[0] && szPath[1] != L':') {
    if (m_szContentBasePath[0]) {
      swprintf(fullPath, MAX_PATH, L"%s%s", m_szContentBasePath, szPath);
      if (GetFileAttributesW(fullPath) == INVALID_FILE_ATTRIBUTES)
        swprintf(fullPath, MAX_PATH, L"%s%s", m_szMilkdrop2Path, szPath);
    } else {
      swprintf(fullPath, MAX_PATH, L"%s%s", m_szMilkdrop2Path, szPath);
    }
  } else {
    wcscpy_s(fullPath, szPath);
  }

  if (GetFileAttributesW(fullPath) == INVALID_FILE_ATTRIBUTES)
    return NULL;

  IWICImagingFactory* pFactory = NULL;
  HRESULT hr = CoCreateInstance(CLSID_WICImagingFactory, NULL, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&pFactory));
  if (FAILED(hr) || !pFactory) return NULL;

  IWICBitmapDecoder* pDecoder = NULL;
  hr = pFactory->CreateDecoderFromFilename(fullPath, NULL, GENERIC_READ, WICDecodeMetadataCacheOnDemand, &pDecoder);
  if (FAILED(hr) || !pDecoder) { pFactory->Release(); return NULL; }

  IWICBitmapFrameDecode* pFrame = NULL;
  pDecoder->GetFrame(0, &pFrame);
  if (!pFrame) { pDecoder->Release(); pFactory->Release(); return NULL; }

  IWICBitmapScaler* pScaler = NULL;
  pFactory->CreateBitmapScaler(&pScaler);
  if (!pScaler) { pFrame->Release(); pDecoder->Release(); pFactory->Release(); return NULL; }
  pScaler->Initialize(pFrame, cx, cy, WICBitmapInterpolationModeLinear);

  IWICFormatConverter* pConverter = NULL;
  pFactory->CreateFormatConverter(&pConverter);
  if (!pConverter) { pScaler->Release(); pFrame->Release(); pDecoder->Release(); pFactory->Release(); return NULL; }
  pConverter->Initialize(pScaler, GUID_WICPixelFormat32bppBGRA, WICBitmapDitherTypeNone, NULL, 0.0, WICBitmapPaletteTypeCustom);

  BITMAPINFO bmi = {};
  bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
  bmi.bmiHeader.biWidth = cx;
  bmi.bmiHeader.biHeight = -cy;  // top-down
  bmi.bmiHeader.biPlanes = 1;
  bmi.bmiHeader.biBitCount = 32;
  void* pvBits = NULL;
  HBITMAP hBmp = CreateDIBSection(NULL, &bmi, DIB_RGB_COLORS, &pvBits, NULL, 0);
  if (hBmp && pvBits)
    pConverter->CopyPixels(NULL, cx * 4, cx * cy * 4, (BYTE*)pvBits);

  pConverter->Release();
  pScaler->Release();
  pFrame->Release();
  pDecoder->Release();
  pFactory->Release();
  return hBmp;
}

void Engine::PopulateSpriteListView() {
  if (!m_hSpriteList) return;
  ListView_DeleteAllItems(m_hSpriteList);
  if (m_hSpriteImageList) ImageList_RemoveAll((HIMAGELIST)m_hSpriteImageList);

  for (int i = 0; i < (int)m_spriteEntries.size(); i++) {
    auto& e = m_spriteEntries[i];

    // Load thumbnail
    HBITMAP hBmp = LoadThumbnailWIC(e.szImg, 32, 32);
    int imgIdx = -1;
    if (hBmp) {
      imgIdx = ImageList_Add((HIMAGELIST)m_hSpriteImageList, hBmp, NULL);
      DeleteObject(hBmp);
    }

    // Insert item
    wchar_t numBuf[16];
    FormatSpriteSection(numBuf, 16, e.nIndex);
    LVITEMW lvi = {};
    lvi.mask = LVIF_TEXT | LVIF_IMAGE;
    lvi.iItem = i;
    lvi.iImage = imgIdx;
    lvi.pszText = numBuf;
    SendMessageW(m_hSpriteList, LVM_INSERTITEMW, 0, (LPARAM)&lvi);

    // Filename column
    const wchar_t* pName = wcsrchr(e.szImg, L'\\');
    if (!pName) pName = wcsrchr(e.szImg, L'/');
    pName = pName ? pName + 1 : e.szImg;
    lvi.mask = LVIF_TEXT;
    lvi.iSubItem = 1;
    lvi.pszText = (LPWSTR)pName;
    SendMessageW(m_hSpriteList, LVM_SETITEMW, 0, (LPARAM)&lvi);

    // Full path column
    lvi.iSubItem = 2;
    lvi.pszText = e.szImg;
    SendMessageW(m_hSpriteList, LVM_SETITEMW, 0, (LPARAM)&lvi);
  }
}

void Engine::UpdateSpriteProperties(int sel) {
  HWND hw = (m_spritesWindow && m_spritesWindow->IsOpen()) ? m_spritesWindow->GetHWND() :
            m_settingsWindow ? m_settingsWindow->GetHWND() : NULL;
  if (!hw || sel < 0 || sel >= (int)m_spriteEntries.size()) return;

  auto& e = m_spriteEntries[sel];

  SetDlgItemTextW(hw, IDC_MW_SPR_IMG_PATH, e.szImg);

  // Parse init code for default variable values
  // Defaults
  double x = 0.5, y2 = 0.5, sx = 1.0, sy = 1.0, rot = 0.0;
  double r = 1.0, g = 1.0, b = 1.0, a = 1.0;
  int blendmode = 0;
  bool flipx = false, flipy = false, burn = false;
  double repeatx = 1.0, repeaty = 1.0;

  // Simple parser: look for "varname = value" or "varname=value" in init code
  auto parseVar = [&](const std::string& code, const char* name, double& val) {
    size_t pos = 0;
    std::string search = std::string(name) + "=";
    std::string search2 = std::string(name) + " =";
    while (pos < code.size()) {
      size_t found = code.find(search, pos);
      size_t found2 = code.find(search2, pos);
      size_t f = std::string::npos;
      if (found != std::string::npos && (found2 == std::string::npos || found <= found2)) f = found;
      else f = found2;
      if (f == std::string::npos) break;
      // Find the value after =
      size_t eq = code.find('=', f);
      if (eq != std::string::npos) {
        val = atof(code.c_str() + eq + 1);
        return;
      }
      pos = f + 1;
    }
  };

  parseVar(e.szInitCode, "x", x);
  parseVar(e.szInitCode, "y", y2);
  parseVar(e.szInitCode, "sx", sx);
  parseVar(e.szInitCode, "sy", sy);
  parseVar(e.szInitCode, "rot", rot);
  parseVar(e.szInitCode, "r", r);
  parseVar(e.szInitCode, "g", g);
  parseVar(e.szInitCode, "b", b);
  parseVar(e.szInitCode, "a", a);
  parseVar(e.szInitCode, "repeatx", repeatx);
  parseVar(e.szInitCode, "repeaty", repeaty);
  double dBlend = 0; parseVar(e.szInitCode, "blendmode", dBlend); blendmode = (int)dBlend;
  double dFlipx = 0; parseVar(e.szInitCode, "flipx", dFlipx); flipx = dFlipx != 0;
  double dFlipy = 0; parseVar(e.szInitCode, "flipy", dFlipy); flipy = dFlipy != 0;
  double dBurn = 0; parseVar(e.szInitCode, "burn", dBurn); burn = dBurn != 0;
  double dLayer = 0; parseVar(e.szInitCode, "layer", dLayer); int layer = (int)dLayer;

  wchar_t buf[64];
  swprintf(buf, 64, L"%.3g", x); SetDlgItemTextW(hw, IDC_MW_SPR_X, buf);
  swprintf(buf, 64, L"%.3g", y2); SetDlgItemTextW(hw, IDC_MW_SPR_Y, buf);
  swprintf(buf, 64, L"%.3g", sx); SetDlgItemTextW(hw, IDC_MW_SPR_SX, buf);
  swprintf(buf, 64, L"%.3g", sy); SetDlgItemTextW(hw, IDC_MW_SPR_SY, buf);
  swprintf(buf, 64, L"%.3g", rot); SetDlgItemTextW(hw, IDC_MW_SPR_ROT, buf);
  swprintf(buf, 64, L"%.3g", r); SetDlgItemTextW(hw, IDC_MW_SPR_R, buf);
  swprintf(buf, 64, L"%.3g", g); SetDlgItemTextW(hw, IDC_MW_SPR_G, buf);
  swprintf(buf, 64, L"%.3g", b); SetDlgItemTextW(hw, IDC_MW_SPR_B, buf);
  swprintf(buf, 64, L"%.3g", a); SetDlgItemTextW(hw, IDC_MW_SPR_A, buf);
  swprintf(buf, 64, L"%.3g", repeatx); SetDlgItemTextW(hw, IDC_MW_SPR_REPEATX, buf);
  swprintf(buf, 64, L"%.3g", repeaty); SetDlgItemTextW(hw, IDC_MW_SPR_REPEATY, buf);
  swprintf(buf, 64, L"0x%06X", e.nColorkey); SetDlgItemTextW(hw, IDC_MW_SPR_COLORKEY, buf);

  SendDlgItemMessageW(hw, IDC_MW_SPR_BLENDMODE, CB_SETCURSEL, blendmode, 0);
  SendDlgItemMessageW(hw, IDC_MW_SPR_LAYER, CB_SETCURSEL, (layer >= 0 && layer <= 1) ? layer : 0, 0);
  // Use ToolWindow's SetChecked — CheckDlgButton doesn't work with BS_OWNERDRAW
  ToolWindow* tw = (m_spritesWindow && m_spritesWindow->IsOpen()) ? (ToolWindow*)m_spritesWindow.get() :
                   (ToolWindow*)m_settingsWindow.get();
  if (tw) { tw->SetChecked(IDC_MW_SPR_FLIPX, flipx); tw->SetChecked(IDC_MW_SPR_FLIPY, flipy); tw->SetChecked(IDC_MW_SPR_BURN, burn); }

  // Set code editors - convert \n to \r\n for edit control
  {
    std::wstring wInit(e.szInitCode.begin(), e.szInitCode.end());
    SetDlgItemTextW(hw, IDC_MW_SPR_INIT_CODE, wInit.c_str());
  }
  {
    std::wstring wFrame(e.szFrameCode.begin(), e.szFrameCode.end());
    SetDlgItemTextW(hw, IDC_MW_SPR_FRAME_CODE, wFrame.c_str());
  }
}

void Engine::SaveCurrentSpriteProperties() {
  HWND hw = (m_spritesWindow && m_spritesWindow->IsOpen()) ? m_spritesWindow->GetHWND() :
            m_settingsWindow ? m_settingsWindow->GetHWND() : NULL;
  if (!hw || m_nSpriteSelected < 0 || m_nSpriteSelected >= (int)m_spriteEntries.size()) return;

  auto& e = m_spriteEntries[m_nSpriteSelected];

  // Read image path (preserves relative or absolute as displayed)
  GetDlgItemTextW(hw, IDC_MW_SPR_IMG_PATH, e.szImg, 511);

  // Read colorkey
  wchar_t buf[64];
  GetDlgItemTextW(hw, IDC_MW_SPR_COLORKEY, buf, 64);
  e.nColorkey = (unsigned int)wcstoul(buf, NULL, 16);

  // Rebuild init code from property fields so edits are always captured
  {
    char initBuf[4096];
    wchar_t tmp[64];

    // Read all property values from UI controls
    GetDlgItemTextW(hw, IDC_MW_SPR_X, tmp, 64);    double x  = _wtof(tmp);
    GetDlgItemTextW(hw, IDC_MW_SPR_Y, tmp, 64);    double y  = _wtof(tmp);
    GetDlgItemTextW(hw, IDC_MW_SPR_SX, tmp, 64);   double sx = _wtof(tmp);
    GetDlgItemTextW(hw, IDC_MW_SPR_SY, tmp, 64);   double sy = _wtof(tmp);
    GetDlgItemTextW(hw, IDC_MW_SPR_ROT, tmp, 64);  double rot = _wtof(tmp);
    GetDlgItemTextW(hw, IDC_MW_SPR_R, tmp, 64);    double r  = _wtof(tmp);
    GetDlgItemTextW(hw, IDC_MW_SPR_G, tmp, 64);    double g  = _wtof(tmp);
    GetDlgItemTextW(hw, IDC_MW_SPR_B, tmp, 64);    double b  = _wtof(tmp);
    GetDlgItemTextW(hw, IDC_MW_SPR_A, tmp, 64);    double a  = _wtof(tmp);
    GetDlgItemTextW(hw, IDC_MW_SPR_REPEATX, tmp, 64); double repeatx = _wtof(tmp);
    GetDlgItemTextW(hw, IDC_MW_SPR_REPEATY, tmp, 64); double repeaty = _wtof(tmp);
    int blend = (int)SendDlgItemMessageW(hw, IDC_MW_SPR_BLENDMODE, CB_GETCURSEL, 0, 0);
    if (blend == CB_ERR) blend = 0;
    int layer = (int)SendDlgItemMessageW(hw, IDC_MW_SPR_LAYER, CB_GETCURSEL, 0, 0);
    if (layer == CB_ERR) layer = 0;
    // Use ToolWindow's IsChecked — IsDlgButtonChecked doesn't work with BS_OWNERDRAW
    ToolWindow* tw = (m_spritesWindow && m_spritesWindow->IsOpen()) ? (ToolWindow*)m_spritesWindow.get() :
                     (ToolWindow*)m_settingsWindow.get();
    bool flipx = tw ? tw->IsChecked(IDC_MW_SPR_FLIPX) : false;
    bool flipy = tw ? tw->IsChecked(IDC_MW_SPR_FLIPY) : false;
    bool burn  = tw ? tw->IsChecked(IDC_MW_SPR_BURN)  : false;

    // Build init code from property values
    sprintf(initBuf, "blendmode = %d;\r\nx = %.6g; y = %.6g;\r\nsx = %.6g; sy = %.6g; rot = %.6g;\r\n"
      "r = %.6g; g = %.6g; b = %.6g; a = %.6g;",
      blend, x, y, sx, sy, rot, r, g, b, a);

    std::string code = initBuf;
    if (flipx)              code += "\r\nflipx = 1;";
    if (flipy)              code += "\r\nflipy = 1;";
    if (burn)               code += "\r\nburn = 1;";
    if (layer != 0)         { char lb[64]; sprintf(lb, "\r\nlayer = %d;", layer); code += lb; }
    if (repeatx != 1.0)     { char rb[64]; sprintf(rb, "\r\nrepeatx = %.6g;", repeatx); code += rb; }
    if (repeaty != 1.0)     { char rb[64]; sprintf(rb, "\r\nrepeaty = %.6g;", repeaty); code += rb; }

    // Append any non-property lines from the init code editor (custom code the user added)
    {
      static const char* propNames[] = {
        "blendmode", "x", "y", "sx", "sy", "rot",
        "r", "g", "b", "a", "flipx", "flipy", "burn", "layer", "repeatx", "repeaty", NULL
      };
      int len = GetWindowTextLengthW(GetDlgItem(hw, IDC_MW_SPR_INIT_CODE));
      if (len > 0) {
        std::vector<wchar_t> wbuf(len + 1);
        GetDlgItemTextW(hw, IDC_MW_SPR_INIT_CODE, wbuf.data(), len + 1);
        std::string editorCode;
        for (wchar_t wc : wbuf) { if (wc == 0) break; editorCode += (char)wc; }

        // Parse each line; keep lines that aren't simple property assignments
        std::istringstream ss(editorCode);
        std::string line;
        while (std::getline(ss, line)) {
          if (!line.empty() && line.back() == '\r') line.pop_back();
          if (line.empty()) continue;
          // Check if this line is a known property assignment
          bool isProperty = false;
          for (int i = 0; propNames[i]; i++) {
            std::string pat1 = std::string(propNames[i]) + " =";
            std::string pat2 = std::string(propNames[i]) + "=";
            // Check if line starts with or contains this property as a standalone assignment
            // Trim leading whitespace
            size_t start = line.find_first_not_of(" \t");
            if (start == std::string::npos) continue;
            std::string trimmed = line.substr(start);
            if (trimmed.compare(0, pat1.size(), pat1) == 0 || trimmed.compare(0, pat2.size(), pat2) == 0) {
              isProperty = true;
              break;
            }
          }
          if (!isProperty) {
            code += "\r\n";
            code += line;
          }
        }
      }
    }
    e.szInitCode = code;

    // Update the init code editor to reflect the rebuilt code
    std::wstring wInit(code.begin(), code.end());
    SetDlgItemTextW(hw, IDC_MW_SPR_INIT_CODE, wInit.c_str());
  }

  // Read per-frame code from editor
  {
    int len = GetWindowTextLengthW(GetDlgItem(hw, IDC_MW_SPR_FRAME_CODE));
    if (len > 0) {
      std::vector<wchar_t> wbuf(len + 1);
      GetDlgItemTextW(hw, IDC_MW_SPR_FRAME_CODE, wbuf.data(), len + 1);
      std::string narrow;
      for (wchar_t wc : wbuf) {
        if (wc == 0) break;
        narrow += (char)wc;
      }
      e.szFrameCode = narrow;
    } else {
      e.szFrameCode.clear();
    }
  }
}

bool Engine::LaunchSprite(int nSpriteNum, int nSlot) {
  char initcode[8192], code[8192], sectionA[64];
  char szTemp[8192];
  wchar_t img[512], section[64];

  initcode[0] = 0;
  code[0] = 0;
  img[0] = 0;
  if (nSpriteNum < 100) {
    swprintf(section, L"img%02d", nSpriteNum);
    sprintf(sectionA, "img%02d", nSpriteNum);
  } else {
    swprintf(section, L"img%d", nSpriteNum);
    sprintf(sectionA, "img%d", nSpriteNum);
  }

  // 1. read in image filename
  GetPrivateProfileStringW(section, L"img", L"", img, sizeof(img) - 1, m_szImgIniFile);
  if (img[0] == 0) {
    wchar_t buf[1024];
    swprintf(buf, wasabiApiLangString(IDS_SPRITE_X_ERROR_COULD_NOT_FIND_IMG_OR_NOT_DEFINED), nSpriteNum);
    AddError(buf, 7.0f, ERR_MISC, false);
    return false;
  }

  { wchar_t dbg[1024]; swprintf(dbg, 1024, L"LaunchSprite(%d): img=%s", nSpriteNum, img); DebugLogW(dbg, LOG_VERBOSE); }

  if (img[1] != L':')// || img[2] != '\\')
  {
    // it's not in the form "x:\blah\billy.jpg" so prepend base path.
    // Try content base path first, then fall back to plugin dir path.
    wchar_t temp[512];
    wcscpy(temp, img);
    if (m_szContentBasePath[0]) {
      swprintf(img, L"%s%s", m_szContentBasePath, temp);
      if (GetFileAttributesW(img) == INVALID_FILE_ATTRIBUTES)
        swprintf(img, L"%s%s", m_szMilkdrop2Path, temp);
    } else {
      swprintf(img, L"%s%s", m_szMilkdrop2Path, temp);
    }
  }
  { wchar_t dbg[1024]; swprintf(dbg, 1024, L"LaunchSprite(%d): resolved=%s exists=%d", nSpriteNum, img,
            GetFileAttributesW(img) != INVALID_FILE_ATTRIBUTES); DebugLogW(dbg, LOG_VERBOSE); }

  // 2. get color key
  //unsigned int ck_lo = (unsigned int)GetPrivateProfileInt(section, "colorkey_lo", 0x00000000, m_szImgIniFile);
  //unsigned int ck_hi = (unsigned int)GetPrivateProfileInt(section, "colorkey_hi", 0x00202020, m_szImgIniFile);
    // FIRST try 'colorkey_lo' (for backwards compatibility) and then try 'colorkey'
  unsigned int ck = (unsigned int)GetPrivateProfileIntW(section, L"colorkey_lo", 0x00000000, m_szImgIniFile/*GetConfigIniFile()*/);
  ck = (unsigned int)GetPrivateProfileIntW(section, L"colorkey", ck, m_szImgIniFile/*GetConfigIniFile()*/);

  // 3. read in init code & per-frame code
  for (int n = 0; n < 2; n++) {
    char* pStr = (n == 0) ? initcode : code;
    char szLineName[32];
    int len;

    int line = 1;
    int char_pos = 0;
    bool bDone = false;

    while (!bDone) {
      if (n == 0)
        sprintf(szLineName, "init_%d", line);
      else
        sprintf(szLineName, "code_%d", line);

      GetPrivateProfileString(sectionA, szLineName, "~!@#$", szTemp, 8192, AutoCharFn(m_szImgIniFile));	// fixme
      len = lstrlen(szTemp);

      if ((strcmp(szTemp, "~!@#$") == 0) ||		// if the key was missing,
        (len >= 8191 - char_pos - 1))			// or if we're out of space
      {
        bDone = true;
      }
      else {
        sprintf(&pStr[char_pos], "%s%c", szTemp, LINEFEED_CONTROL_CHAR);
      }

      char_pos += len + 1;
      line++;
    }
    pStr[char_pos++] = 0;	// null-terminate
  }

  if (nSlot == -1) {
    // find first empty slot; if none, chuck the oldest sprite & take its slot.
    int oldest_index = 0;
    int oldest_frame = m_texmgr.m_tex[0].nStartFrame;
    for (int x = 0; x < NUM_TEX; x++) {
      if (!m_texmgr.m_tex[x].pSurface) {
        nSlot = x;
        break;
      }
      else if (m_texmgr.m_tex[x].nStartFrame < oldest_frame) {
        oldest_index = x;
        oldest_frame = m_texmgr.m_tex[x].nStartFrame;
      }
    }

    if (nSlot == -1) {
      nSlot = oldest_index;
      m_texmgr.KillTex(nSlot);
    }
  }

  int ret = m_texmgr.LoadTex(img, nSlot, initcode, code, GetTime(), GetFrame(), ck);
  m_texmgr.m_tex[nSlot].nUserData = nSpriteNum;

  { wchar_t dbg[512]; swprintf(dbg, 512, L"LaunchSprite(%d): slot=%d ret=0x%X valid=%d pSurf=%p",
    nSpriteNum, nSlot, ret,
    m_texmgr.m_tex[nSlot].dx12Surface.IsValid() ? 1 : 0,
    m_texmgr.m_tex[nSlot].pSurface); DebugLogW(dbg, LOG_VERBOSE); }

  wchar_t buf[1024];
  switch (ret & TEXMGR_ERROR_MASK) {
  case TEXMGR_ERR_SUCCESS:
    switch (ret & TEXMGR_WARNING_MASK) {
    case TEXMGR_WARN_ERROR_IN_INIT_CODE:
      swprintf(buf, wasabiApiLangString(IDS_SPRITE_X_WARNING_ERROR_IN_INIT_CODE), nSpriteNum);
      AddError(buf, 6.0f, ERR_MISC, true);
      break;
    case TEXMGR_WARN_ERROR_IN_REG_CODE:
      swprintf(buf, wasabiApiLangString(IDS_SPRITE_X_WARNING_ERROR_IN_PER_FRAME_CODE), nSpriteNum);
      AddError(buf, 6.0f, ERR_MISC, true);
      break;
    default:
      // success; no errors OR warnings.
      break;
    }
    break;
  case TEXMGR_ERR_BAD_INDEX:
    swprintf(buf, wasabiApiLangString(IDS_SPRITE_X_ERROR_BAD_SLOT_INDEX), nSpriteNum);
    AddError(buf, 6.0f, ERR_MISC, true);
    break;
    /*
      case TEXMGR_ERR_OPENING:                sprintf(m_szUserMessage, "sprite #%d error: unable to open imagefile", nSpriteNum); break;
    case TEXMGR_ERR_FORMAT:                 sprintf(m_szUserMessage, "sprite #%d error: file is corrupt or non-jpeg image", nSpriteNum); break;
    case TEXMGR_ERR_IMAGE_NOT_24_BIT:       sprintf(m_szUserMessage, "sprite #%d error: image does not have 3 color channels", nSpriteNum); break;
    case TEXMGR_ERR_IMAGE_TOO_LARGE:        sprintf(m_szUserMessage, "sprite #%d error: image is too large", nSpriteNum); break;
    case TEXMGR_ERR_CREATESURFACE_FAILED:   sprintf(m_szUserMessage, "sprite #%d error: createsurface() failed", nSpriteNum); break;
    case TEXMGR_ERR_LOCKSURFACE_FAILED:     sprintf(m_szUserMessage, "sprite #%d error: lock() failed", nSpriteNum); break;
    case TEXMGR_ERR_CORRUPT_JPEG:           sprintf(m_szUserMessage, "sprite #%d error: jpeg is corrupt", nSpriteNum); break;
      */
  case TEXMGR_ERR_BADFILE:
    swprintf(buf, wasabiApiLangString(IDS_SPRITE_X_ERROR_IMAGE_FILE_MISSING_OR_CORRUPT), nSpriteNum);
    AddError(buf, 6.0f, ERR_MISC, true);
    break;
  case TEXMGR_ERR_OUTOFMEM:
    swprintf(buf, wasabiApiLangString(IDS_SPRITE_X_ERROR_OUT_OF_MEM), nSpriteNum);
    AddError(buf, 6.0f, ERR_MISC, true);
    break;
  }

  return (ret & TEXMGR_ERROR_MASK) ? false : true;
}

void Engine::KillSprite(int iSlot) {
  m_texmgr.KillTex(iSlot);
}

} // namespace mdrop
