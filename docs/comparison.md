# Visual Comparison: MDropDX12 vs Milkwave Visualizer

Side-by-side visual comparison of preset rendering between MDropDX12 (DirectX 12) and Milkwave Visualizer (DirectX 9Ex).

| Project | Graphics API | Version |
| ------- | ------------ | ------- |
| **MDropDX12** | DirectX 12 | v2.4.0 |
| **Milkwave Visualizer** | DirectX 9Ex | v3.5 |

---

## Presets Compared

Each preset loaded on both visualizers simultaneously with identical audio.

### 1. Martin - blue haze

| MDropDX12 | Milkwave |
| --------- | -------- |
| ![MDropDX12](images/comparison/01_blue_haze_mdrop.jpg) | ![Milkwave](images/comparison/01_blue_haze_milkwave.jpg) |

Both render the deep blue-purple background, concentric swirl rings, and bright particle fountain from center with matching color palette (pink/magenta strands, golden highlights, white-hot core). The fire orb sprite renders correctly on both. No visible differences in warp distortion, color grading, or particle behavior.

**Verdict:** Visually equivalent.

### 2. BrainStain - re entry

| MDropDX12 | Milkwave |
| --------- | -------- |
| ![MDropDX12](images/comparison/02_re_entry_mdrop.jpg) | ![Milkwave](images/comparison/02_re_entry_milkwave.jpg) |

Major difference. MDropDX12 renders a bright radial burst filling a large sphere (yellow-green), while Milkwave shows a nearly black screen with only a faint green glow near center. MDropDX12 appears to be accumulating feedback energy much more aggressively — the warp zoom (0.2) and rotation (1.0) amplify the radial pattern through the feedback loop. Milkwave's output suggests the feedback is decaying faster or the initial waveform injection is weaker.

**Verdict:** Significant difference — MDropDX12 much brighter/more active.

### 3. balkhan + IkeC - Tunnel Cylinders

| MDropDX12 | Milkwave |
| --------- | -------- |
| ![MDropDX12](images/comparison/03_tunnel_cylinders_mdrop.jpg) | ![Milkwave](images/comparison/03_tunnel_cylinders_milkwave.jpg) |

This is a comp shader preset with 3D raymarched tunnel geometry. Both renderers produce the same concentric cylindrical tunnel structure with identical blue/orange/peach color gradients, geometric faceting, and spiral depth recession. The central rose-spiral focal point and floating arrow shapes match exactly.

**Verdict:** Visually equivalent.

### 4. Marex + IkeC - Shadow Party Shader Jam 2025

| MDropDX12 | Milkwave |
| --------- | -------- |
| ![MDropDX12](images/comparison/04_shadow_party_mdrop.jpg) | ![Milkwave](images/comparison/04_shadow_party_milkwave.jpg) |

MDropDX12 renders completely black. Milkwave shows the intended output: a raymarched scene of reflective green-to-yellow spheres in a recursive lattice with specular highlights and ambient occlusion. This is likely a shader compilation failure or missing texture binding on DX12 — the comp shader may use features that fail silently.

**Verdict:** Broken on MDropDX12 — black screen.

### 5. Illusion & Rovastar - Clouded Bottle

| MDropDX12 | Milkwave |
| --------- | -------- |
| ![MDropDX12](images/comparison/05_clouded_bottle_mdrop.jpg) | ![Milkwave](images/comparison/05_clouded_bottle_milkwave.jpg) |

Both render the same dark scene with green waveform threads crossing in an X-pattern. The wave line density, color (dark green), and crossing geometry are consistent. MDropDX12 shows slightly more blue tint in some threads; Milkwave's lines appear marginally denser. The overall composition and mood match.

**Verdict:** Visually equivalent.
