// comp_inversion.h — does this preset's composite shader invert its feedback?
//
// A warning, never an action. The feedback damp scales the accumulated history
// down each frame, which calms a preset whose output rises with its feedback.
// On a preset whose composite shader is a DECREASING function of the feedback
// it samples, the same knob does the opposite: the picture gets brighter the
// harder it is damped, all the way to white. Measured on
// "rarian rakista - Eyes through the ether", walking the multiplier
// 1.00 -> 0.90 -> 0.80 -> 0.70 took the mean 0.49 -> 0.87 -> 0.96 -> 0.98.
//
// Two forms account for the cases found so far, and both are visible in the
// shader text without running anything:
//
//     ret = 1 - ret;                    a plain terminal inversion
//     ret = 1 - ret*(1 - ret)*4;        a solarize fold, i.e. (1-2*ret)^2,
//                                       whose slope is negative below mid grey
//
// So the check is a text scan of the concatenated comp body. It is a HEURISTIC
// over arbitrary HLSL and is wrong in both directions on code it has not seen:
// it cannot follow a value through a chain of temporaries, and a `1 - x` on a
// quantity that is not the feedback will fool it. That is precisely why it only
// ever produces a hint next to the Damp control. Neither mitigation is applied
// automatically -- which fix a preset wants is a judgement about that preset,
// and this file is not equipped to make it.

#pragma once

#include <cstring>
#include <string>

namespace mdrop {

// A comp shader whose final write to `ret` inverts or folds it. Cheap enough to
// run on every preset load; no allocation beyond one lowercased copy.
inline bool CompShaderInvertsFeedback(const char* szCompText)
{
    if (!szCompText || !*szCompText) return false;

    // Strip comments and whitespace and lowercase, so the patterns below do not
    // each need to cope with `ret=1-ret`, `ret = 1 - ret` and `RET = 1-RET`.
    //
    // 0x01 is a LINE BREAK here, not stray data. MilkDrop stores a multi-line
    // preset string with 0x01 between lines and converts to a real newline only
    // when the shader reaches the compiler, so m_szCompShadersText contains no
    // newline at all. Miss that and a `//` comment runs to the end of the whole
    // shader: "rarian rakista - Eyes through the ether" ends
    //
    //     ret *= 1.00; //gamma <0x01>    ret = 1 - ret*(1 - ret)*4; //invert
    //
    // and treating 0x01 as ordinary whitespace let `//gamma` swallow the very
    // inversion this function exists to find -- it reported "no inversion" on
    // the preset that motivated the check.
    std::string s;
    s.reserve(strlen(szCompText));
    bool inLine = false, inBlock = false;
    for (const char* p = szCompText; *p; ++p) {
        if (inLine) { if (*p == '\n' || *p == '\x01') inLine = false; continue; }
        if (inBlock) {
            if (p[0] == '*' && p[1] == '/') { inBlock = false; ++p; }
            continue;
        }
        if (p[0] == '/' && p[1] == '/') { inLine = true; ++p; continue; }
        if (p[0] == '/' && p[1] == '*') { inBlock = true; ++p; continue; }
        if (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n' ||
            *p == '\x01') continue;
        s += (char)((*p >= 'A' && *p <= 'Z') ? (*p - 'A' + 'a') : *p);
    }
    if (s.empty()) return false;

    // Only the LAST assignment to ret decides the output's sign. An inversion
    // halfway down is usually undone, and flagging it would make the warning
    // fire on presets the damp handles perfectly well.
    static const char* kLhs[] = { "ret=", "ret.xyz=", "ret.rgb=" };
    size_t at = std::string::npos;
    size_t lhsLen = 0;
    for (const char* lhs : kLhs) {
        const size_t n = strlen(lhs);
        size_t pos = s.rfind(lhs);
        // rfind can land on "xret=" or "myret="; require a non-identifier before it.
        while (pos != std::string::npos) {
            const char c = pos ? s[pos - 1] : ';';
            const bool ident = (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_' || c == '.';
            if (!ident) break;
            pos = pos ? s.rfind(lhs, pos - 1) : std::string::npos;
        }
        if (pos != std::string::npos && (at == std::string::npos || pos > at)) {
            at = pos;
            lhsLen = n;
        }
    }
    if (at == std::string::npos) return false;

    const size_t end = s.find(';', at);
    const std::string rhs = s.substr(at + lhsLen,
                                     (end == std::string::npos) ? std::string::npos
                                                                : end - at - lhsLen);
    if (rhs.empty()) return false;

    // `1-ret`, `1.0-ret`, `.71-ret`: a constant minus the feedback. Matched at
    // the START of the right-hand side, so `a*(1-ret)` -- a blend, not an
    // inversion -- does not trip it.
    const size_t minus = rhs.find('-');
    if (minus != std::string::npos && minus > 0) {
        const std::string lead = rhs.substr(0, minus);
        bool numeric = true;
        for (char c : lead)
            if (!((c >= '0' && c <= '9') || c == '.')) { numeric = false; break; }
        if (numeric && rhs.find("ret", minus) != std::string::npos)
            return true;
    }

    // A fold: `ret*(1-ret)` is a parabola, and any constant minus it inverts
    // the slope over the lower half of the range.
    if (rhs.find("ret*(1-ret)") != std::string::npos) return true;
    if (rhs.find("abs(ret-.5)") != std::string::npos ||
        rhs.find("abs(ret-0.5)") != std::string::npos) return true;

    return false;
}

}  // namespace mdrop
