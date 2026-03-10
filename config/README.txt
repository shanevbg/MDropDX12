MDropDX12 - DirectX 12 MilkDrop2 Music Visualizer
Maintained by shanevbg — https://github.com/shanevbg/MDropDX12

MDropDX12 is a ground-up DirectX 12 rebuild of the MilkDrop2 visualizer
engine, originally derived from IkeC's Milkwave (which was itself derived
from BeatDrop and MilkDrop2). The rendering backend, text pipeline, settings
UI, texture management, and shader compilation have all been rewritten.

MDropDX12.exe:
The Visualizer. Run this to start. Press F8 to open the Settings window.
Works standalone or with Milkwave Remote for extended control.

settings.ini:
Settings for the Visualizer. Feel free to edit.

sprites.ini:
Sprites definition file for the Visualizer. To display your own images,
add a definition in this file and put the image in resources\sprites.
Trigger them by typing 00..99 in the Visualizer window when Sprite Mode
is selected.

messages.ini:
Messages definition file for the Visualizer. This is the standard MilkDrop
way to display pre-defined messages. Trigger them by typing 00..99 in the
Visualizer window when Message Mode is selected.

precompile.txt:
A list of presets that will be precompiled (once) when you start the
Visualizer. Feel free to edit.

For the full manual, keyboard shortcuts, and feature documentation:
https://github.com/shanevbg/MDropDX12/blob/main/docs/Manual.md

If you find bugs or have feature requests, open an issue on GitHub:
https://github.com/shanevbg/MDropDX12/issues
