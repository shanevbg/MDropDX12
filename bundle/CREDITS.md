# What is in this folder, and who made it

`presets/` holds 93 MilkDrop presets. `textures/` holds 25 images.

## The presets

These are community MilkDrop presets. They are here because they meet four
conditions, and nothing was added that does not:

1. **`.milk` only.** A `.milk` preset is equations plus MilkDrop's own
   standard shader scaffolding. A `.milk3` is a converted GLSL shader and
   carries the original author's terms, which are often explicitly
   no-redistribution. None are included.
2. **No ports of an individual shader artist's published work**, even where
   someone has written the port as a `.milk`. Same concern, different file
   extension.
3. **No third-party textures.** Every preset here needs either no image at
   all, or only the `randNN` slots — which are filled by the photographs in
   `textures/`. 31 of the 93 use them.
4. **Classic MilkDrop lineage** — authors whose presets have been part of
   MilkDrop, projectM and BeatDrop distributions for about twenty years.

Credited in the filenames, with the number of presets each name appears on
(collaborations count for every contributor):

- **Flexi** — 43
- **geiss** — 41
- **Se7enSlasher** — 40
- **martin** — 22
- **fiShbRaiN** — 12
- **bdrv** — 7
- **Stahlregen** — 7
- **MilkDrop2077** — 6
- **Cope** — 4
- **Eo.S.** — 3
- **Rovastar** — 3
- **Shifter** — 3
- **Zylot** — 3
- **Aderrasi** — 2
- **Goody** — 2
- **nil** — 2
- **ShadowHarlequin** — 2
- **Amandio c** — 1
- **Evet** — 1
- **EvilJim** — 1
- **Krash** — 1
- **Neon** — 1
- **ORB** — 1
- **suksma** — 1
- **TobiasWolfBoi** — 1
- **yin** — 1

If you are one of these authors and would rather your work were not
included, say so and it will be removed in the next release.

## The textures

All 25 are photographs taken by Shane Vincent, the author of MDropDX12,
included here with his permission. They are downscaled to 1024 pixels on the
longest edge, which is the engine's default load-time cap — beyond that the
resolution is not visible once an image is sampled onto a warp mesh, and it
costs sixteen times the video memory.

They fill the `randNN` texture slots, so a preset asking for a random image
gets one of these.

## Using your own library instead

Point MDropDX12 at your own presets from **Settings → Files**. Nothing here
is required; the engine starts on a preset compiled into the executable.
