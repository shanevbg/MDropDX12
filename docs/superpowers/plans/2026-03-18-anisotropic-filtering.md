# Anisotropic Filtering Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add a checkbox to the Visual ToolWindow that toggles 16x anisotropic filtering on the two linear DX12 static samplers (s0, s1), with INI persistence and runtime pipeline rebuild.

**Architecture:** The DX12 root signature contains 4 static samplers baked at init. Toggling aniso requires recreating the root signature + all PSOs + buffers. A new `RenderCmd::ResetPipeline` command handles the full pipeline rebuild on the render thread. The setting is read from INI during DXContext construction (before `Internal_Init` creates the root signature) and persisted by the Engine.

**Tech Stack:** C++17, DirectX 12, Win32 API

**Spec:** `docs/superpowers/specs/2026-03-18-anisotropic-filtering-design.md`

---

### Task 1: Add control ID and window message constants

**Files:**
- Modify: `src/mDropDX12/engine_helpers.h:351` (control IDs), `src/mDropDX12/engine_helpers.h:984` (WM messages)

- [ ] **Step 1: Add IDC_MW_ANISO control ID**

In `engine_helpers.h`, after the line `#define IDC_MW_WTP_WINDOWS 2192` (line 351), add:

```cpp
#define IDC_MW_ANISO            2193  // Checkbox: Anisotropic Filtering (Visual tab)
```

- [ ] **Step 2: Add WM_MW_RESET_PIPELINE window message**

In `engine_helpers.h`, after the line `#define WM_MW_BRING_TO_TOP (WM_APP + 26)` (line 984), add:

```cpp
#define WM_MW_RESET_PIPELINE    (WM_APP + 27) // recreate root signature + all PSOs (aniso toggle)
```

- [ ] **Step 3: Commit**

```bash
git add src/mDropDX12/engine_helpers.h
git commit -m "feat: add anisotropic filtering control ID and pipeline reset message"
```

---

### Task 2: Add RenderCmd::ResetPipeline enum value

**Files:**
- Modify: `src/mDropDX12/render_commands.h:30`

- [ ] **Step 1: Add ResetPipeline to the RenderCmd enum**

In `render_commands.h`, add `ResetPipeline` before `Quit` (line 30):

```cpp
    DisableAllOutputs,   // Ctrl+F2 kill switch
    ResetPipeline,       // RecreateRootSigAndPipelines() + ResetBufferAndFonts()
    Quit,                // Clean shutdown
```

- [ ] **Step 2: Commit**

```bash
git add src/mDropDX12/render_commands.h
git commit -m "feat: add ResetPipeline render command"
```

---

### Task 3: Uncomment Engine member variable and INI persistence

**Files:**
- Modify: `src/mDropDX12/engine.h:672`
- Modify: `src/mDropDX12/engine.cpp:1193` (default), `src/mDropDX12/engine.cpp:1477` (read), `src/mDropDX12/engine.cpp:1776` (write)

- [ ] **Step 1: Uncomment m_bAnisotropicFiltering in engine.h**

Change line 672 from:
```cpp
  //bool        m_bAnisotropicFiltering;
```
to:
```cpp
  bool        m_bAnisotropicFiltering;
```

- [ ] **Step 2: Set default to false in OverrideDefaults (engine.cpp:1193)**

Change line 1193 from:
```cpp
  //m_bAnisotropicFiltering = true;
```
to:
```cpp
  m_bAnisotropicFiltering = false;
```

- [ ] **Step 3: Uncomment and update INI read to wide-string API (engine.cpp:1477)**

Change line 1477 from:
```cpp
  //m_bAnisotropicFiltering = GetPrivateProfileBool("settings","bAnisotropicFiltering",m_bAnisotropicFiltering,pIni);
```
to:
```cpp
  m_bAnisotropicFiltering = GetPrivateProfileBoolW(L"Settings", L"bAnisotropicFiltering", m_bAnisotropicFiltering, pIni);
```

- [ ] **Step 4: Uncomment and update INI write to wide-string API (engine.cpp:1776)**

