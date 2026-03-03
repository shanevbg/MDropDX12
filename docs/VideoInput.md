# Video Input in MDropDX12

MDropDX12 can receive live video from a webcam, play a video file, or accept an external Spout sender, and composite it with the visualizer output. Use this as a background behind the preset (with the visualizer effects warping over the video) or as an overlay on top. A luma key effect lets dark parts of the video become transparent, blending the video feed with the visualizer in creative ways.

## Quick Start

1. Open **Settings** (F8) → **Displays** tab
2. Under **Video Input**, set **Source** to one of:
   - **Webcam** — capture from a connected camera
   - **Video File** — play a video file from disk
   - **Spout** — receive video from an external Spout sender
3. Choose **Background** or **Overlay** layer
4. Adjust **Opacity** and optionally enable **Luma Key**

## Source Types

### Webcam

Captures live video from a connected USB or integrated camera using Windows Media Foundation.

- Select the camera from the **Webcam** dropdown (or leave as "Default" for the first available device)
- Click **Refresh** to re-scan for newly connected cameras
- The capture starts immediately when the source is selected

Webcam resolution is determined by the camera's default settings. Most webcams default to 640x480 or 1280x720.

### Video File

Plays a video file from disk. Supported formats include any format that Windows Media Foundation can decode:

| Format | Extension |
|--------|-----------|
| MPEG-4 | `.mp4` |
| AVI | `.avi` |
| Windows Media | `.wmv` |
| Matroska | `.mkv` |
| QuickTime | `.mov` |

- Click **Browse...** to select a file
- Enable **Loop video** to repeat playback when the file ends
- Playback is paced to the video's native frame rate

Codec support depends on your Windows installation. H.264 and H.265/HEVC are supported on most systems. If a file fails to open, check that the required codec is installed.

### Spout

Receives video from any application that sends via the [Spout](https://spout.zeal.co/) protocol. This is useful for:

- Sending video from OBS, Unity, TouchDesigner, or other creative tools
- Chaining multiple visualizers together
- Advanced camera setups with preprocessing

Select a specific sender from the **Sender** dropdown, or leave as "Auto" to connect to the first available sender. Click **Refresh** to update the sender list.

## Layer Modes

| Mode | Description |
|------|-------------|
| **Background** | Video is drawn onto the preset's input texture (VS[0]) before the warp pass. The preset's warp distortion, shapes, and comp shader all render on top of the video. This creates an effect where the visualizer "warps" the video feed. |
| **Overlay** | Video is drawn onto the final output after the comp pass. The video appears on top of the preset, useful for picture-in-picture effects or when using luma key to blend video with the visualizer. |

## Opacity

Controls the overall transparency of the video layer (0% = fully transparent, 100% = fully opaque). This applies regardless of layer mode.

## Luma Key

Luma key makes dark parts of the video transparent based on brightness (luminance). This is especially useful for:

- Showing the visualizer through dark areas of a webcam feed
- Creating silhouette effects where only bright parts of the video are visible
- Blending a video background with the preset

### How It Works

Each pixel's brightness is calculated using the Rec.709 formula:

```
luminance = 0.299 × R + 0.587 × G + 0.114 × B
```

Pixels darker than the **Threshold** become fully transparent. Pixels brighter than Threshold + **Softness** are fully opaque. Pixels in between are partially transparent, creating a smooth transition.

### Settings

| Setting | Range | Description |
|---------|-------|-------------|
| **Threshold** | 0–100% | Brightness cutoff. Pixels below this are transparent. Start with 10–20% for typical webcam feeds. |
| **Softness** | 0–100% | Width of the transition zone. Higher values create a softer, more gradual blend. 10–30% works well for most uses. |

**Tip:** For a green-screen style effect, point a webcam at a dark background. Set Threshold to ~15% and Softness to ~10%. The dark background disappears, leaving only your lit subject overlaid on the visualizer.

## Aspect Ratio

Video input uses **cover mode**: the video is scaled to fill the target area while maintaining its aspect ratio. If the video's aspect ratio doesn't match the target, excess is cropped equally from both sides (or top/bottom).

## Performance

- Webcam capture and video file decoding run on a dedicated background thread and don't block the render loop
- GPU upload happens once per frame and is very fast (a single `CopyTextureRegion` call)
- For best performance, use video sources that match or are smaller than your visualizer resolution
- Setting Source to **None** has zero performance impact

## Settings Reference

All video input settings are stored in `settings.ini` under the `[SpoutInput]` section (the section name is kept for backward compatibility).

| Setting | INI Key | Values | Description |
|---------|---------|--------|-------------|
| Source | `Source` | 0–3 | 0=None, 1=Spout, 2=Webcam, 3=Video File |
| Enabled | `Enabled` | 0/1 | Legacy key (1 = Source is Spout) |
| Layer | `OnTop` | 0/1 | 0=Background, 1=Overlay |
| Opacity | `Opacity` | 0.00–1.00 | Video opacity |
| Luma Key | `LumaKey` | 0/1 | Enable luma key transparency |
| Threshold | `LumaThreshold` | 0.00–1.00 | Luma key brightness cutoff |
| Softness | `LumaSoftness` | 0.00–1.00 | Luma key transition width |
| Spout Sender | `SenderName` | text | Spout sender name (empty = auto) |
| Webcam Device | `WebcamDevice` | text | Webcam friendly name (empty = default) |
| Video File | `VideoFile` | path | Path to video file |
| Loop | `VideoLoop` | 0/1 | Loop video file playback |

## Tips

- **Start with Background mode** to see the video warped by the preset — it creates the most interesting effects.
- **Use Luma Key with Overlay mode** to let the visualizer show through dark parts of your webcam feed.
- **Lower opacity** (50–70%) for a subtle video presence that doesn't overpower the visualizer.
- **Loop short video clips** as animated textures — abstract patterns, slow-motion footage, and nature scenes work well.
- **Check debug.log** (`LogLevel=2`) if a video source fails to open. Media Foundation error codes will be logged.
