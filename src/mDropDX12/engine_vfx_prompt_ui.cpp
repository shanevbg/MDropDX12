// engine_vfx_prompt_ui.cpp — "Keep video effect changes?" prompt
//
// A preset (or one of its tags) can name a VFX profile, which is applied for
// as long as that preset is on screen and then undone. If the user changes a
// video effect DURING that, undoing it silently would throw away a deliberate
// edit -- so the edit is kept, the scope is marked dirty, and leaving the
// preset raises this window instead of restoring.
//
// It is a tool window, not a dialog, on purpose: it runs on its own thread, so
// the render loop never stalls waiting for an answer. Presets can advance every
// few seconds and a modal here would freeze the visualiser.
//
// Both buttons call Engine::AnswerScopedVFX -- the same function the
// VFX_SCOPED_KEEP IPC command calls -- so the path the tests exercise is the
// path the buttons take.

#include "engine.h"
#include "tool_window.h"
#include "engine_helpers.h"
#include "utility.h"

namespace mdrop {

VFXKeepPromptWindow::VFXKeepPromptWindow(Engine* pEngine)
    : ToolWindow(pEngine, 470, 280) {}

// ---------------------------------------------------------------------------
// Open / Close wrappers on Engine
// ---------------------------------------------------------------------------
void Engine::OpenVFXKeepPrompt(const std::wstring& profileName)
{
    if (!m_pVFXKeepPromptWindow)
        m_pVFXKeepPromptWindow = new VFXKeepPromptWindow(this);
    m_pVFXKeepPromptWindow->SetProfileName(profileName);
    m_pVFXKeepPromptWindow->Open();
}

void Engine::CloseVFXKeepPrompt()
{
    if (m_pVFXKeepPromptWindow) {
        m_pVFXKeepPromptWindow->Close();
        delete m_pVFXKeepPromptWindow;
        m_pVFXKeepPromptWindow = nullptr;
    }
}

// ---------------------------------------------------------------------------
// DoBuildControls
// ---------------------------------------------------------------------------
void VFXKeepPromptWindow::DoBuildControls()
{
    auto base = BuildBaseControls();
    int y = base.y, lineH = base.lineH, gap = base.gap;
    int x = base.x, rw = base.rw;

    RECT rc;
    GetClientRect(m_hWnd, &rc);
    const int clientH = rc.bottom;

    // Buttons are anchored to the BOTTOM, not placed after the text.
    //
    // Laid out top-down they ran past the end of a window that does not
    // scroll, and the first build of this window shipped with both buttons
    // invisible -- the one thing a prompt cannot afford. Anchoring means the
    // answer is reachable at any window size or HUD font.
    const int btnGap = 8;
    const int col2 = (rw - btnGap) / 2;
    const int btnY = clientH - gap - lineH;
    CreateBtn(m_hWnd, L"Keep", IDC_MW_VFXPROMPT_KEEP, x, btnY, col2, lineH, m_hFont);
    CreateBtn(m_hWnd, L"Discard", IDC_MW_VFXPROMPT_DISCARD,
              x + col2 + btnGap, btnY, col2, lineH, m_hFont);

    // The message fills whatever is left above them. One multi-line static
    // that wraps, rather than hand-split lines that break in odd places when
    // the profile name is long.
    const std::wstring who = m_profile.empty() ? std::wstring(L"a profile")
                                               : (L"\"" + m_profile + L"\"");
    const std::wstring msg =
        L"Video effects changed while this preset used " + who +
        L".\r\nKeep them in that profile, or discard?";

    const int textH = max(lineH, btnY - gap - y);
    HWND hMsg = CreateWindowExW(0, L"STATIC", msg.c_str(),
                                WS_CHILD | WS_VISIBLE | SS_LEFT | SS_EDITCONTROL,
                                x, y, rw, textH, m_hWnd, NULL, NULL, NULL);
    if (hMsg) {
        SendMessageW(hMsg, WM_SETFONT, (WPARAM)m_hFont, TRUE);
        m_childCtrls.push_back(hMsg);   // so the dark theme paints it
    }
}

// ---------------------------------------------------------------------------
// DoCommand
// ---------------------------------------------------------------------------
LRESULT VFXKeepPromptWindow::DoCommand(HWND hWnd, int id, int code, LPARAM lParam)
{
    if (id != IDC_MW_VFXPROMPT_KEEP && id != IDC_MW_VFXPROMPT_DISCARD)
        return -1;

    // Answer first, then close. AnswerScopedVFX both writes (on Keep) and
    // restores the pre-preset state, so nothing else has to be done here.
    m_pEngine->AnswerScopedVFX(id == IDC_MW_VFXPROMPT_KEEP);

    // Closing deletes this object, so post rather than calling Close() from
    // inside its own message handler.
    PostMessageW(m_hWnd, WM_CLOSE, 0, 0);
    return 0;
}

} // namespace mdrop