Change line 1776 from:
```cpp
  //WritePrivateProfileIntW(m_bAnisotropicFiltering,	"bAnisotropicFiltering",pIni, "settings");
```
to:
```cpp
  WritePrivateProfileIntW(m_bAnisotropicFiltering, L"bAnisotropicFiltering", pIni, L"Settings");
```

- [ ] **Step 5: Commit**

```bash
git add src/mDropDX12/engine.h src/mDropDX12/engine.cpp
git commit -m "feat: uncomment anisotropic filtering member and INI persistence"
```

---

### Task 4: Add aniso flag to DXContext and modify CreateRootSignature

**Files:**
- Modify: `src/mDropDX12/dxcontext.h:191`
- Modify: `src/mDropDX12/dxcontext.cpp:89-95` (constructor, read INI), `src/mDropDX12/dxcontext.cpp:934-936` (CreateRootSignature)

- [ ] **Step 1: Add m_bAnisotropicFiltering and RecreateRootSigAndPipelines to DXContext header**

In `dxcontext.h`, after line 191 (`bool CreateRootSignature();`), add:

```cpp
  bool RecreateRootSigAndPipelines();
  bool m_bAnisotropicFiltering = false;
```

- [ ] **Step 2: Read aniso flag from INI in DXContext constructor (before Internal_Init)**

In `dxcontext.cpp`, right before line 94 (`// Store device references passed from the initializer`), add:

```cpp
    // Read anisotropic filtering setting from INI (needed before CreateRootSignature in Internal_Init)
    m_bAnisotropicFiltering = GetPrivateProfileIntW(L"Settings", L"bAnisotropicFiltering", 0, m_szIniFile) != 0;
```

- [ ] **Step 3: Add aniso override in CreateRootSignature**

In `dxcontext.cpp`, after the line `staticSamplers[3].Filter = D3D12_FILTER_MIN_MAG_MIP_POINT;` (line 935) and before the `D3D12_ROOT_SIGNATURE_DESC` block (line 937), add:

```cpp
    // Anisotropic filtering: upgrade LINEAR samplers (s0, s1) to ANISOTROPIC
    if (m_bAnisotropicFiltering) {
        staticSamplers[0].Filter = D3D12_FILTER_ANISOTROPIC;
        staticSamplers[0].MaxAnisotropy = 16;
        staticSamplers[1].Filter = D3D12_FILTER_ANISOTROPIC;
        staticSamplers[1].MaxAnisotropy = 16;
    }
```

- [ ] **Step 4: Implement RecreateRootSigAndPipelines**

In `dxcontext.cpp`, after the closing `}` of `CreateRootSignature()` (line 962), add:

```cpp
bool DXContext::RecreateRootSigAndPipelines()
{
    WaitForGpu();
    m_rootSignature.Reset();
    m_blurRootSignature.Reset();
    for (auto& pso : m_PSOs)
        pso.Reset();
    if (!CreateRootSignature()) return false;
    if (!CreatePipelines()) return false;
    return true;
}
```

- [ ] **Step 5: Commit**

```bash
git add src/mDropDX12/dxcontext.h src/mDropDX12/dxcontext.cpp
git commit -m "feat: add anisotropic filtering to DX12 root signature samplers"
```

---

### Task 5: Add ResetPipeline handler in engine_input and engineshell

**Files:**
- Modify: `src/mDropDX12/engine_input.cpp:520-522`
- Modify: `src/mDropDX12/engineshell.cpp:2975-2977`

- [ ] **Step 1: Handle WM_MW_RESET_PIPELINE in engine_input.cpp**

After the `WM_MW_RESET_BUFFERS` case (line 520-522), add:

```cpp
  case WM_MW_RESET_PIPELINE:
    EnqueueRenderCmd(RenderCmd::ResetPipeline);
    return 0;
```

- [ ] **Step 2: Handle RenderCmd::ResetPipeline in engineshell.cpp**

In `ExecuteRenderCommand()`, after the `ResetBuffers` case (line 2975-2977), add:

```cpp
    case RenderCmd::ResetPipeline:
      if (m_lpDX && m_lpDX->RecreateRootSigAndPipelines())
        ResetBufferAndFonts();
      break;
```

