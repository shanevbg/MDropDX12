// video_effect_params.h — the Video Effects parameter set.
//
// The values a video input is composited with: transform, colour, effects,
// blend mode, and the audio-reactive links that drive them. Pure data, no
// engine, no rendering, no file format.
//
// These lived as nested types inside Engine, which meant every consumer said
// Engine::VideoEffectParams and nothing could touch a parameter set without
// pulling in a ~1,900 line header. They are the currency between the engine's
// live state (Engine::m_videoFX) and the profile store (vfx_profile_store.h),
// so they belong to neither.

#pragma once

namespace mdrop {

struct AudioLink {
    int   source    = 0;    // 0=none, 1=bass, 2=mid, 3=treb, 4=vol
    float intensity = 0.5f; // 0.0–2.0
};

struct VideoEffectParams {
    // Transform
    float posX = 0, posY = 0;       // -1 to 1
    float scale = 1.0f;             // 0.1 to 5.0
    float rotation = 0;             // 0–360 degrees
    bool  mirrorH = false, mirrorV = false;
    // Color
    float tintR = 1, tintG = 1, tintB = 1; // 0–2
    float brightness = 0;           // -1 to 1
    float contrast = 1.0f;          // 0–3
    float saturation = 1.0f;        // 0–3
    float hueShift = 0;             // 0–360
    bool  invert = false;
    // Effects
    float pixelation = 0;           // 0 (off) to 1 (max)
    float chromatic = 0;            // 0 (off) to 0.05
    bool  edgeDetect = false;
    // Blend: 0=Alpha, 1=Additive, 2=Multiply, 3=Screen, 4=Overlay, 5=Difference
    int   blendMode = 0;
    // Audio-reactive links
    AudioLink arPosX, arPosY, arScale, arRotation;
    AudioLink arBrightness, arSaturation, arChromatic;

    bool IsDefault() const {
        return posX == 0 && posY == 0 && scale == 1.0f && rotation == 0
            && !mirrorH && !mirrorV
            && tintR == 1 && tintG == 1 && tintB == 1
            && brightness == 0 && contrast == 1.0f && saturation == 1.0f
            && hueShift == 0 && !invert
            && pixelation == 0 && chromatic == 0 && !edgeDetect
            && blendMode == 0
            && arPosX.source == 0 && arPosY.source == 0
            && arScale.source == 0 && arRotation.source == 0
            && arBrightness.source == 0 && arSaturation.source == 0
            && arChromatic.source == 0;
    }

    // Field-by-field rather than memcmp: this struct has padding between the
    // bools and the floats, and padding bytes are indeterminate, so memcmp
    // would report spurious differences and keep the Save button permanently
    // red.
    bool Equals(const VideoEffectParams& o) const {
        auto sameLink = [](const AudioLink& a, const AudioLink& b) {
            return a.source == b.source && a.intensity == b.intensity;
        };
        return posX == o.posX && posY == o.posY && scale == o.scale
            && rotation == o.rotation && mirrorH == o.mirrorH && mirrorV == o.mirrorV
            && tintR == o.tintR && tintG == o.tintG && tintB == o.tintB
            && brightness == o.brightness && contrast == o.contrast
            && saturation == o.saturation && hueShift == o.hueShift && invert == o.invert
            && pixelation == o.pixelation && chromatic == o.chromatic
            && edgeDetect == o.edgeDetect && blendMode == o.blendMode
            && sameLink(arPosX, o.arPosX) && sameLink(arPosY, o.arPosY)
            && sameLink(arScale, o.arScale) && sameLink(arRotation, o.arRotation)
            && sameLink(arBrightness, o.arBrightness)
            && sameLink(arSaturation, o.arSaturation)
            && sameLink(arChromatic, o.arChromatic);
    }
};

}  // namespace mdrop