Note: `ResetBufferAndFonts()` calls `CleanUpDX9Stuff(0)` + `AllocateDX9Stuff()` which rebuilds ALL remaining PSOs (preset warp/comp, fallback, blur, inject effect, spout, buffer A/B/C/D) that also reference the root signature.

- [ ] **Step 3: Commit**

```bash
git add src/mDropDX12/engine_input.cpp src/mDropDX12/engineshell.cpp
git commit -m "feat: add pipeline reset handler for anisotropic filtering toggle"
```

---

### Task 6: Add checkbox to Visual ToolWindow

**Files:**
- Modify: `src/mDropDX12/engine_visual_ui.cpp:101` (UI creation), `src/mDropDX12/engine_visual_ui.cpp:274` (reset handler), `src/mDropDX12/engine_visual_ui.cpp:324-341` (checkbox handler)

- [ ] **Step 1: Add the checkbox control after the Texture Precision combo**

In `engine_visual_ui.cpp`, after the line `y += lineH + gap + 4;` (line 101), add:

```cpp
  // Anisotropic Filtering
  TrackControl(CreateCheck(hw, L"Anisotropic Filtering", IDC_MW_ANISO, x, y, 200, lineH, hFont, p->m_bAnisotropicFiltering, true));
  y += lineH + gap;
```

- [ ] **Step 2: Add checkbox handler in DoCommand BN_CLICKED switch**

In `engine_visual_ui.cpp`, inside the `if (code == BN_CLICKED)` switch block (after the `IDC_MW_VSYNC_ENABLED` case at line 338-340), add:

```cpp
    case IDC_MW_ANISO:
      p->m_bAnisotropicFiltering = bChecked;
      if (p->m_lpDX) p->m_lpDX->m_bAnisotropicFiltering = bChecked;
      { HWND hw = p->GetPluginWindow();
        if (hw) PostMessage(hw, WM_MW_RESET_PIPELINE, 0, 0); }
      return 0;
```

- [ ] **Step 3: Add reset logic for the checkbox in the Reset button handler**

In `engine_visual_ui.cpp`, inside the `IDC_MW_RESET_VISUAL` handler, after the line `SetChecked(IDC_MW_VSYNC_ENABLED, true);` (line 294), add:

```cpp
    p->m_bAnisotropicFiltering = false;
    if (p->m_lpDX) p->m_lpDX->m_bAnisotropicFiltering = false;
    SetChecked(IDC_MW_ANISO, false);
```

Also change the `PostMessage` for side-effects at line 298 from `WM_MW_RESET_BUFFERS` to `WM_MW_RESET_PIPELINE` so the root signature is rebuilt on reset:

```cpp
    if (hw) PostMessage(hw, WM_MW_RESET_PIPELINE, 0, 0);
```

- [ ] **Step 4: Commit**

```bash
git add src/mDropDX12/engine_visual_ui.cpp
git commit -m "feat: add Anisotropic Filtering checkbox to Visual window"
```

---

### Task 7: Build and test

**Files:** None (verification only)

- [ ] **Step 1: Build**

```bash
powershell -ExecutionPolicy Bypass -File build.ps1 Release x64
```

Expected: Build succeeds with 0 errors.

- [ ] **Step 2: Launch and verify**

```bash
cd src/mDropDX12/Release_x64 && start MDropDX12.exe
```

Manual test checklist:
1. Open Visual window (Settings F8 → Tools → Visual)
2. Verify "Anisotropic Filtering" checkbox appears below Texture Precision combo
3. Check the box — visualizer should briefly reset and continue rendering (pipeline rebuild)
4. Uncheck the box — same reset behavior, back to bilinear
5. Click Reset button — verify checkbox resets to unchecked
6. Close and reopen the app — verify the setting persists from INI

- [ ] **Step 3: Commit any build fixes if needed**

---

### Task 8: Final commit (squash if desired)

- [ ] **Step 1: Verify all changes are committed**

```bash
git status
git log --oneline -10
```

All tasks above produce individual commits. The user may choose to squash them into a single feature commit.
