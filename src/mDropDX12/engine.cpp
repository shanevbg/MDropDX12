/*
  LICENSE
  -------
Copyright 2005-2013 Nullsoft, Inc.
All rights reserved.

Redistribution and use in source and binary forms, with or without modification,
are permitted provided that the following conditions are met:

  * Redistributions of source code must retain the above copyright notice,
    this list of conditions and the following disclaimer.

  * Redistributions in binary form must reproduce the above copyright notice,
    this list of conditions and the following disclaimer in the documentation
    and/or other materials provided with the distribution.

  * Neither the name of Nullsoft nor the names of its contributors may be used to
    endorse or promote products derived from this software without specific prior written permission.

THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND ANY EXPRESS OR
IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND
FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR
CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER
IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT
OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
*/
/*
  ##########################################################################################

  case 'q':
    m_pState->m_fVideoEchoZoom /= 1.05f;
    SendPresetWaveInfoToMDropDX12Remote();
    return 0; // we processed (or absorbed) the key
  case 'Q':
    m_pState->m_fVideoEchoZoom *= 1.05f;
    SendPresetWaveInfoToMDropDX12Remote();
    return 0; // we processed (or absorbed) the key
    Order of function calls...

    When the PLUGIN launches
    ------------------------
        INITIALIZATION
            OverrideDefaults
            MyPreInitialize
            MyReadConfig
            << DirectX gets initialized at this point >>
            AllocateMyNonDx9Stuff
            AllocateMyDX9Stuff
        RUNNING
            +--> { CleanUpMyDX9Stuff + AllocateMyDX9Stuff }  // called together when user resizes window or toggles fullscreen<->windowed.
            |    MyRenderFn
            |    MyRenderUI
            |    { MyWindowProc }                            // called, between frames, on mouse/keyboard/system events.  100% threadsafe.
            +----<< repeat >>
        CLEANUP
            CleanUpMyDX9Stuff
            CleanUpMyNonDx9Stuff
            << DirectX gets uninitialized at this point >>

    When the CONFIG PANEL launches
    ------------------------------
        INITIALIZATION
            OverrideDefaults
            MyPreInitialize
            MyReadConfig
            << DirectX gets initialized at this point >>
        RUNNING
            { MyConfigTabProc }                  // called on startup & on keyboard events
        CLEANUP
            [ MyWriteConfig ]                    // only called if user clicked 'OK' to exit
            << DirectX gets uninitialized at this point >>
*/

/*
  NOTES
  -----



  To do
  -----
    -VMS VERSION:
        -based on vms 1.05, but the 'fix slow text' option has been added.
            that includes m_lpDDSText, CTextManager (m_text), changes to
            DrawDarkTranslucentBox, the replacement of all DrawText calls
            (now routed through m_text), and adding the 'fix slow text' cb
            to the config panel.

    -KILLED FEATURES:
        -vj mode

    -NEW FEATURES FOR 1.04:
            -added the following variables for per-frame scripting: (all booleans, except 'gamma')
              wave_usedots, wave_thick, wave_additive, wave_brighten
                gamma, darken_center, wrap, invert, brighten, darken, solarize
                (also, note that echo_zoom, echo_alpha, and echo_orient were already in there,
                 but weren't covered in the documentation!)
        d   -fixed: spectrum w/512 samples + 256 separation -> infinite spike
        d   -reverted dumb changes to aspect ratio stuff
        d   -reverted wave_y fix; now it's backwards, just like it's always been
                (i.e. for wave's y position, 0=bottom and 1=top, which is opposite
                to the convention in the rest of milkdrop.  decided to keep the
                'bug' so presets don't need modified.)
        d   -fixed: Krash: Inconsistency bug - pressing Escape while in the code windows
                for custom waves completely takes you out of the editing menus,
                rather than back to the custom wave menu
        d   -when editing code: fix display of '&' character
        d   -internal texture size now has a little more bias toward a finer texture,
                based on the window size, when set to 'Auto'.  (Before, for example,
                to reach 1024x1024, the window had to be 768x768 or greater; now, it
                only has to be 640x640 (25% of the way there).  I adjusted it because
                before, at in-between resolutions like 767x767, it looked very grainy;
                now it will always look nice and crisp, at any window size, but still
                won't cause too much aliasing (due to downsampling for display).
        d   -fixed: rova:
                When creating presets have commented code // in the per_pixel section when cause error in preset.
                Example nothing in per_frame and just comments in the per_pixel. EXamples on repuest I have a few.
        d   -added kill keys:
                -CTRL+K kills all running sprites
                -CTRL+T kills current song title anim
                -CTRL+Y kills current custom message
        d   -notice to sprite users:
                -in milk_img.ini, color key can't be a range anymore; it's
                    now limited to just a single color.  'colorkey_lo' and
                    'colorkey_hi' have been replaced with just one setting,
                    'colorkey'.
        d   -song titles + custom messages are working again
        ?   -fixed?: crashes on window resize [out of mem]
                -Rova: BTW the same bug as krash with the window resizing.
                -NOT due to the 'integrate w/winamp' option.
                -> might be fixed now (had forgotten to release m_lpDDSText)
        <AFTER BETA 3..>
        d   -added checkbox to config screen to automatically turn SCROLL LOCK on @ startup
        d   -added alphanumeric seeking to the playlist; while playlist is up,
                you can now press A-Z and 0-9 to seek to the next song in the playlist
                that starts with that character.
        d   -<fixed major bug w/illegal mem access on song title launches;
                could have been causing crashing madness @ startup on many systems>
        d   -<fixed bug w/saving 64x48 mesh size>
        d   -<fixed squashed shapes>
        d   -<fixed 'invert' variable>
        d   -<fixed squashed song titles + custom msgs>
        ?   -<might have fixed scroll lock stuff>
        ?   -<might have fixed crashing; could have been due to null ptr for failed creation of song title texture.>
        ?   -<also might have solved any remaining resize or exit bugs by callign SetTexture(NULL)
                in DX8 cleanup.>
        d   -<fixed sizing issues with songtitle font.>
        d   -<fixed a potentially bogus call to deallocate memory on exit, when it was cleaning up the menus.>
        d   -<fixed more scroll lock issues>
        d   -<fixed broken Noughts & Crosses presets; max # of per-frame vars was one too few, after the additions of the new built-in variables.>
        d   -<hopefully fixed waveforms>
        <AFTER BETA 4>
            -now when playlist is up, SHIFT+A-Z seeks upward (while lowercase/regular a-z seeks downward).
            -custom shapes:
                -OH MY GOD
                -increased max. # of custom shapes (and waves) from 3 to 4
                -added 'texture' option, which allows you to use the last frame as a texture on the shape
                    -added "tex_ang" and "tex_zoom" params to control the texture coords
                -each frame, custom shapes now draw BEFORE regular waveform + custom waves
                -added init + per-frame vars: "texture", "additive", "thick", "tex_ang", "tex_zoom"
            -fixed valid characters for filenames when importing/exporting custom shapes/waves;
                also, it now gives error messages on error in import/export.
            -cranked max. meshsize up to 96x72
            -Krash, Rova: now the 'q' variables, as modified by the preset per-frame equations, are again
                readable by the custom waves + custom shapes.  Sorry about that.  Should be the end of the
                'q' confusion.
            -added 'meshx' and 'meshy' [read-only] variables to the preset init, per-frame, and per-pixel
                equations (...and inc'd the size of the global variable pool by 2).
            -removed t1-t8 vars for Custom Shapes; they were unnecessary (since there's no per-point code there).
            -protected custom waves from trying to draw when # of sample minus the separation is < 2
                (or 1 if drawing with dots)
            -fixed some [minor] preset-blending bugs in the custom wave code
            -created a visual map for the flow of values for the q1-q8 and t1-t8 variables:
                q_and_t_vars.gif (or something).
            -fixed clipping of onscreen text in low-video-memory situations.  Now, if there isn't enough
                video memory to create an offscreen texture that is at least 75% of the size of the
                screen (or to create at least a 256x256 one), it won't bother using one, and will instead draw text directly to the screen.
                Otherwise, if the texture is from 75%-99% of the screen size, text will now at least
                appear in the correct position on the screen so that it will be visible; this will mean
                that the right- and bottom-aligned text will no longer be fully right/bottom-aligned
                to the edge of the screen.
            -fixed blurry text
            -VJ mode is partially restored; the rest will come with beta 7 or the final release.  At the time of beta 6, VJ mode still has some glitches in it, but I'm working on them.  Most notably, it doesn't resize the text image when you resize the window; that's next on my list.
        <AFTER BETA 6:>
            -now sprites can burn-in on any frame, not just on the last frame.
                set 'burn' to one (in the sprite code) on any frame to make it burn in.
                this will break some older sprites, but it's super easy to fix, and
                I think it's worth it. =)  thanks to papaw00dy for the suggestion!
            -fixed the asymptotic-value bug with custom waves using spectral data & having < 512 samples (thanks to telek's example!)
            -fixed motion vectors' reversed Y positioning.
            -fixed truncation ("...") of long custom messages
            -fixed that pesky bug w/the last line of code on a page
            -fixed the x-positioning of custom waves & shapes.  Before, if you were
                saving some coordinates from the preset's per-frame equations (say in q1 and q2)
                and then you set "x = q1; y = q2;" in a custom shape's per-frame code
                (or in a custom wave's per-point code), the x position wouldn't really be
                in the right place, because of aspect ratio multiplications.  Before, you had
                to actually write "x = (q1-0.5)*0.75 + 0.5; y = q2;" to get it to line up;
                now it's fixed, though, and you can just write "x = q1; y = q2;".
            -fixed some bugs where the plugin start up, in windowed mode, on the wrong window
                (and hence run super slow).
            -fixed some bugs w/a munged window frame when the "integrate with winamp" option
                was checked.
        <AFTER BETA 7:>
            -preset ratings are no longer read in all at once; instead, they are scanned in
                1 per frame until they're all in.  This fixes the long pauses when you switch
                to a directory that has many hundreds of presets.  If you want to switch
                back to the old way (read them all in at once), there is an option for it
                in the config panel.
            -cranked max. mesh size up to 128x96
            -fixed bug in custom shape per-frame code, where t1-t8 vars were not
                resetting, at the beginning of each frame, to the values that they had
                @ the end of the custom shape init code's execution.
            -
            -
            -


        beta 2 thread: http://forums.winamp.com/showthread.php?threadid=142635
        beta 3 thread: http://forums.winamp.com/showthread.php?threadid=142760
        beta 4 thread: http://forums.winamp.com/showthread.php?threadid=143500
        beta 6 thread: http://forums.winamp.com/showthread.php?threadid=143974
        (+read about beat det: http://forums.winamp.com/showthread.php?threadid=102205)

@       -code editing: when cursor is on 1st posn. in line, wrong line is highlighted!?
        -requests:
            -random sprites (...they can just write a prog for this, tho)
            -Text-entry mode.
                -Like your favorite online game, hit T or something to enter 'text entry' mode. Type a message, then either hit ESC to clear and cancel text-entry mode, or ENTER to display the text on top of the vis. Easier for custom messages than editing the INI file (and probably stopping or minimizing milkdrop to do it) and reloading.
                -OR SKIP IT; EASY TO JUST EDIT, RELOAD, AND HIT 00.
            -add 'AA' parameter to custom message text file?
        -when mem is low, fonts get kicked out -> white boxes
            -probably happening b/c ID3DXFont can't create a
             temp surface to use to draw text, since all the
             video memory is gobbled up.
*       -add to installer: q_and_t_vars.gif
*       -presets:
            1. pick final set
                    a. OK-do a pass weeding out slow presets (crank mesh size up)
                    b. OK-do 2nd pass; rate them & delete crappies
                    c. OK-merge w/set from 1.03; check for dupes; delete more suckies
            2. OK-check for cpu-guzzlers
            3. OK-check for big ones (>= 8kb)
            4. check for ultra-spastic-when-audio-quiet ones
            5. update all ratings
            6. zip 'em up for safekeeping
*       -docs:
                -link to milkdrop.co.uk
                -preset authoring:
                    -there are 11 variable pools:
                        preset:
                            a) preset init & per-frame code
                            b) preset per-pixel code
                        custom wave 1:
                            c) init & per-frame code
                            d) per-point code
                        custom wave 2:
                            e) init & per-frame code
                            f) per-point code
                        custom wave 3:
                            g) init & per-frame code
                            h) per-point code
                        i) custom shape 1: init & per-frame code
                        j) custom shape 2: init & per-frame code
                        k) custom shape 3: init & per-frame code

                    -all of these have predefined variables, the values of many of which
                        trickle down from init code, to per-frame code, to per-pixel code,
                        when the same variable is defined for each of these.
                    -however, variables that you define ("my_var = 5;") do NOT trickle down.
                        To allow you to pass custom values from, say, your per-frame code
                        to your per-pixel code, the variables q1 through q8 were created.
                        For custom waves and custom shapes, t1 through t8 work similarly.
                    -q1-q8:
                        -purpose: to allow [custom] values to carry from {the preset init
                            and/or per-frame equations}, TO: {the per-pixel equations},
                            {custom waves}, and {custom shapes}.
                        -are first set in preset init code.
                        -are reset, at the beginning of each frame, to the values that
                            they had at the end of the preset init code.
                        -can be modified in per-frame code...
                            -changes WILL be passed on to the per-pixel code
                            -changes WILL pass on to the q1-q8 vars in the custom waves
                                & custom shapes code
                            -changes will NOT pass on to the next frame, though;
                                use your own (custom) variables for that.
                        -can be modified in per-pixel code...
                            -changes will pass on to the next *pixel*, but no further
                            -changes will NOT pass on to the q1-q8 vars in the custom
                                waves or custom shapes code.
                            -changes will NOT pass on to the next frame, after the
                                last pixel, though.
                        -CUSTOM SHAPES: q1-q8...
                            -are readable in both the custom shape init & per-frame code
                            -start with the same values as q1-q8 had at the end of the *preset*
                                per-frame code, this frame
                            -can be modified in the init code, but only for a one-time
                                pass-on to the per-frame code.  For all subsequent frames
                                (after the first), the per-frame code will get the q1-q8
                                values as described above.
                            -can be modified in the custom shape per-frame code, but only
                                as temporary variables; the changes will not pass on anywhere.
                        -CUSTOM WAVES: q1-q8...
                            -are readable in the custom wave init, per-frame, and per-point code
                            -start with the same values as q1-q8 had at the end of the *preset*
                                per-frame code, this frame
                            -can be modified in the init code, but only for a one-time
                                pass-on to the per-frame code.  For all subsequent frames
                                (after the first), the per-frame code will get the q1-q8
                                values as described above.
                            -can be modified in the custom wave per-frame code; changes will
                                pass on to the per-point code, but that's it.
                            -can be modified in the per-point code, and the modified values
                                will pass on from point to point, but won't carry beyond that.
                        -CUSTOM WAVES: t1-t8...
                            -allow you to generate & save values in the custom wave init code,
                                that can pass on to the per-frame and, more sigificantly,
                                per-point code (since it's in a different variable pool).
                            -...



                        !-whatever the values of q1-q8 were at the end of the per-frame and per-pixel
                            code, these are copied to the q1-q8 variables in the custom wave & custom
                            shape code, for that frame.  However, those values are separate.
                            For example, if you modify q1-q8 in the custom wave #1 code, those changes
                            will not be visible anywhere else; if you modify q1-q8 in the custom shape
                            #2 code, same thing.  However, if you modify q1-q8 in the per-frame custom
                            wave code, those modified values WILL be visible to the per-point custom
                            wave code, and can be modified within it; but after the last point,
                            the values q1-q8 will be discarded; on the next frame, in custom wave #1
                            per-frame code, the values will be freshly copied from the values of the
                            main q1-q8 after the preset's per-frame and per-point code have both been
                            executed.
                    -monitor:
                        -can be read/written in preset init code & preset per-frame code.
                        -not accessible from per-pixel code.
                        -if you write it on one frame, then that value persists to the next frame.
                    -t1-t8:
                        -
                        -
                        -
                -regular docs:
                    -put in the stuff recommended by VMS (vidcap, etc.)
                    -add to troubleshooting:
                        1) desktop mode icons not appearing?  or
                           onscreen text looking like colored boxes?
                             -> try freeing up some video memory.  lower your res; drop to 16 bit;
                                etc.  TURN OFF AUTO SONGTITLES.
                        1) slow desktop/fullscr mode?  -> try disabling auto songtitles + desktop icons.
                            also try reducing texsize to 256x256, since that eats memory that the text surface could claim.
                        2)
                        3)
        *   -presets:
                -add new
                -fix 3d presets (bring gammas back down to ~1.0)
                -check old ones, make sure they're ok
                    -"Rovastar - Bytes"
                    -check wave_y
        *   -document custom waves & shapes
        *   -slow text is mostly fixed... =(
                -desktop icons + playlist both have begin/end around them now, but in desktop mode,
                 if you bring up playlist or Load menu, fps drops in half; press Esc, and fps doesn't go back up.
            -
            -
            -
        -DONE / v1.04:
            -updated to VMS 1.05
                -[list benefits...]
                -
                -
            -3d mode:
                a) SWAPPED DEFAULT L/R LENS COLORS!  All images on the web are left=red, right=blue!
                b) fixed image display when viewing a 3D preset in a non-4:3 aspect ratio window
                c) gamma now works for 3d presets!  (note: you might have to update your old presets.
                        if they were 3D presets, the gamma was ignored and 1.0 was used; now,
                        if gamma was >1.0 in the old preset, it will probably appear extremely bright.)
                d) added SHIFT+F9 and CTRL+C9 to inc/dec stereo separation
                e) added default stereo separation to config panel
            -cranked up the max. mesh size (was 48x36, now 64x48) and the default mesh size
                (was 24x18, now 32x24)
            -fixed aspect ratio for final display
            -auto-texsize is now computed slightly differently; for vertically or horizontally-stretched
                windows, the texsize is now biased more toward the larger dimension (vs. just the
                average).
            -added anisotropic filtering (for machines that support it)
            -fixed bug where the values of many variables in the preset init code were not set prior
                to execution of the init code (e.g. time, bass, etc. were all broken!)
            -added various preset blend effects.  In addition to the old uniform fade, there is
                now a directional wipe, radial wipe, and plasma fade.
            -FIXED SLOW TEXT for DX8 (at least, on the geforce 4).
                Not using DT_RIGHT or DT_BOTTOM was the key.


        -why does text kill it in desktop mode?
        -text is SLOOW
        -to do: add (and use) song title font + tooltip font
        -re-test: menus, text, boxes, etc.
        -re-test: TIME
        -testing:
            -make sure sound works perfectly.  (had to repro old pre-vms sound analysis!)
            -autogamma: currently assumes/requires that GetFrame() resets to 0 on a mode change
                (i.e. windowed -> fullscreen)... is that the case?
            -restore motion vectors
            -
            -
        -restore lost surfaces
        -test bRedraw flag (desktop mode/paused)
        -search for //? in milkdropfs.cpp and fix things

        problem: no good soln for VJ mode
        problem: D3DX won't give you solid background for your text.
            soln: (for later-) create wrapper fn that draws w/solid bkg.

        SOLN?: use D3DX to draw all text (plugin.cpp stuff AND playlist);
        then, for VJ mode, create a 2nd DxContext
        w/its own window + windowproc + fonts.  (YUCK)
    1) config panel: test, and add WM_HELP's (copy from tooltips)
    2) keyboard input: test; and...
        -need to reset UI_MODE when playlist is turned on, and
        -need to reset m_show_playlist when UI_MODE is changed.  (?)
        -(otherwise they can both show @ same time and will fight
            for keys and draw over each other)
    3) comment out most of D3D stuff in milkdropfs.cpp, and then
        get it running w/o any milkdrop, but with text, etc.
    4) sound

  Issues / To Do Later
  --------------------
    1) sprites: color keying stuff probably won't work any more...
    2) scroll lock: pull code from Monkey
    3) m_nGridY should not always be m_nGridX*3/4!
    4) get solid backgrounds for menus, waitstring code, etc.
        (make a wrapper function!)

    99) at end: update help screen

  Things that will be different
  -----------------------------
    1) font sizes are no longer relative to size of window; they are absolute.
    2) 'don't clear screen at startup' option is gone
    3) 'always on top' option is gone
    4) text will not be black-on-white when an inverted-color preset is showing

                -VJ mode:
                    -notes
                        1. (remember window size/pos, and save it from session to session?  nah.)
                        2. (kiv: scroll lock)
                        3. (VJ window + desktop mode:)
                                -ok w/o VJ mode
                                -w/VJ mode, regardless of 'fix slow text' option, probs w/focus;
                                    click on vj window, and plugin window flashes to top of Z order!
                                -goes away if you comment out 1st call to PushWindowToJustBeforeDesktop()...
                                -when you disable PushWindowToJustBeforeDesktop:
                                    -..and click on EITHER window, milkdrop jumps in front of the taskbar.
                                    -..and click on a non-MD window, nothing happens.
                                d-FIXED somehow, magically, while fixing bugs w/true fullscreen mode!
                        4. (VJ window + true fullscreen mode:)
                                d-make sure VJ window gets placed on the right monitor, at startup,
                                    and respects taskbar posn.
                                d-bug - start in windowed mode, then dbl-clk to go [true] fullscreen
                                    on 2nd monitor, all with VJ mode on, and it excepts somewhere
                                    in m_text.DrawNow() in a call to DrawPrimitive()!
                                    FIXED - had to check m_vjd3d8_device->TestCooperativeLevel
                                    each frame, and destroy/reinit if device needed reset.
                                d-can't resize VJ window when grfx window is running true fullscreen!
                                    -FIXED, by dropping the Sleep(30)/return when m_lost_focus
                                        was true, and by not consuming WM_NCACTIVATE in true fullscreen
                                        mode when m_hTextWnd was present, since DX8 doesn't do its
                                        auto-minimize thing in that case.



========================================================================================================
SPOUT :

  Credit to psilocin@openmailbox.org for the original idea to convert MilkDrop for Spout output

  22.10.14 - changed from Ctrl-Z on and off to default Spout output when the plugin starts
         and Ctrl-Z to disable and enable while it is running. Otherwise Spout has to be re-enabled
         every time another track is selected.
  30.10.14 - changed from Glut to pixelformat and OpenGL context creation
  31.10.14 - changed initialization section to renderframe to ensure correct frame size
       - added Ctrl-D user selection of DirectX mode
       - flag bUseDX11 to select either DirectX 9 or DirectX 11
       - saved DX mode flag in configuration file
  05.11.14 - Included Spout options in the Visualization configuration control panel
        Options -> Visualizatiosn -> Configure Plugin
        MORE SETTINGS tab
          Enable Spout output
          Enable Spout DirectX 11 mode
        Settings are saved with OK
       - retained Ctrl-Z for spout on / off while the Visualizer is running
       - included Ctrl-D to change from DirectX 9 to DirectX 11
         (this might be removed in a future release if it gives trouble)
         The selected settings are saved when the Visualizer is stopped.
  25.04.15 - Changed Spout SDK from graphics auto detection to set DirectX mode to optional installer
       - Recompile for dual DX option installer
  17.06.15 - User observation that custom messages do not work.
         This is isolated to "RenderStringToTitleTexture" and seems to be related to
         generating the fonts from GDI to DX9. Not sure of the reason. Could be DX9 libraries.
         As a a workaround, custom message rendering is replaced with the same as used for
         title animation which works OK. The limitation is that this gives a fixed font,
         but the colour should come out the same as in the custom message setup file.
  07.07.15 - Recompile for 2.004 release
  15.09.15 - Recompile for 2.005 release - revised memoryshare SDK
  08.11.15 - removed directX9/directX11 option for 2.005
       - OpenSender and milkdropfs.cpp - removed XRGB format option and always send as ARGB
  12.11.18 - Removed DX11 (bUseDX11) option from plugin - test for user DX9 selection instead
  02.12.18 - Rebuild VS2017 /MT with VS2010 100 toolset - Spout 2.007
         (VS2017 140 toolset does not work)


  03.12.18 - Started modifications to the BeatDrop project (not back-compatible)
         BeatDrop name, versioning and authoring by Maxim Volskiy retained
         Use the VJ console for help and text output.
         Output resolution is 1920x1080 at start
         Resolution can subsequently be changed by resizing the render window
         The render window can be hidden with the F12 key
         and the VJ console can be minimized when not being used
         without affecting Spout output.
  04.12.18 - Monitor dpi awareness for scaled displays
         Reset help or menu text when activating either of them
         Disable minimize and maximize
         Use SpoutLibrary instead of Spout SDK source files
         Cleanup
         Rebuild VS2017 /MT with Visual Studio 2017 toolset (v141)
    05.12.18 : Create GitHub fork and update. Publish release 1001.
               Bring up the VJ console for F1 help if it has been minimised
         PluginShell.cpp - remove maximize from VJ window
  21.12.18 : Update SpoutLibrary - Version 2.007
  03.01.19 : Rebuild SpoutLibrary
         Rebuild VS2017 /MT with Visual Studio 2017 toolset (v141)
  16.01.19   TODO : Bug when resizing - re-creates the sender with a new name.
  29.04.19   Noted : setting  Monitor dpi awareness uusing Manifest tool compiler option
         results in blurry text for the console window. Retained SetProcessDpiAwareness
         Noted : warning 'Zc:forScope-' - deprecated for VS2017
         For future Visual Studio compilers, might need changes throughout for loops
  31.05.19   Rebuild with revised 2.007 SpoutLibrary
         VS2017 /MT with Visual Studio 2017 toolset (v141), Windows SDK 10.0.17763.0

  03.10.19   Change from DX9 to DX9EX

    Credit to Patrick Pomerleau of Nest Immersion (http://nestimmersion.ca/)

         Search on "DX9EX" for the changes.
          Milkdrop2PcmVisualizer.cpp
          milkdropfs.cpp
          pluginshell.cpp
          pluginshell.h
          dxcontext.cpp
          dxcontext.h

         - Set application to use Spout DX9 mode
         - Add function : IDirect3D9Ex* EngineShell::GetDX9object()
         - OpenSender - set Spout to use the application DX9EX object and device
         - Use new surface copy function WriteDX9surface rather than CPU pixel copy.
  05.10.19   - Lock resolution to 1920x1080
         - Retain VJ console for standalone
         - Remove close button from VJ console
         - Add : F8 - copy Winamp Milkdrop generated config file
  08.10.19   - Change window resolution to 1280x720
           Retain output resolution 1920x1080
         - Modify WriteDX9surface and SetDX9device - see milkdropfs and plugin.cpp
           Modify corresponding Spout SDK and SpoutLibrary functions
         - Rebuild VS2017 /MT Win32 with modified 2.007 SpoutLibrary
           Search on "// SPOUT DX9EX" for changes
  29.10.19   - Milkdrop2PcmVisualizer.cpp
             Change dpi awareness to use SetProcessDpiAwarenessContext
             for Windows 7 compatibility
           Remove #include <ShellScalingApi.h> and #pragma comment(lib, "shcore.lib")

    15.05.23   - Change from SpoutLibrary to SpoutDX9 support class
                 Changed files :
                   vis_milk2\plugin.cpp
                   vis_milk2\plugin.h
                   vis_milk2\Milkdrop2PcmVisualizer.cpp
                   vis_milk2\milkdropfs.cpp
                   vis_milk2\pluginshell.h
                 nseel2/nseel-compiler.c
                   remove "floor" intrinsic re-definition
                 Add spoutDX9 folder and files
                 Add spoutDX9 filter to project
                 Project > Properties > Include Directories
                 Specifically include $(WindowsSDK_IncludePath) before $(DXSDK_DIR)Include
                 to prevent macro re-definitions with Windows Kits\10 together with
                 Microsoft DirectX SDK (June 2010)
                 Rebuild Visual Studio 2022 Release/Win32
    25.09.23     MyWindowProc - WM_KEYDOWN default - fall through
    26.09.23     SendDX9surface - add update flag to allow a fixed sender size if false
                 SpoutDX9.cpp/CheckDX9sender - CreateSharedDX9Texture to new width and height for size change


*/

#include "engine.h"
#include "tool_window.h"
#include "video_capture.h"
#include "engine_helpers.h"
#include "utility.h"
#include "support.h"
#include "resource.h"
#include "defines.h"
#include "shell_defines.h"
#include "wasabi.h"
#include <assert.h>
#include <locale.h>
#include <process.h>  // for beginthread, etc.
#include <shellapi.h>
#include <shobjidl.h>  // IFileDialog for folder picker (Ctrl+L)
#include <strsafe.h>
#include <Windows.h>
#include "AutoCharFn.h"
#include "embedded_shaders.h"
#include <sstream>

#include <dwmapi.h>  // Link with Dwmapi.lib
#pragma comment(lib, "dwmapi.lib")
#define FRAND ((rand() % 7381)/7380.0f)
#define clamp(value, min, max) ((value) < (min) ? (min) : ((value) > (max) ? (max) : (value)))

namespace mdrop {

int ToggleFPSNumPressed = 7;			// Default is Unlimited FPS.
int HardcutMode = 0;
float timetick = 0;
float timetick2 = 0;
float TimeToAutoLockPreset = 0;
int beatcount;
bool TranspaMode = false;
int NumTotalPresetsLoaded = 0;
bool AutoLockedPreset = false;
std::chrono::steady_clock::time_point LastSentMDropDX12Message{};

//For Sample Rate auto-detection
#include <windows.h>
#include <mmdeviceapi.h>
#include <propsys.h>
#include <functiondiscoverykeys_devpkey.h>
#include <cstdint>
#include <commctrl.h>  // Trackbar, tab, and list-view controls
#include <commdlg.h>   // ChooseFont, ChooseColor common dialogs
#include <uxtheme.h>   // SetWindowTheme for dark mode controls
#include <set>
#pragma comment(lib, "comctl32.lib")
#pragma comment(lib, "uxtheme.lib")

#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "propsys.lib")
//

} // namespace mdrop (temporarily close for C-linkage stubs)

// NSEEL_VM_resetvars: compatibility wrapper (not in WDL ns-eel2 API)
void NSEEL_VM_resetvars(NSEEL_VMCTX ctx) {
  NSEEL_VM_freeRAM(ctx);
  NSEEL_VM_remove_all_nonreg_vars(ctx);
}

namespace mdrop {

// note: these must match layouts in support.h!!
D3DVERTEXELEMENT9 g_MyVertDecl[] =
{
    { 0, 0,  D3DDECLTYPE_FLOAT3,   D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_POSITION, 0 },
    { 0, 12, D3DDECLTYPE_D3DCOLOR, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_COLOR,    0 },
    { 0, 16, D3DDECLTYPE_FLOAT4,   D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_TEXCOORD, 0 },
    { 0, 32, D3DDECLTYPE_FLOAT2,   D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_TEXCOORD, 1 },
    D3DDECL_END()
};
D3DVERTEXELEMENT9 g_WfVertDecl[] =
{
    { 0, 0,  D3DDECLTYPE_FLOAT3,   D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_POSITION, 0 },
    { 0, 12, D3DDECLTYPE_D3DCOLOR, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_COLOR,    0 },
    D3DDECL_END()
};
D3DVERTEXELEMENT9 g_SpriteVertDecl[] =
{
  // matches D3DFVF_XYZ | D3DFVF_DIFFUSE | D3DFVF_TEX1
  { 0, 0,  D3DDECLTYPE_FLOAT3,   D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_POSITION, 0 },
  { 0, 12, D3DDECLTYPE_D3DCOLOR, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_COLOR,    0 },
  { 0, 16, D3DDECLTYPE_FLOAT2,   D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_TEXCOORD, 0 },
  D3DDECL_END()
};

//extern CSoundData*   pg_sound;	// declared in main.cpp
extern Engine g_engine;		// declared in App.cpp
extern CShaderParamsList global_CShaderParams_master_list; // defined in engine_shaders.cpp

//----------------------------------------------------------------------
// Settings types, enums, IDC_MW_*, WM_MW_* now in engine_helpers.h

SettingDesc g_settingsDesc[] = {
  { L"Preset Directory",       ST_PATH,     SET_PRESET_DIR,       0, 0, 0,       L"Settings",  L"szPresetDir" },
  { L"Audio Device",           ST_READONLY, SET_AUDIO_DEVICE,     0, 0, 0,       NULL,         NULL },
  { L"Audio Sensitivity",      ST_FLOAT,    SET_AUDIO_SENSITIVITY, -1, 256, 0.5f, L"Milkwave",  L"AudioSensitivity" },
  { L"Blend Time",             ST_FLOAT,    SET_BLEND_TIME,       0.1f, 10, 0.1f, L"Settings", L"fBlendTimeAuto" },
  { L"Time Between Presets",   ST_FLOAT,    SET_TIME_BETWEEN,     1, 300, 5,     L"Settings",  L"fTimeBetweenPresets" },
  { L"Hard Cuts Disabled",     ST_BOOL,     SET_HARD_CUTS,        0, 0, 0,       L"Settings",  L"bHardCutsDisabled" },
  { L"Preset Lock on Startup", ST_BOOL,     SET_PRESET_LOCK,      0, 0, 0,       L"Settings",  L"bPresetLockOnAtStartup" },
  { L"Sequential Order",       ST_BOOL,     SET_SEQ_ORDER,        0, 0, 0,       L"Settings",  L"bSequentialPresetOrder" },
  { L"Song Title Animations",  ST_BOOL,     SET_SONG_TITLE_ANIMS, 0, 0, 0,       L"Settings",  L"bSongTitleAnims" },
  { L"Change Preset w/ Song",  ST_BOOL,     SET_CHANGE_WITH_SONG, 0, 0, 0,       L"Milkwave",  L"ChangePresetWithSong" },
  { L"Show FPS",               ST_BOOL,     SET_SHOW_FPS,         0, 0, 0,       L"Settings",  L"bShowFPS" },
  { L"Always On Top",          ST_BOOL,     SET_ALWAYS_ON_TOP,    0, 0, 0,       L"Milkwave",  L"WindowAlwaysOnTop" },
  { L"Borderless Window",      ST_BOOL,     SET_BORDERLESS,       0, 0, 0,       L"Milkwave",  L"WindowBorderless" },
  { L"Spout Output",           ST_BOOL,     SET_SPOUT,            0, 0, 0,       L"Settings",  L"bSpoutOut" },
  { L"Messages/Sprites",       ST_INT,      SET_SPRITES_MESSAGES, 0, 3, 1,       L"Settings",  L"nSpriteMessagesMode" },
};

} // namespace mdrop (close for cross-namespace externs)
// from support.cpp (global namespace):
extern bool g_bDebugOutput;
extern bool g_bDumpFileCleared;
namespace mdrop {

// for __UpdatePresetList:
volatile HANDLE g_hThread;  // only r/w from our MAIN thread
volatile bool g_bThreadAlive; // set true by MAIN thread, and set false upon exit from 2nd thread.
volatile int  g_bThreadShouldQuit;  // set by MAIN thread to flag 2nd thread that it wants it to exit.
CRITICAL_SECTION g_cs;

// IsAlphabetChar, IsAlphanumericChar, IsNumericChar now in engine_helpers.h

const unsigned char LC2UC[256] = {
  0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16,
  17,18,19,20,21,22,23,24,25,26,27,28,29,30,31,255,
  33,34,35,36,37,38,39,40,41,42,43,44,45,46,47,48,
  49,50,51,52,53,54,55,56,57,58,59,60,61,62,63,64,
  97,98,99,100,101,102,103,104,105,106,107,108,109,110,111,112,
  113,114,115,116,117,118,119,120,121,122,91,92,93,94,95,96,
  97,98,99,100,101,102,103,104,105,106,107,108,109,110,111,112,
  113,114,115,116,117,118,119,120,121,122,123,124,125,126,127,128,
  129,130,131,132,133,134,135,136,137,138,139,140,141,142,143,144,
  145,146,147,148,149,150,151,152,153,154,155,156,157,158,159,160,
  161,162,163,164,165,166,167,168,169,170,171,172,173,174,175,176,
  177,178,179,180,181,182,183,184,185,186,187,188,189,190,191,192,
  193,194,195,196,197,198,199,200,201,202,203,204,205,206,207,208,
  209,210,211,212,213,214,215,216,217,218,219,220,221,222,223,224,
  225,226,227,228,229,230,231,232,233,234,235,236,237,238,239,240,
  241,242,243,244,245,246,247,248,249,250,251,252,253,254,255,
};

// Helper: look up a filename in the embedded shader table.
// Returns the embedded source string, or nullptr if not found.
static const char* FindEmbeddedShader(const wchar_t* szBaseFilename) {
  for (int i = 0; i < k_num_embedded_shaders; i++) {
    if (_wcsicmp(szBaseFilename, k_embedded_shaders[i].filename) == 0)
      return k_embedded_shaders[i].data;
  }
  return nullptr;
}

// Helper: copy embedded shader source into szDestText with optional LF conversion.
static void CopyEmbeddedToBuffer(const char* src, char* szDestText, int nMaxBytes, bool bConvertLFsToSpecialChar) {
  int len = 0;
  char prev_ch = 0;
  while (*src && len < nMaxBytes - 4) {
    char orig_ch = *src++;
    char ch = orig_ch;
    bool bSkipChar = false;
    if (bConvertLFsToSpecialChar) {
      if (ch == 10) {
        if (prev_ch == 13)
          bSkipChar = true;
        else
          ch = LINEFEED_CONTROL_CHAR;
      }
      else if (ch == 13)
        ch = LINEFEED_CONTROL_CHAR;
    }
    if (!bSkipChar)
      szDestText[len++] = ch;
    prev_ch = orig_ch;
  }
  szDestText[len] = 0;
  szDestText[len++] = ' ';  // trailing whitespace (matches original behavior)
}

bool ReadFileToString(const wchar_t* szBaseFilename, char* szDestText, int nMaxBytes, bool bConvertLFsToSpecialChar) {
  wchar_t szFile[MAX_PATH];
  swprintf(szFile, L"%s%s", g_engine.m_szMilkdrop2Path, szBaseFilename);

  // Embedded shaders are the primary source. Disk .fx files serve as user overrides.
  const char* embedded = FindEmbeddedShader(szBaseFilename);

  // If a disk file exists, use it (user override). Otherwise use embedded.
  FILE* f = _wfopen(szFile, L"rb");
  if (f) {
    if (embedded) {
      wchar_t dbg[512];
      swprintf(dbg, L"Using disk override for: %s", szBaseFilename);
      g_engine.dumpmsg(dbg);
    }

    // Read from disk. Replace { 13; 13+10; 10 } with LINEFEED_CONTROL_CHAR if requested.
    int len = 0;
    int x;
    char prev_ch = 0;
    while ((x = fgetc(f)) >= 0 && len < nMaxBytes - 4) {
      char orig_ch = (char)x;
      char ch = orig_ch;
      bool bSkipChar = false;
      if (bConvertLFsToSpecialChar) {
        if (ch == 10) {
          if (prev_ch == 13)
            bSkipChar = true;
          else
            ch = LINEFEED_CONTROL_CHAR;
        }
        else if (ch == 13)
          ch = LINEFEED_CONTROL_CHAR;
      }
      if (!bSkipChar)
        szDestText[len++] = ch;
      prev_ch = orig_ch;
    }
    szDestText[len] = 0;
    szDestText[len++] = ' ';   // make sure there is some whitespace after
    fclose(f);
    return true;
  }

  // No disk file — use embedded if available
  if (embedded) {
    CopyEmbeddedToBuffer(embedded, szDestText, nMaxBytes, bConvertLFsToSpecialChar);
    return true;
  }

  // Neither embedded nor on disk
  wchar_t buf[1024], title[64];
  swprintf(buf, wasabiApiLangString(IDS_UNABLE_TO_READ_DATA_FILE_X), szFile);
  g_engine.dumpmsg(buf);
  MessageBoxW(NULL, buf, wasabiApiLangString(IDS_MILKDROP_ERROR, title, 64), MB_OK | MB_SETFOREGROUND | MB_TOPMOST);
  return false;
}

// these callback functions are called by menu.cpp whenever the user finishes editing an eval_ expression.
void OnUserEditedPerFrame(LPARAM param1, LPARAM param2) {
  g_engine.m_pState->RecompileExpressions(RECOMPILE_PRESET_CODE, 0);
}

void OnUserEditedPerPixel(LPARAM param1, LPARAM param2) {
  g_engine.m_pState->RecompileExpressions(RECOMPILE_PRESET_CODE, 0);
}

void OnUserEditedPresetInit(LPARAM param1, LPARAM param2) {
  g_engine.m_pState->RecompileExpressions(RECOMPILE_PRESET_CODE, 1);
}

void OnUserEditedWavecode(LPARAM param1, LPARAM param2) {
  g_engine.m_pState->RecompileExpressions(RECOMPILE_WAVE_CODE, 0);
}

void OnUserEditedWavecodeInit(LPARAM param1, LPARAM param2) {
  g_engine.m_pState->RecompileExpressions(RECOMPILE_WAVE_CODE, 1);
}

void OnUserEditedShapecode(LPARAM param1, LPARAM param2) {
  g_engine.m_pState->RecompileExpressions(RECOMPILE_SHAPE_CODE, 0);
}

void OnUserEditedShapecodeInit(LPARAM param1, LPARAM param2) {
  g_engine.m_pState->RecompileExpressions(RECOMPILE_SHAPE_CODE, 1);
}

void OnUserEditedWarpShaders(LPARAM param1, LPARAM param2) {
  g_engine.m_bNeedRescanTexturesDir = true;
  g_engine.ClearErrors(ERR_PRESET);
  if (g_engine.m_nMaxPSVersion == 0)
    return;
  if (!g_engine.RecompilePShader(g_engine.m_pState->m_szWarpShadersText, &g_engine.m_shaders.warp, SHADER_WARP, false, g_engine.m_pState->m_nWarpPSVersion, false)) {
    // switch to fallback
    if (g_engine.m_fallbackShaders_ps.warp.ptr) g_engine.m_fallbackShaders_ps.warp.ptr->AddRef();
    if (g_engine.m_fallbackShaders_ps.warp.CT) g_engine.m_fallbackShaders_ps.warp.CT->AddRef();
    if (g_engine.m_fallbackShaders_ps.warp.bytecodeBlob) g_engine.m_fallbackShaders_ps.warp.bytecodeBlob->AddRef();
    g_engine.m_shaders.warp = g_engine.m_fallbackShaders_ps.warp;
  }
  g_engine.CreateDX12PresetPSOs();
}

void OnUserEditedCompShaders(LPARAM param1, LPARAM param2) {
  g_engine.m_bNeedRescanTexturesDir = true;
  g_engine.ClearErrors(ERR_PRESET);
  if (g_engine.m_nMaxPSVersion == 0)
    return;
  if (!g_engine.RecompilePShader(g_engine.m_pState->m_szCompShadersText, &g_engine.m_shaders.comp, SHADER_COMP, false, g_engine.m_pState->m_nCompPSVersion, false)) {
    // switch to fallback
    if (g_engine.m_fallbackShaders_ps.comp.ptr) g_engine.m_fallbackShaders_ps.comp.ptr->AddRef();
    if (g_engine.m_fallbackShaders_ps.comp.CT) g_engine.m_fallbackShaders_ps.comp.CT->AddRef();
    if (g_engine.m_fallbackShaders_ps.comp.bytecodeBlob) g_engine.m_fallbackShaders_ps.comp.bytecodeBlob->AddRef();
    g_engine.m_shaders.comp = g_engine.m_fallbackShaders_ps.comp;
  }
  g_engine.CreateDX12PresetPSOs();
}

// Modify the help screen text here.
// Watch the # of lines, though; if there are too many, they will get cut off;
//   and watch the length of the lines, since there is no wordwrap.
// A good guideline: your entire help screen should be visible when fullscreen
//   @ 640x480 and using the default help screen font.
wchar_t* g_szHelp = 0;
wchar_t* g_szHelp_Page2 = 0;
int g_szHelp_W = 0;

// this is for integrating modern skins (with their Random button)
// and having it match our Scroll Lock (preset lock) state...
#define IPC_CB_VISRANDOM 628

//----------------------------------------------------------------------

void Engine::OverrideDefaults() {
  // Here, you have the option of overriding the "default defaults"
  //   for the stuff on tab 1 of the config panel, replacing them
  //   with custom defaults for your plugin.
  // To override any of the defaults, just uncomment the line
  //   and change the value.
  // DO NOT modify these values from any function but this one!

  // This example plugin only changes the default width/height
  //   for fullscreen mode; the "default defaults" are just
  //   640 x 480.
  // If your plugin is very dependent on smooth animation and you
  //   wanted it plugin to have the 'save cpu' option OFF by default,
  //   for example, you could set 'm_save_cpu' to 0 here.

  // m_start_fullscreen      = 0;       // 0 or 1
  // m_start_desktop         = 0;       // 0 or 1
  // m_fake_fullscreen_mode  = 0;       // 0 or 1
  //m_max_fps_fs            = 0;      // 1-120, or 0 for 'unlimited'
  //m_max_fps_dm            = 0;      // 1-120, or 0 for 'unlimited'
  //m_max_fps_w             = 0;      // 1-120, or 0 for 'unlimited'
  // m_show_press_f1_msg     = 1;       // 0 or 1
  m_allow_page_tearing_w = 0;       // 0 or 1
  // m_allow_page_tearing_fs = 0;       // 0 or 1
  // m_allow_page_tearing_dm = 1;       // 0 or 1
  // m_minimize_winamp       = 1;       // 0 or 1
  // m_desktop_textlabel_boxes = 1;     // 0 or 1
  // m_save_cpu              = 0;       // 0 or 1

  // lstrcpy(m_fontinfo[0].szFace, "Trebuchet MS"); // system font
  // m_fontinfo[0].nSize     = 18;
  // m_fontinfo[0].bBold     = 0;
  // m_fontinfo[0].bItalic   = 0;
  // lstrcpy(m_fontinfo[1].szFace, "Times New Roman"); // decorative font
  // m_fontinfo[1].nSize     = 24;
  // m_fontinfo[1].bBold     = 0;
  // m_fontinfo[1].bItalic   = 1;

  // Don't override default FS mode here; shell is now smart and sets it to match
  // the current desktop display mode, by default.

  //m_disp_mode_fs.Width    = 1024;             // normally 640
  //m_disp_mode_fs.Height   = 768;              // normally 480
  // use either D3DFMT_X8R8G8B8 or D3DFMT_R5G6B5.
  // The former will match to any 32-bit color format available,
  // and the latter will match to any 16-bit color available,
  // if that exact format can't be found.
//m_disp_mode_fs.Format   = D3DFMT_UNKNOWN; //<- this tells config panel & visualizer to use current display mode as a default!!   //D3DFMT_X8R8G8B8;
// m_disp_mode_fs.RefreshRate = 60;
}

//----------------------------------------------------------------------
// Preset directory auto-descend helpers — must be defined before MyPreInitialize
void Engine::MyPreInitialize() {
  // Initialize EVERY data member you've added to Engine here;
  //   these will be the default values.
  // If you want to initialize any of your variables with random values
  //   (using rand()), be sure to seed the random number generator first!
  // (If you want to change the default values for settings that are part of
  //   the plugin shell (framework), do so from OverrideDefaults() above.)


// =========================================================
// SPOUT initialisation
//

  // Error logging to AppData
  // EnableSpoutLogFile("SpoutBeatdrop.log");
  // For debugging 
  // EnableSpoutLog(); // Shows Spout logs on the console
    // OpenSpoutConsole(); // Empty console

  sprintf(WinampSenderName, "MDropDX12");
  bInitialized = false;
  bSpoutOut = true; // User on/off toggle
  bSpoutChanged = false; // set to write config on exit
  // DirectX 11 mode uses a format that is incompatible with DirectX 9 receivers
  // DirectX9 mode can fail with some drivers. Noted on Intel/NVIDIA laptop.
  g_Width = 0;
  g_Height = 0;
  g_hwnd = NULL;
  g_hdc = NULL;

  // seed the system's random number generator w/the current system time:
  //srand((unsigned)time(NULL));  -don't - let winamp do it

// Generate F1 help text dynamically from the hotkey binding table.
  // This replaces the old static .bin resource loading so the help
  // overlay always reflects the user's current key assignments.
  GenerateHelpText();
  g_szHelp = m_szHelpPage1;
  g_szHelp_Page2 = m_szHelpPage2;
  g_szHelp_W = 1;

  // CONFIG PANEL SETTINGS THAT WE'VE ADDED (TAB #2)
  m_bFirstRun = true;
  m_bInitialPresetSelected = false;
  m_fBlendTimeUser = 1.7f;
  m_fBlendTimeAuto = 2.7f;
  m_fTimeBetweenPresets = 60.0f;
  m_fTimeBetweenPresetsRand = 10.0f;
  m_bSequentialPresetOrder = true;
  m_bHardCutsDisabled = true;
  m_nInjectEffectMode = 0;
  m_fHardCutLoudnessThresh = 2.5f;
  m_fHardCutHalflife = 60.0f;
  //m_nWidth			= 1024;
  //m_nHeight			= 768;
  //m_nDispBits		= 16;
  m_nCanvasStretch = 100;
  m_nTexSizeX = -1;	// -1 means "auto"
  m_nTexSizeY = -1;	// -1 means "auto"
  m_nTexBitsPerCh = 8;
  m_nGridX = 64;//32;
  m_nGridY = 48;//24;

  // m_bShowPressF1ForHelp = true;
  //lstrcpy(m_szMonitorName, "[don't use multimon]");
  m_bShowMenuToolTips = true;	// NOTE: THIS IS CURRENTLY HARDWIRED TO TRUE - NO OPTION TO CHANGE
  m_n16BitGamma = 2;
  m_bAutoGamma = true;
  //m_nFpsLimit			= -1;
  m_bEnableRating = true;
  //m_bInstaScan            = false;
  m_bSongTitleAnims = false;
  m_nSpriteMessagesMode = 3;  // Messages & Sprites
  m_fSongTitleAnimDuration = 1.7f;
  m_fTimeBetweenRandomSongTitles = -1.0f;
  m_fTimeBetweenRandomCustomMsgs = -1.0f;
  m_nSongTitlesSpawned = 0;
  m_nCustMsgsSpawned = 0;
  m_nFramesSinceResize = 0;

  //m_bAlways3D		  	    = false;
  //m_fStereoSep            = 1.0f;
  //m_bAlwaysOnTop		= false;
  //m_bFixSlowText          = true;
  //m_bWarningsDisabled     = false;
  m_bWarningsDisabled2 = false;
  //m_bAnisotropicFiltering = true;
  m_bPresetLockOnAtStartup = true;
  m_bPreventScollLockHandling = false;
  m_nMaxPSVersion_ConfigPanel = -1;  // -1 = auto, 0 = disable shaders, 2 = ps_2_0, 3 = ps_3_0
  m_nMaxPSVersion_DX9 = -1;          // 0 = no shader support, 2 = ps_2_0, 3 = ps_3_0
  m_nMaxPSVersion = -1;              // this one will be the ~min of the other two.  0/2/3.
  m_nMaxImages = 2048;
  m_nMaxBytes = 2000000000;

#ifdef _DEBUG
  m_dwShaderFlags = D3DCOMPILE_DEBUG | D3DCOMPILE_ENABLE_BACKWARDS_COMPATIBILITY;
#else
  m_dwShaderFlags = D3DCOMPILE_ENABLE_BACKWARDS_COMPATIBILITY;
#endif
  //m_pFragmentLinker = NULL;
  //m_pCompiledFragments = NULL;
  m_pShaderCompileErrors = NULL;
  //m_vs_warp = NULL;
  //m_ps_warp = NULL;
  //m_vs_comp = NULL;
  //m_ps_comp = NULL;
  // Note: PShaderInfo/VShaderInfo constructors already initialize all members.
  // Do NOT use ZeroMemory on these — it corrupts std::vector internals in Debug mode.
  m_bWarpShaderLock = false;
  m_bCompShaderLock = false;
  m_bNeedRescanTexturesDir = true;

  // vertex declarations:
  m_pSpriteVertDecl = NULL;
  m_pWfVertDecl = NULL;
  m_pMyVertDecl = NULL;

  m_gdi_title_font_doublesize = NULL;
  m_d3dx_title_font_doublesize = NULL;

  // RUNTIME SETTINGS THAT WE'VE ADDED
  m_prev_time = GetTime() - 0.0333f; // note: this will be updated each frame, at bottom of MyRenderFn.
  m_bTexSizeWasAutoPow2 = false;
  m_bTexSizeWasAutoExact = false;
  //m_bPresetLockedByUser = false;  NOW SET IN DERIVED SETTINGS
  m_bPresetLockedByCode = false;
  m_fStartTime = 0.0f;
  m_fPresetStartTime = 0.0f;
  m_fNextPresetTime = -1.0f;	// negative value means no time set (...it will be auto-set on first call to UpdateTime)
  m_nLoadingPreset = 0;
  m_nPresetsLoadedTotal = 0;
  m_fSnapPoint = 0.5f;
  m_pState = &m_state_DO_NOT_USE[0];
  m_pOldState = &m_state_DO_NOT_USE[1];
  m_pNewState = &m_state_DO_NOT_USE[2];
  m_UI_mode = UI_REGULAR;
  m_bShowShaderHelp = false;

  m_nMashSlot = 0;    //0..MASH_SLOTS-1
  for (int mash = 0; mash < MASH_SLOTS; mash++)
    m_nLastMashChangeFrame[mash] = 0;

  //m_nTrackPlaying	= 0;
//m_nSongPosMS      = 0;
//m_nSongLenMS      = 0;
  m_bUserPagedUp = false;
  m_bUserPagedDown = false;
  m_fMotionVectorsTempDx = 0.0f;
  m_fMotionVectorsTempDy = 0.0f;

  m_waitstring.bActive = false;
  m_waitstring.bOvertypeMode = false;
  m_waitstring.szClipboard[0] = 0;

  m_nPresets = 0;
  m_nDirs = 0;
  m_nPresetListCurPos = 0;
  m_nCurrentPreset = -1;
  m_szCurrentPresetFile[0] = 0;
  m_szLoadingPreset[0] = 0;
  //m_szPresetDir[0] = 0; // will be set @ end of this function
  m_bPresetListReady = false;
  m_szUpdatePresetMask[0] = 0;
  //m_nRatingReadProgress = -1;

  myfft.Init(576, MY_FFT_SAMPLES, -1);
  memset(&mysound, 0, sizeof(mysound));

  for (int i = 0; i < PRESET_HIST_LEN; i++)
    m_presetHistory[i] = L"";
  m_presetHistoryPos = 0;
  m_presetHistoryBackFence = 0;
  m_presetHistoryFwdFence = 0;

  //m_nTextHeightPixels = -1;
  //m_nTextHeightPixels_Fancy = -1;
  m_bShowFPS = false;
  m_bShowRating = false;
  m_bShowPresetInfo = false;
  m_bShowDebugInfo = false;
  m_bShowSongTitle = false;
  m_bShowSongTime = false;
  m_bShowSongLen = false;
  m_fShowRatingUntilThisTime = -1.0f;
  ClearErrors();
  m_szDebugMessage[0] = 0;
  // m_szSongTitle preserved across resize — track info has its own bucket

  m_lpVS[0] = NULL;
  m_lpVS[1] = NULL;
#if (NUM_BLUR_TEX>0)
  for (int i = 0; i < NUM_BLUR_TEX; i++)
    m_lpBlur[i] = NULL;
#endif

  for (int i = 0; i < NUM_SUPERTEXTS; i++) {
    m_lpDDSTitle[i] = NULL;
  }

  m_nTitleTexSizeX = 0;
  m_nTitleTexSizeY = 0;
  m_verts = NULL;
  m_verts_temp = NULL;
  m_vertinfo = NULL;
  m_indices_list = NULL;
  m_indices_strip = NULL;

  m_bMMX = false;
  m_bHasFocus = true;
  m_bHadFocus = false;
  m_bOrigScrollLockState = GetKeyState(VK_SCROLL) & 1;
  // m_bMilkdropScrollLockState is derived at end of MyReadConfig()

  m_nNumericInputMode = NUMERIC_INPUT_MODE_SPRITE;
  m_nNumericInputNum = 0;
  m_nNumericInputDigits = 0;
  //td_custom_msg_font   m_CustomMessageFont[MAX_CUSTOM_MESSAGE_FONTS];
  //td_custom_msg        m_CustomMessage[MAX_CUSTOM_MESSAGES];

  texmgr      m_texmgr;		// for user sprites
  KillAllSupertexts();
  // --------------------other init--------------------

  g_bDebugOutput = false;
  g_bDumpFileCleared = false;

  // m_szBaseDir already points to the directory containing "resources\"
  swprintf(m_szMilkdrop2Path, L"%s%s", m_szBaseDir, SUBDIR);
  swprintf(m_szPresetDir, L"%spresets\\", m_szMilkdrop2Path);

  // note that the config dir can be under Program Files or Application Data!!
  wchar_t szConfigDir[MAX_PATH] = { 0 };
  lstrcpyW(szConfigDir, GetConfigIniFile());
  wchar_t* p = wcsrchr(szConfigDir, L'\\');
  if (p) *(p + 1) = 0;
  swprintf(m_szMsgIniFile, L"%s%s", szConfigDir, MSG_INIFILE);
  swprintf(m_szImgIniFile, L"%s%s", szConfigDir, IMG_INIFILE);
}

//----------------------------------------------------------------------

void Engine::MyReadConfig() {
  // Read the user's settings from the .INI file.
  // If you've added any controls to the config panel, read their value in
  //   from the .INI file here.

  // use this function         declared in   to read a value of this type:
  // -----------------         -----------   ----------------------------
  // GetPrivateProfileInt      Win32 API     int
  // GetPrivateProfileBool     utility.h     bool
  // GetPrivateProfileBOOL     utility.h     BOOL
  // GetPrivateProfileFloat    utility.h     float
  // GetPrivateProfileString   Win32 API     string

  //ex: m_fog_enabled = GetPrivateProfileInt("settings","fog_enabled"       ,m_fog_enabled       ,GetConfigIniFile());

  int n = 0;
  wchar_t* pIni = GetConfigIniFile();

  // ======================================
  // SPOUT - save whether in DirectX11 (true) or DirectX 9 (false) mode, default true
  bSpoutOut = GetPrivateProfileBoolW(L"Settings", L"bSpoutOut", bSpoutOut, pIni);
  bQualityAuto = GetPrivateProfileBoolW(L"Settings", L"bQualityAuto", bQualityAuto, pIni);
  bSpoutFixedSize = GetPrivateProfileBoolW(L"Settings", L"bSpoutFixedSize", bSpoutFixedSize, pIni);
  nSpoutFixedWidth = GetPrivateProfileIntW(L"Settings", L"nSpoutFixedWidth", nSpoutFixedWidth, pIni);
  nSpoutFixedHeight = GetPrivateProfileIntW(L"Settings", L"nSpoutFixedHeight", nSpoutFixedHeight, pIni);

  // Display outputs (monitors + Spout senders) — enumerate first, then load saved settings
  EnumerateDisplayOutputs();
  LoadDisplayOutputSettings();

  // Global hotkeys
  LoadHotkeySettings();

  // Idle timer (screensaver mode)
  LoadIdleTimerSettings();

  // Spout video input
  LoadSpoutInputSettings();

  // Game controller
  LoadControllerSettings();
  LoadControllerJSON();

  // MIDI input
  LoadMidiSettings();
  LoadMidiJSON();

  m_nInjectEffectMode = GetPrivateProfileIntW(L"Settings", L"nInjectEffectMode", 0, pIni);
  m_nInjectEffectMode = max(0, min(4, m_nInjectEffectMode)); // clamp to valid range
  // ======================================
  m_fRenderQuality = GetPrivateProfileFloatW(L"Settings", L"fRenderQuality", m_fRenderQuality, pIni);

  m_bFirstRun = !GetPrivateProfileBoolW(L"Settings", L"bConfigured", false, pIni);
  m_bEnableRating = GetPrivateProfileBoolW(L"Settings", L"bEnableRating", m_bEnableRating, pIni);
  m_bEnableMouseInteraction = GetPrivateProfileBoolW(L"Settings", L"bEnableMouseInteraction", m_bEnableMouseInteraction, pIni);

  //m_bInstaScan    = GetPrivateProfileBool("settings","bInstaScan",m_bInstaScan,pIni);
  m_bHardCutsDisabled = GetPrivateProfileBoolW(L"Settings", L"bHardCutsDisabled", m_bHardCutsDisabled, pIni);
  g_bDebugOutput = GetPrivateProfileBoolW(L"Settings", L"bDebugOutput", g_bDebugOutput, pIni);
  //m_bShowSongInfo = GetPrivateProfileBool("settings","bShowSongInfo",m_bShowSongInfo,pIni);
  //m_bShowPresetInfo=GetPrivateProfileBool("settings","bShowPresetInfo",m_bShowPresetInfo,pIni);
  // m_bShowPressF1ForHelp = GetPrivateProfileBoolW(L"Settings", L"bShowPressF1ForHelp", m_bShowPressF1ForHelp, pIni);
  //m_bShowMenuToolTips = GetPrivateProfileBool("settings","bShowMenuToolTips",m_bShowMenuToolTips,pIni);
  m_bSongTitleAnims = GetPrivateProfileBoolW(L"Settings", L"bSongTitleAnims", m_bSongTitleAnims, pIni);
  m_nSpriteMessagesMode = GetPrivateProfileIntW(L"Settings", L"nSpriteMessagesMode", 3, pIni);
  if (m_nSpriteMessagesMode < 0 || m_nSpriteMessagesMode > 3) m_nSpriteMessagesMode = 3;
  m_bEnablePresetStartup = GetPrivateProfileBoolW(L"Settings", L"bEnablePresetStartup", m_bEnablePresetStartup, pIni);
  m_bEnableAudioCapture = GetPrivateProfileBoolW(L"Settings", L"bEnableAudioCapture", m_bEnableAudioCapture, pIni);
  m_bEnableD2DText = GetPrivateProfileBoolW(L"Settings", L"bEnableD2DText", m_bEnableD2DText, pIni);
  m_fAudioSensitivity = GetPrivateProfileFloatW(L"Milkwave", L"AudioSensitivity", m_fAudioSensitivity, pIni);
  if (m_fAudioSensitivity < -1.0f) m_fAudioSensitivity = -1.0f;
  if (m_fAudioSensitivity > 256.0f) m_fAudioSensitivity = 256.0f;
  if (m_fAudioSensitivity == -1.0f) {
    mdropdx12_audio_adaptive = true;
    mdropdx12_audio_sensitivity = 1.0f;   // fallback, not used in adaptive mode
  } else {
    mdropdx12_audio_adaptive = false;
    if (m_fAudioSensitivity < 0.5f) m_fAudioSensitivity = 0.5f;
    mdropdx12_audio_sensitivity = m_fAudioSensitivity;
  }
  { char dbg[128]; sprintf(dbg, "AudioSensitivity: %.2f, adaptive=%d, gain=%.2f",
    m_fAudioSensitivity, (int)mdropdx12_audio_adaptive, mdropdx12_audio_sensitivity);
    DebugLogA(dbg); }
  m_bEnablePresetStartupSavingOnClose = GetPrivateProfileBoolW(L"Settings", L"bEnablePresetStartupSavingOnClose", m_bEnablePresetStartupSavingOnClose, pIni);

  m_bAutoLockPresetWhenNoMusic = GetPrivateProfileBoolW(L"Settings", L"bAutoLockPresetWhenNoMusic", m_bAutoLockPresetWhenNoMusic, pIni);
  m_bScreenDependentRenderMode = GetPrivateProfileBoolW(L"Settings", L"bScreenDependentRenderMode", m_bScreenDependentRenderMode, pIni);

  m_bShowFPS = GetPrivateProfileBoolW(L"Settings", L"bShowFPS", m_bShowFPS, pIni);
  m_bShowRating = GetPrivateProfileBoolW(L"Settings", L"bShowRating", m_bShowRating, pIni);
  m_bShowPresetInfo = GetPrivateProfileBoolW(L"Settings", L"bShowPresetInfo", m_bShowPresetInfo, pIni);
  //m_bShowDebugInfo	= GetPrivateProfileBool("settings","bShowDebugInfo", m_bShowDebugInfo	,pIni);
  m_bShowSongTitle = GetPrivateProfileBoolW(L"Settings", L"bShowSongTitle", m_bShowSongTitle, pIni);
  m_bShowSongTime = GetPrivateProfileBoolW(L"Settings", L"bShowSongTime", m_bShowSongTime, pIni);
  m_bShowSongLen = GetPrivateProfileBoolW(L"Settings", L"bShowSongLen", m_bShowSongLen, pIni);

  //m_bFixPinkBug		= GetPrivateProfileBool("settings","bFixPinkBug",m_bFixPinkBug,pIni);
  int nTemp = GetPrivateProfileBoolW(L"Settings", L"bFixPinkBug", -1, pIni);
  if (nTemp == 0)
    m_n16BitGamma = 0;
  else if (nTemp == 1)
    m_n16BitGamma = 2;
  m_n16BitGamma = GetPrivateProfileIntW(L"Settings", L"n16BitGamma", m_n16BitGamma, pIni);
  m_bAutoGamma = GetPrivateProfileBoolW(L"Settings", L"bAutoGamma", m_bAutoGamma, pIni);
  //m_bAlways3D				= GetPrivateProfileBool("settings","bAlways3D",m_bAlways3D,pIni);
    //m_fStereoSep            = GetPrivateProfileFloat("settings","fStereoSep",m_fStereoSep,pIni);
  //m_bFixSlowText          = GetPrivateProfileBool("settings","bFixSlowText",m_bFixSlowText,pIni);
  //m_bAlwaysOnTop		= GetPrivateProfileBool("settings","bAlwaysOnTop",m_bAlwaysOnTop,pIni);
  //m_bWarningsDisabled		= GetPrivateProfileBool("settings","bWarningsDisabled",m_bWarningsDisabled,pIni);
  m_bWarningsDisabled2 = GetPrivateProfileBoolW(L"Settings", L"bWarningsDisabled2", m_bWarningsDisabled2, pIni);
  //m_bAnisotropicFiltering = GetPrivateProfileBool("settings","bAnisotropicFiltering",m_bAnisotropicFiltering,pIni);
  m_bPresetLockOnAtStartup = GetPrivateProfileBoolW(L"Settings", L"bPresetLockOnAtStartup", m_bPresetLockOnAtStartup, pIni);
  m_bSequentialPresetOrder = GetPrivateProfileBoolW(L"Settings", L"bSequentialPresetOrder", m_bSequentialPresetOrder, pIni);

  m_bPreventScollLockHandling = GetPrivateProfileBoolW(L"Settings", L"m_bPreventScollLockHandling", m_bPreventScollLockHandling, pIni);

  m_nCanvasStretch = 100;  //GetPrivateProfileIntW(L"Settings",L"nCanvasStretch"    ,m_nCanvasStretch,pIni);
  m_nTexSizeX = -1; //GetPrivateProfileIntW(L"Settings",L"nTexSize"    ,m_nTexSizeX   ,pIni);
  m_nTexSizeY = -1; //m_nTexSizeX;
  m_bTexSizeWasAutoPow2 = (m_nTexSizeX == -2);
  m_bTexSizeWasAutoExact = (m_nTexSizeX == -1);
  m_nTexBitsPerCh = GetPrivateProfileIntW(L"Settings", L"nTexBitsPerCh", m_nTexBitsPerCh, pIni);
  m_nGridX = GetPrivateProfileIntW(L"Settings", L"nMeshSize", m_nGridX, pIni);
  m_nGridY = m_nGridX * 3 / 4;

  m_nMaxPSVersion_ConfigPanel = GetPrivateProfileIntW(L"Settings", L"MaxPSVersion", m_nMaxPSVersion_ConfigPanel, pIni);
  m_nMaxImages = GetPrivateProfileIntW(L"Settings", L"MaxImages", m_nMaxImages, pIni);
  m_nMaxBytes = GetPrivateProfileIntW(L"Settings", L"MaxBytes", m_nMaxBytes, pIni);
  m_nBassStart = GetPrivateProfileIntW(L"Settings", L"BassStart", m_nBassStart, pIni);
  m_nBassEnd = GetPrivateProfileIntW(L"Settings", L"BassEnd", m_nBassEnd, pIni);
  m_nMidStart = GetPrivateProfileIntW(L"Settings", L"MidStart", m_nMidStart, pIni);
  m_nMidEnd = GetPrivateProfileIntW(L"Settings", L"MidEnd", m_nMidEnd, pIni);
  m_nTrebStart = GetPrivateProfileIntW(L"Settings", L"TrebStart", m_nTrebStart, pIni);
  m_nTrebEnd = GetPrivateProfileIntW(L"Settings", L"TrebEnd", m_nTrebEnd, pIni);

  m_fBlendTimeUser = GetPrivateProfileFloatW(L"Settings", L"fBlendTimeUser", m_fBlendTimeUser, pIni);
  m_fBlendTimeAuto = GetPrivateProfileFloatW(L"Settings", L"fBlendTimeAuto", m_fBlendTimeAuto, pIni);
  m_fTimeBetweenPresets = GetPrivateProfileFloatW(L"Settings", L"fTimeBetweenPresets", m_fTimeBetweenPresets, pIni);
  m_fTimeBetweenPresetsRand = GetPrivateProfileFloatW(L"Settings", L"fTimeBetweenPresetsRand", m_fTimeBetweenPresetsRand, pIni);

  m_fHardCutLoudnessThresh = GetPrivateProfileFloatW(L"Settings", L"fHardCutLoudnessThresh", m_fHardCutLoudnessThresh, pIni);
  m_fHardCutHalflife = GetPrivateProfileFloatW(L"Settings", L"fHardCutHalflife", m_fHardCutHalflife, pIni);
  m_fSongTitleAnimDuration = GetPrivateProfileFloatW(L"Settings", L"fSongTitleAnimDuration", m_fSongTitleAnimDuration, pIni);
  m_fTimeBetweenRandomSongTitles = GetPrivateProfileFloatW(L"Settings", L"fTimeBetweenRandomSongTitles", m_fTimeBetweenRandomSongTitles, pIni);
  m_fTimeBetweenRandomCustomMsgs = GetPrivateProfileFloatW(L"Settings", L"fTimeBetweenRandomCustomMsgs", m_fTimeBetweenRandomCustomMsgs, pIni);
  m_adapterId = GetPrivateProfileIntW(L"Settings", L"nVideoAdapterIndex", 0, pIni);

  // --------

  GetPrivateProfileStringW(L"Settings", L"szPresetDir", m_szPresetDir, m_szPresetDir, sizeof(m_szPresetDir), pIni);

  // Validate preset directory — if it doesn't exist, try default or create it
  if (GetFileAttributesW(m_szPresetDir) == INVALID_FILE_ATTRIBUTES) {
    // Try the default presets dir before flagging
    wchar_t szDefault[MAX_PATH];
    swprintf(szDefault, L"%spresets\\", m_szMilkdrop2Path);
    if (GetFileAttributesW(szDefault) != INVALID_FILE_ATTRIBUTES) {
      lstrcpyW(m_szPresetDir, szDefault);
      TryDescendIntoPresetSubdirHelper(m_szPresetDir);
      WritePrivateProfileStringW(L"Settings", L"szPresetDir", m_szPresetDir, pIni);
    }
    else {
      // Self-bootstrap: create the default preset directory
      CreateDirectoryW(szDefault, NULL);
      lstrcpyW(m_szPresetDir, szDefault);
      DebugLogA("Created default preset directory (self-bootstrap)");
    }
  }

  GetPrivateProfileStringW(L"Settings", L"szPresetStartup", m_szPresetStartup, m_szPresetStartup, sizeof(m_szPresetStartup), pIni);

  // MDropDX12:
  GetPrivateProfileStringW(L"Milkwave", L"AudioDevice", m_szAudioDevice, m_szAudioDevice, sizeof(m_szAudioDevice), pIni);
  m_nAudioDeviceRequestType = GetPrivateProfileIntW(L"Milkwave", L"AudioDeviceRequestType", m_nAudioDeviceRequestType, pIni);
  m_nTrackInfoSource = GetPrivateProfileIntW(L"Milkwave", L"TrackInfoSource", m_nTrackInfoSource, pIni);
  if (m_nTrackInfoSource < 0 || m_nTrackInfoSource > 2) m_nTrackInfoSource = TRACK_SOURCE_SMTC;
  m_bSongInfoOverlay = GetPrivateProfileBoolW(L"Milkwave", L"SongInfoOverlay", m_bSongInfoOverlay, pIni);
  GetPrivateProfileStringW(L"Milkwave", L"TrackWindowTitle", L"", m_szTrackWindowTitle, _countof(m_szTrackWindowTitle), pIni);

  // Window Title Profiles
  {
    int profileCount = GetPrivateProfileIntW(L"Milkwave", L"WindowTitleProfileCount", 0, pIni);
    m_nActiveWindowTitleProfile = GetPrivateProfileIntW(L"Milkwave", L"WindowTitleProfile", 0, pIni);
    m_windowTitleProfiles.clear();
    for (int i = 0; i < profileCount; i++) {
      WindowTitleProfile p;
      wchar_t key[64];
      swprintf(key, 64, L"WTP%d_Name", i);
      GetPrivateProfileStringW(L"Milkwave", key, L"", p.szName, _countof(p.szName), pIni);
      swprintf(key, 64, L"WTP%d_WindowRegex", i);
      GetPrivateProfileStringW(L"Milkwave", key, L"", p.szWindowRegex, _countof(p.szWindowRegex), pIni);
      swprintf(key, 64, L"WTP%d_ParseRegex", i);
      GetPrivateProfileStringW(L"Milkwave", key, L"", p.szParseRegex, _countof(p.szParseRegex), pIni);
      swprintf(key, 64, L"WTP%d_PollInterval", i);
      p.nPollIntervalSec = GetPrivateProfileIntW(L"Milkwave", key, 2, pIni);
      if (p.nPollIntervalSec < 1) p.nPollIntervalSec = 1;
      if (p.nPollIntervalSec > 10) p.nPollIntervalSec = 10;
      m_windowTitleProfiles.push_back(p);
    }
    // Backward compat: migrate old TrackWindowTitle to a default profile
    if (m_windowTitleProfiles.empty() && m_szTrackWindowTitle[0] != L'\0') {
      WindowTitleProfile p;
      wcscpy_s(p.szName, L"Default");
      // Use a simple .*title.* pattern so it broadly matches
      // (the old value was an exact window title for FindWindowW)
      std::wstring pattern = L".*";
      // Regex-escape special chars in the old title
      for (const wchar_t* c = m_szTrackWindowTitle; *c; ++c) {
        if (wcschr(L"\\^$.|?*+()[]{}", *c))
          pattern += L'\\';
        pattern += *c;
      }
      pattern += L".*";
      wcsncpy_s(p.szWindowRegex, pattern.c_str(), _countof(p.szWindowRegex) - 1);
      wcscpy_s(p.szParseRegex, L"(?<artist>.+?) - (?<title>.+)");
      p.nPollIntervalSec = 2;
      m_windowTitleProfiles.push_back(p);
      m_nActiveWindowTitleProfile = 0;
    }
    if (m_nActiveWindowTitleProfile < 0 || m_nActiveWindowTitleProfile >= (int)m_windowTitleProfiles.size())
      m_nActiveWindowTitleProfile = 0;
  }

  m_SongInfoPollingEnabled = GetPrivateProfileBoolW(L"Milkwave", L"SongInfoPollingEnabled", m_SongInfoPollingEnabled, pIni);
  m_SongInfoDisplayCorner = GetPrivateProfileIntW(L"Milkwave", L"SongInfoDisplayCorner", m_SongInfoDisplayCorner, pIni);
  GetPrivateProfileStringW(L"Milkwave", L"SongInfoFormat", L"Artist;Title;Album", m_SongInfoFormat, sizeof(m_SongInfoFormat), pIni);
  m_ChangePresetWithSong = GetPrivateProfileBoolW(L"Milkwave", L"ChangePresetWithSong", m_ChangePresetWithSong, pIni);
  m_SongInfoDisplaySeconds = GetPrivateProfileFloatW(L"Milkwave", L"SongInfoDisplaySeconds", m_SongInfoDisplaySeconds, pIni);
  m_DisplayCover = GetPrivateProfileBoolW(L"Milkwave", L"DisplayCover", m_DisplayCover, pIni);
  m_DisplayCoverWhenPressingB = GetPrivateProfileBoolW(L"Milkwave", L"DisplayCoverWhenPressingB", m_DisplayCoverWhenPressingB, pIni);
  m_bSongInfoAlwaysShow = GetPrivateProfileBoolW(L"Milkwave", L"SongInfoAlwaysShow", m_bSongInfoAlwaysShow, pIni);
  m_HideNotificationsWhenRemoteActive = GetPrivateProfileBoolW(L"Milkwave", L"HideNotificationsWhenRemoteActive", m_HideNotificationsWhenRemoteActive, pIni);

  m_ShowLockSymbol = GetPrivateProfileBoolW(L"Milkwave", L"ShowLockSymbol", m_ShowLockSymbol, pIni);
  m_ShaderCaching = GetPrivateProfileBoolW(L"Milkwave", L"ShaderCaching", m_ShaderCaching, pIni);
  m_ShaderPrecompileOnStartup = GetPrivateProfileBoolW(L"Milkwave", L"ShaderPrecompileOnStartup", m_ShaderPrecompileOnStartup, pIni);
  m_CheckDirectXOnStartup = GetPrivateProfileBoolW(L"Milkwave", L"CheckDirectXOnStartup", m_CheckDirectXOnStartup, pIni);
  m_LogLevel = GetPrivateProfileIntW(L"Milkwave", L"LogLevel", m_LogLevel, pIni);
  if (m_bSelfBootstrapped && m_LogLevel < 4)
    m_LogLevel = 4; // verbose logging for first-run diagnostics
  DebugLogSetLevel(m_LogLevel); // apply log level to DebugLog system
  GetPrivateProfileStringW(L"Milkwave", L"WindowTitle", L"", m_szWindowTitle, 256, pIni);
  GetPrivateProfileStringW(L"Milkwave", L"RemoteWindowTitle", L"", m_szRemoteWindowTitle, 256, pIni);

  m_blackmode = GetPrivateProfileBoolW(L"Milkwave", L"BlackMode", m_blackmode, pIni);

  m_MessageDefaultBurnTime = GetPrivateProfileFloatW(L"Milkwave", L"MessageDefaultBurnTime", m_MessageDefaultBurnTime, pIni);
  m_MessageDefaultFadeinTime = GetPrivateProfileFloatW(L"Milkwave", L"MessageDefaultFadeinTime", m_MessageDefaultFadeinTime, pIni);
  m_MessageDefaultFadeoutTime = GetPrivateProfileFloatW(L"Milkwave", L"MessageDefaultFadeoutTime", m_MessageDefaultFadeoutTime, pIni);

  // We'll put these in the settings section since other MilkDrop forks use similar settings
  m_MinPSVersionConfig = GetPrivateProfileIntW(L"Settings", L"MinPSVersion", m_MinPSVersionConfig, pIni);
  if (m_MinPSVersionConfig < 0) m_MinPSVersionConfig = 2;
  m_MaxPSVersionConfig = GetPrivateProfileIntW(L"Settings", L"MaxPSVersion", m_MaxPSVersionConfig, pIni);
  if (m_MaxPSVersionConfig < 0) m_MaxPSVersionConfig = 4;
  m_nMixType = GetPrivateProfileIntW(L"Settings", L"Mixtype", m_nMixType, pIni);

  m_ShowUpArrowInDescriptionIfPSMinVersionForced = GetPrivateProfileBoolW(L"Milkwave", L"ShowUpArrowInDescriptionIfPSMinVersionForced", m_ShowUpArrowInDescriptionIfPSMinVersionForced, pIni);

  // GPU Protection
  m_nMaxShapeInstances = GetPrivateProfileIntW(L"Milkwave", L"MaxShapeInstances", m_nMaxShapeInstances, pIni);
  if (m_nMaxShapeInstances < 0) m_nMaxShapeInstances = 0;
  m_bScaleInstancesByResolution = GetPrivateProfileBoolW(L"Milkwave", L"ScaleInstancesByResolution", m_bScaleInstancesByResolution, pIni);
  m_nInstanceScaleBaseWidth = GetPrivateProfileIntW(L"Milkwave", L"InstanceScaleBaseWidth", m_nInstanceScaleBaseWidth, pIni);
  if (m_nInstanceScaleBaseWidth < 640) m_nInstanceScaleBaseWidth = 640;
  m_bSkipHeavyPresets = GetPrivateProfileBoolW(L"Milkwave", L"SkipHeavyPresets", m_bSkipHeavyPresets, pIni);
  m_nHeavyPresetMaxInstances = GetPrivateProfileIntW(L"Milkwave", L"HeavyPresetMaxInstances", m_nHeavyPresetMaxInstances, pIni);
  if (m_nHeavyPresetMaxInstances < 64) m_nHeavyPresetMaxInstances = 64;
  m_bEnableVSync = GetPrivateProfileBoolW(L"Milkwave", L"EnableVSync", m_bEnableVSync, pIni);
  m_fShaderCompileTimeout = GetPrivateProfileFloatW(L"Milkwave", L"ShaderCompileTimeout", m_fShaderCompileTimeout, pIni);
  if (m_fShaderCompileTimeout < 2.0f) m_fShaderCompileTimeout = 2.0f;

  m_WindowBorderless = GetPrivateProfileBoolW(L"Milkwave", L"WindowBorderless", m_WindowBorderless, pIni);
  m_bAlwaysOnTop = GetPrivateProfileBoolW(L"Milkwave", L"WindowAlwaysOnTop", m_bAlwaysOnTop, pIni);

  fOpacity = GetPrivateProfileFloatW(L"Milkwave", L"WindowOpacity", fOpacity, pIni);
  m_WindowWatermarkModeOpacity = GetPrivateProfileFloatW(L"Milkwave", L"WindowWatermarkModeOpacity", m_WindowWatermarkModeOpacity, pIni);
  m_WindowX = GetPrivateProfileIntW(L"Milkwave", L"WindowX", m_WindowX, pIni);
  m_WindowY = GetPrivateProfileIntW(L"Milkwave", L"WindowY", m_WindowY, pIni);
  m_WindowWidth = GetPrivateProfileIntW(L"Milkwave", L"WindowWidth", m_WindowWidth, pIni);
  m_WindowHeight = GetPrivateProfileIntW(L"Milkwave", L"WindowHeight", m_WindowHeight, pIni);
  // Settings window position/size now managed by ToolWindow::LoadWindowPosition()
  m_nSettingsFontSize = GetPrivateProfileIntW(L"Milkwave", L"SettingsFontSize", -16, pIni);
  if (m_nSettingsFontSize > -12) m_nSettingsFontSize = -12;  // min font size
  if (m_nSettingsFontSize < -24) m_nSettingsFontSize = -24;  // max font size

  // Settings window theme mode (Dark/Light/Follow System)
  // Migration: if new ThemeMode key doesn't exist yet, read old DarkTheme bool
  if (GetPrivateProfileIntW(L"SettingsTheme", L"ThemeMode", -1, pIni) == -1) {
    bool oldDark = GetPrivateProfileIntW(L"SettingsTheme", L"DarkTheme", 1, pIni) != 0;
    m_nThemeMode = oldDark ? THEME_DARK : THEME_LIGHT;
  } else {
    m_nThemeMode = (ThemeMode)GetPrivateProfileIntW(L"SettingsTheme", L"ThemeMode", (int)m_nThemeMode, pIni);
    if (m_nThemeMode < THEME_DARK || m_nThemeMode > THEME_SYSTEM) m_nThemeMode = THEME_DARK;
  }
  m_WindowFixedWidth = GetPrivateProfileIntW(L"Milkwave", L"WindowFixedWidth", m_WindowFixedWidth, pIni);
  m_WindowFixedHeight = GetPrivateProfileIntW(L"Milkwave", L"WindowFixedHeight", m_WindowFixedHeight, pIni);

  ReadCustomMessages();
  BuildMsgPlaybackOrder();
  LoadMsgAutoplaySettings();
  if (m_bMsgAutoplay)
    ScheduleNextAutoMessage();
  LoadUserDefaults();
  LoadFallbackPaths();

  // bounds-checking:
  if (m_nGridX > MAX_GRID_X)
    m_nGridX = MAX_GRID_X;
  if (m_nGridY > MAX_GRID_Y)
    m_nGridY = MAX_GRID_Y;
  if (m_fTimeBetweenPresetsRand < 0)
    m_fTimeBetweenPresetsRand = 0;
  if (m_fTimeBetweenPresets < 0.1f)
    m_fTimeBetweenPresets = 0.1f;

  // DERIVED SETTINGS
  m_bPresetLockedByUser = m_bPresetLockOnAtStartup;
  //m_bMilkdropScrollLockState = m_bPresetLockOnAtStartup;
}

//----------------------------------------------------------------------

void Engine::MyWriteConfig() {
  // Write the user's settings to the .INI file.
  // This gets called only when the user runs the config panel and hits OK.
  // If you've added any controls to the config panel, write their value out
  //   to the .INI file here.

  // use this function         declared in   to write a value of this type:
  // -----------------         -----------   ----------------------------
  // WritePrivateProfileInt    Win32 API     int
  // WritePrivateProfileInt    utility.h     bool
  // WritePrivateProfileInt    utility.h     BOOL
  // WritePrivateProfileFloat  utility.h     float
  // WritePrivateProfileString Win32 API     string

  // ex: WritePrivateProfileInt(m_fog_enabled       ,"fog_enabled"       ,GetConfigIniFile(),"settings");

  wchar_t* pIni = GetConfigIniFile();

  // constants:
  WritePrivateProfileStringW(L"Settings", L"bConfigured", L"1", pIni);

  //note: m_szPresetDir is not written here; it is written manually, whenever it changes.

  wchar_t szSectionName[] = L"Settings";

  // ================================
  // SPOUT
  WritePrivateProfileIntW(bSpoutOut, L"bSpoutOut", pIni, L"Settings");
  WritePrivateProfileIntW(bQualityAuto, L"bQualityAuto", pIni, L"Settings");
  WritePrivateProfileIntW(bSpoutFixedSize, L"bSpoutFixedSize", pIni, L"Settings");
  WritePrivateProfileIntW(nSpoutFixedWidth, L"nSpoutFixedWidth", pIni, L"Settings");
  WritePrivateProfileIntW(nSpoutFixedHeight, L"nSpoutFixedHeight", pIni, L"Settings");

  // Display outputs (monitors + Spout senders)
  SaveDisplayOutputSettings();
  // ================================
  WritePrivateProfileFloatW(m_fRenderQuality, L"fRenderQuality", pIni, L"Settings");

  WritePrivateProfileIntW(m_bSongTitleAnims, L"bSongTitleAnims", pIni, L"Settings");
  WritePrivateProfileIntW(m_nSpriteMessagesMode, L"nSpriteMessagesMode", pIni, L"Settings");
  WritePrivateProfileIntW(m_bHardCutsDisabled, L"bHardCutsDisabled", pIni, L"Settings");
  WritePrivateProfileIntW(m_nInjectEffectMode, L"nInjectEffectMode", pIni, L"Settings");
  WritePrivateProfileIntW(m_bEnableRating, L"bEnableRating", pIni, L"Settings");
  WritePrivateProfileIntW(m_bEnableMouseInteraction, L"bEnableMouseInteraction", pIni, L"Settings");

  //WritePrivateProfileIntW(m_bInstaScan,            "bInstaScan",		    pIni, "settings");
  WritePrivateProfileIntW(g_bDebugOutput, L"bDebugOutput", pIni, L"Settings");

  //itePrivateProfileInt(m_bShowPresetInfo, 	    "bShowPresetInfo",		pIni, "settings");
  //itePrivateProfileInt(m_bShowSongInfo, 		"bShowSongInfo",        pIni, "settings");
  //itePrivateProfileInt(m_bFixPinkBug, 		    "bFixPinkBug",			pIni, "settings");

  //WritePrivateProfileIntW(m_bShowPressF1ForHelp, L"bShowPressF1ForHelp", pIni, L"Settings");
  //itePrivateProfileInt(m_bShowMenuToolTips, 	"bShowMenuToolTips",    pIni, "settings");
  WritePrivateProfileIntW(m_n16BitGamma, L"n16BitGamma", pIni, L"Settings");
  WritePrivateProfileIntW(m_bAutoGamma, L"bAutoGamma", pIni, L"Settings");

  //WritePrivateProfileIntW(m_bAlways3D, 			"bAlways3D",			pIni, "settings");
    //WritePrivateProfileFloat(m_fStereoSep,          "fStereoSep",           pIni, "settings");
  //WritePrivateProfileIntW(m_bFixSlowText,		    "bFixSlowText",			pIni, "settings");
  //itePrivateProfileInt(m_bAlwaysOnTop,		    "bAlwaysOnTop",			pIni, "settings");
  //WritePrivateProfileIntW(m_bWarningsDisabled,	    "bWarningsDisabled",	pIni, "settings");
  WritePrivateProfileIntW(m_bWarningsDisabled2, L"bWarningsDisabled2", pIni, L"Settings");
  //WritePrivateProfileIntW(m_bAnisotropicFiltering,	"bAnisotropicFiltering",pIni, "settings");
  WritePrivateProfileIntW(m_bPresetLockOnAtStartup, L"bPresetLockOnAtStartup", pIni, L"Settings");
  WritePrivateProfileIntW(m_bSequentialPresetOrder, L"bSequentialPresetOrder", pIni, L"Settings");

  WritePrivateProfileIntW(m_bPreventScollLockHandling, L"m_bPreventScollLockHandling", pIni, L"Settings");
  // note: this is also written @ exit of the visualizer
  WritePrivateProfileIntW(m_bEnablePresetStartup, L"bEnablePresetStartup", pIni, L"Settings");
  WritePrivateProfileIntW(m_bAutoLockPresetWhenNoMusic, L"bAutoLockPresetWhenNoMusic", pIni, L"Settings");
  WritePrivateProfileIntW(m_bScreenDependentRenderMode, L"bScreenDependentRenderMode", pIni, L"Settings");

  WritePrivateProfileIntW(m_nCanvasStretch, L"nCanvasStretch", pIni, L"Settings");
  //WritePrivateProfileIntW(m_nTexSizeX,			    L"nTexSize",				pIni, L"Settings");
  WritePrivateProfileIntW(m_nTexBitsPerCh, L"nTexBitsPerCh", pIni, L"Settings");
  WritePrivateProfileIntW(m_nGridX, L"nMeshSize", pIni, L"Settings");
  WritePrivateProfileIntW(m_nMaxPSVersion_ConfigPanel, L"MaxPSVersion", pIni, L"Settings");
  WritePrivateProfileIntW(m_nMaxImages, L"MaxImages", pIni, L"Settings");
  WritePrivateProfileIntW(m_nMaxBytes, L"MaxBytes", pIni, L"Settings");
  WritePrivateProfileIntW(m_nBassStart, L"BassStart", pIni, L"Settings");
  WritePrivateProfileIntW(m_nBassEnd, L"BassEnd", pIni, L"Settings");
  WritePrivateProfileIntW(m_nMidStart, L"MidStart", pIni, L"Settings");
  WritePrivateProfileIntW(m_nMidEnd, L"MidEnd", pIni, L"Settings");
  WritePrivateProfileIntW(m_nTrebStart, L"TrebStart", pIni, L"Settings");
  WritePrivateProfileIntW(m_nTrebEnd, L"TrebEnd", pIni, L"Settings");

  WritePrivateProfileFloatW(m_fBlendTimeAuto, L"fBlendTimeAuto", pIni, L"Settings");
  WritePrivateProfileFloatW(m_fBlendTimeUser, L"fBlendTimeUser", pIni, L"Settings");
  WritePrivateProfileFloatW(m_fTimeBetweenPresets, L"fTimeBetweenPresets", pIni, L"Settings");
  WritePrivateProfileFloatW(m_fTimeBetweenPresetsRand, L"fTimeBetweenPresetsRand", pIni, L"Settings");
  WritePrivateProfileFloatW(m_fHardCutLoudnessThresh, L"fHardCutLoudnessThresh", pIni, L"Settings");
  WritePrivateProfileFloatW(m_fHardCutHalflife, L"fHardCutHalflife", pIni, L"Settings");
  WritePrivateProfileFloatW(m_fSongTitleAnimDuration, L"fSongTitleAnimDuration", pIni, L"Settings");
  WritePrivateProfileFloatW(m_fTimeBetweenRandomSongTitles, L"fTimeBetweenRandomSongTitles", pIni, L"Settings");
  WritePrivateProfileFloatW(m_fTimeBetweenRandomCustomMsgs, L"fTimeBetweenRandomCustomMsgs", pIni, L"Settings");

  WritePrivateProfileIntW(m_adapterId, L"nVideoAdapterIndex", pIni, L"Settings");
  WritePrivateProfileIntW(m_bPresetLockedByUser, L"bPresetLockOnAtStartup", GetConfigIniFile(), L"Settings");
  if (m_bEnablePresetStartupSavingOnClose) {
    WritePrivateProfileStringW(L"Settings", L"szPresetStartup", m_szCurrentPresetFile, pIni);
  }
  WritePrivateProfileIntW(m_bShowFPS, L"bShowFPS", GetConfigIniFile(), L"Settings");
  WritePrivateProfileIntW(m_bShowRating, L"bShowRating", GetConfigIniFile(), L"Settings");
  WritePrivateProfileIntW(m_bShowPresetInfo, L"bShowPresetInfo", GetConfigIniFile(), L"Settings");
  WritePrivateProfileIntW(m_show_press_f1_msg, L"show_press_f1_msg", GetConfigIniFile(), L"Settings");

  // MDropDX12:
  WritePrivateProfileStringW(L"Milkwave", L"AudioDevice", m_szAudioDevice, pIni);
  WritePrivateProfileIntW(m_nAudioDeviceRequestType, L"AudioDeviceRequestType", pIni, L"Milkwave");
  { wchar_t asBuf[32]; swprintf(asBuf, 32, L"%g", (double)m_fAudioSensitivity);
    WritePrivateProfileStringW(L"Milkwave", L"AudioSensitivity", asBuf, pIni); }
  WritePrivateProfileIntW(m_nTrackInfoSource, L"TrackInfoSource", pIni, L"Milkwave");
  WritePrivateProfileIntW(m_bSongInfoOverlay, L"SongInfoOverlay", pIni, L"Milkwave");
  WritePrivateProfileStringW(L"Milkwave", L"TrackWindowTitle", m_szTrackWindowTitle, pIni);

  // Window Title Profiles
  {
    int profileCount = (int)m_windowTitleProfiles.size();
    WritePrivateProfileIntW(profileCount, L"WindowTitleProfileCount", pIni, L"Milkwave");
    WritePrivateProfileIntW(m_nActiveWindowTitleProfile, L"WindowTitleProfile", pIni, L"Milkwave");
    for (int i = 0; i < profileCount; i++) {
      const auto& p = m_windowTitleProfiles[i];
      wchar_t key[64];
      swprintf(key, 64, L"WTP%d_Name", i);
      WritePrivateProfileStringW(L"Milkwave", key, p.szName, pIni);
      swprintf(key, 64, L"WTP%d_WindowRegex", i);
      WritePrivateProfileStringW(L"Milkwave", key, p.szWindowRegex, pIni);
      swprintf(key, 64, L"WTP%d_ParseRegex", i);
      WritePrivateProfileStringW(L"Milkwave", key, p.szParseRegex, pIni);
      swprintf(key, 64, L"WTP%d_PollInterval", i);
      wchar_t val[16]; swprintf(val, 16, L"%d", p.nPollIntervalSec);
      WritePrivateProfileStringW(L"Milkwave", key, val, pIni);
    }
    // Clean up old profile keys that may be leftover from a larger set
    for (int i = profileCount; i < profileCount + 10; i++) {
      wchar_t key[64];
      swprintf(key, 64, L"WTP%d_Name", i);
      wchar_t test[4] = {};
      GetPrivateProfileStringW(L"Milkwave", key, L"", test, 4, pIni);
      if (test[0] == L'\0') break; // no more old keys
      WritePrivateProfileStringW(L"Milkwave", key, NULL, pIni);
      swprintf(key, 64, L"WTP%d_WindowRegex", i);
      WritePrivateProfileStringW(L"Milkwave", key, NULL, pIni);
      swprintf(key, 64, L"WTP%d_ParseRegex", i);
      WritePrivateProfileStringW(L"Milkwave", key, NULL, pIni);
      swprintf(key, 64, L"WTP%d_PollInterval", i);
      WritePrivateProfileStringW(L"Milkwave", key, NULL, pIni);
    }
  }

  WritePrivateProfileIntW(m_SongInfoPollingEnabled, L"SongInfoPollingEnabled", pIni, L"Milkwave");
  WritePrivateProfileIntW(m_SongInfoDisplayCorner, L"SongInfoDisplayCorner", pIni, L"Milkwave");
  WritePrivateProfileStringW(L"Milkwave", L"SongInfoFormat", m_SongInfoFormat, pIni);
  { wchar_t secBuf[32]; swprintf(secBuf, 32, L"%.1f", m_SongInfoDisplaySeconds);
    WritePrivateProfileStringW(L"Milkwave", L"SongInfoDisplaySeconds", secBuf, pIni); }
  WritePrivateProfileIntW(m_ChangePresetWithSong, L"ChangePresetWithSong", pIni, L"Milkwave");
  WritePrivateProfileIntW(m_DisplayCover, L"DisplayCover", pIni, L"Milkwave");
  WritePrivateProfileIntW(m_DisplayCoverWhenPressingB, L"DisplayCoverWhenPressingB", pIni, L"Milkwave");
  WritePrivateProfileIntW(m_bSongInfoAlwaysShow, L"SongInfoAlwaysShow", pIni, L"Milkwave");
  WritePrivateProfileIntW(m_blackmode, L"BlackMode", pIni, L"Milkwave");
  WritePrivateProfileIntW(m_CheckDirectXOnStartup, L"CheckDirectXOnStartup", pIni, L"Milkwave");

  WritePrivateProfileIntW(m_WindowBorderless, L"WindowBorderless", pIni, L"Milkwave");
  WritePrivateProfileIntW(m_bAlwaysOnTop, L"WindowAlwaysOnTop", pIni, L"Milkwave");
  WritePrivateProfileStringW(L"Milkwave", L"WindowTitle", m_szWindowTitle, pIni);
  WritePrivateProfileStringW(L"Milkwave", L"RemoteWindowTitle", m_szRemoteWindowTitle, pIni);

  WritePrivateProfileFloatW(m_WindowWatermarkModeOpacity, L"WindowWatermarkModeOpacity", pIni, L"Milkwave");
  WritePrivateProfileFloatW(fOpacity, L"WindowOpacity", pIni, L"Milkwave");
  WritePrivateProfileIntW(m_WindowX, L"WindowX", pIni, L"Milkwave");
  WritePrivateProfileIntW(m_WindowY, L"WindowY", pIni, L"Milkwave");
  WritePrivateProfileIntW(m_WindowWidth, L"WindowWidth", pIni, L"Milkwave");
  WritePrivateProfileIntW(m_WindowHeight, L"WindowHeight", pIni, L"Milkwave");
  // Settings window position/size now managed by ToolWindow::SaveWindowPosition()
  WritePrivateProfileIntW(m_nSettingsFontSize, L"SettingsFontSize", pIni, L"Milkwave");

  // GPU Protection
  WritePrivateProfileIntW(m_nMaxShapeInstances, L"MaxShapeInstances", pIni, L"Milkwave");
  WritePrivateProfileIntW(m_bScaleInstancesByResolution, L"ScaleInstancesByResolution", pIni, L"Milkwave");
  WritePrivateProfileIntW(m_nInstanceScaleBaseWidth, L"InstanceScaleBaseWidth", pIni, L"Milkwave");
  WritePrivateProfileIntW(m_bSkipHeavyPresets, L"SkipHeavyPresets", pIni, L"Milkwave");
  WritePrivateProfileIntW(m_nHeavyPresetMaxInstances, L"HeavyPresetMaxInstances", pIni, L"Milkwave");
  WritePrivateProfileIntW(m_bEnableVSync, L"EnableVSync", pIni, L"Milkwave");
  WritePrivateProfileFloatW(m_fShaderCompileTimeout, L"ShaderCompileTimeout", pIni, L"Milkwave");
}

void Engine::SaveWindowSizeAndPosition(HWND hwnd) {
  RECT rect;
  if (GetWindowRect(hwnd, &rect)) {
    m_WindowX = rect.left;
    m_WindowY = rect.top;
    m_WindowWidth = rect.right - rect.left;
    m_WindowHeight = rect.bottom - rect.top;
  }
}

//----------------------------------------------------------------------

void ConvertLLCto1310(char* d, const char* s) {
  // src and dest can NOT be the same pointer.
  assert(s != d);

  while (*s) {
    if (*s == LINEFEED_CONTROL_CHAR) {
      *d++ = 13;
      *d++ = 10;
    }
    else {
      *d++ = *s;
    }
    s++;
  };
  *d = 0;
}

void StripComments(char* str) {
  if (!str || !str[0] || !str[1])
    return;

  char c0 = str[0];
  char c1 = str[1];
  char* dest = str;
  char* p = &str[1];
  bool bIgnoreTilEndOfLine = false;
  bool bIgnoreTilCloseComment = false; //this one takes precedence
  int nCharsToSkip = 0;
  while (1) {
    // handle '//' comments
    if (!bIgnoreTilCloseComment && c0 == '/' && c1 == '/')
      bIgnoreTilEndOfLine = true;
    if (bIgnoreTilEndOfLine && (c0 == 10 || c0 == 13)) {
      bIgnoreTilEndOfLine = false;
      nCharsToSkip = 0;
    }

    // handle /* */ comments
    if (!bIgnoreTilEndOfLine && c0 == '/' && c1 == '*')
      bIgnoreTilCloseComment = true;
    if (bIgnoreTilCloseComment && c0 == '*' && c1 == '/') {
      bIgnoreTilCloseComment = false;
      nCharsToSkip = 2;
    }

    if (!bIgnoreTilEndOfLine && !bIgnoreTilCloseComment) {
      if (nCharsToSkip > 0)
        nCharsToSkip--;
      else
        *dest++ = c0;
    }

    if (c1 == 0)
      break;

    p++;
    c0 = c1;
    c1 = *p;
  }

  *dest++ = 0;
}

int Engine::AllocateMyNonDx9Stuff() {
  // This gets called only once, when your plugin is actually launched.
  // If only the config panel is launched, this does NOT get called.
  //   (whereas MyPreInitialize() still does).
  // If anything fails here, return FALSE to safely exit the plugin,
  //   but only after displaying a messagebox giving the user some information
  //   about what went wrong.

  /*
  if (!m_hBlackBrush)
  m_hBlackBrush = CreateSolidBrush(RGB(0,0,0));
  */

  g_hThread = INVALID_HANDLE_VALUE;
  g_bThreadAlive = false;
  g_bThreadShouldQuit = false;
  InitializeCriticalSection(&g_cs);

  // read in 'm_szShaderIncludeText'
  bool bSuccess = true;
  bSuccess = ReadFileToString(L"data\\include.fx", m_szShaderIncludeText, sizeof(m_szShaderIncludeText) - 4, false);
  if (!bSuccess) return false;
  StripComments(m_szShaderIncludeText);
  m_nShaderIncludeTextLen = lstrlen(m_szShaderIncludeText);
  bSuccess |= ReadFileToString(L"data\\warp_vs.fx", m_szDefaultWarpVShaderText, sizeof(m_szDefaultWarpVShaderText), true);
  if (!bSuccess) return false;
  bSuccess |= ReadFileToString(L"data\\warp_ps.fx", m_szDefaultWarpPShaderText, sizeof(m_szDefaultWarpPShaderText), true);
  if (!bSuccess) return false;
  bSuccess |= ReadFileToString(L"data\\comp_vs.fx", m_szDefaultCompVShaderText, sizeof(m_szDefaultCompVShaderText), true);
  if (!bSuccess) return false;
  bSuccess |= ReadFileToString(L"data\\comp_ps.fx", m_szDefaultCompPShaderText, sizeof(m_szDefaultCompPShaderText), true);
  if (!bSuccess) return false;
  bSuccess |= ReadFileToString(L"data\\blur_vs.fx", m_szBlurVS, sizeof(m_szBlurVS), true);
  if (!bSuccess) return false;
  bSuccess |= ReadFileToString(L"data\\blur1_ps.fx", m_szBlurPSX, sizeof(m_szBlurPSX), true);
  if (!bSuccess) return false;
  bSuccess |= ReadFileToString(L"data\\blur2_ps.fx", m_szBlurPSY, sizeof(m_szBlurPSY), true);
  if (!bSuccess) return false;

  BuildMenus();

  m_bMMX = CheckForMMX();
  //m_bSSE = CheckForSSE();

  m_pState->Default();
  m_pOldState->Default();
  m_pNewState->Default();

  //LoadRandomPreset(0.0f);   -avoid this here; causes some DX9 stuff to happen.

  return true;
}

//----------------------------------------------------------------------

void CancelThread(int max_wait_time_ms) {
  g_bThreadShouldQuit = true;
  int waited = 0;
  while (g_bThreadAlive && waited < max_wait_time_ms) {
    Sleep(30);
    waited += 30;
  }

  if (g_bThreadAlive) {
    TerminateThread(g_hThread, 0);
    g_bThreadAlive = false;
  }

  if (g_hThread != INVALID_HANDLE_VALUE)
    CloseHandle(g_hThread);
  g_hThread = INVALID_HANDLE_VALUE;
}

void Engine::CleanUpMyNonDx9Stuff() {
  // This gets called only once, when your plugin exits.
  // Be sure to clean up any objects here that were
  //   created/initialized in AllocateMyNonDx9Stuff.

  // Close settings window and tool windows if open
  CloseSettingsWindow();
  CloseDisplaysWindow();
  CloseSongInfoWindow();
  CloseHotkeysWindow();
  CloseMidiWindow();
  CloseBoardWindow();
  ClosePresetsWindow();
  CloseSpritesWindow();
  CloseMessagesWindow();
  CloseMidiDevice();

  // Join any in-flight preset load thread
  if (m_presetLoadThread.joinable())
    m_presetLoadThread.join();

// =========================================================
// Display outputs cleanup
  DestroyAllDisplayOutputs();

// SPOUT cleanup on exit (legacy)
  SpoutReleaseWraps();
  spoutsender.CloseDirectX12();

  // If Spout output or DirectX mode has been changed, save the config file
  // so it is started in the selected mode the next time
  if (bSpoutChanged) MyWriteConfig();
  // =========================================================

  //sound.Finish();

    // NOTE: DO NOT DELETE m_gdi_titlefont_doublesize HERE!!!

  DeleteCriticalSection(&g_cs);

  CancelThread(1000);

  m_menuPreset.Finish();
  m_menuWave.Finish();
  m_menuAugment.Finish();
  m_menuCustomWave.Finish();
  m_menuCustomShape.Finish();
  m_menuMotion.Finish();
  m_menuPost.Finish();
  for (int i = 0; i < MAX_CUSTOM_WAVES; i++)
    m_menuWavecode[i].Finish();
  for (int i = 0; i < MAX_CUSTOM_SHAPES; i++)
    m_menuShapecode[i].Finish();

  //dumpmsg("Finish: cleanup complete.");
}

//----------------------------------------------------------------------

float SquishToCenter(float x, float fExp) {
  if (x > 0.5f)
    return powf(x * 2 - 1, fExp) * 0.5f + 0.5f;

  return (1 - powf(1 - x * 2, fExp)) * 0.5f;
}

int GetNearestPow2Size(int w, int h) {
  float fExp = logf(max(w, h) * 0.75f + 0.25f * min(w, h)) / logf(2.0f);
  float bias = 0.55f;
  if (fExp + bias >= 11.0f)   // ..don't jump to 2048x2048 quite as readily
    bias = 0.5f;
  int   nExp = (int)(fExp + bias);
  int log2size = (int)powf(2.0f, (float)nExp);
  return log2size;
}

int Engine::AllocateMyDX9Stuff() {
  // (...aka OnUserResizeWindow)
  // (...aka OnToggleFullscreen)

  // Allocate and initialize all your DX9 and D3DX stuff here: textures,
  //   surfaces, vertex/index buffers, D3DX fonts, and so on.
  // If anything fails here, return FALSE to safely exit the plugin,
  //   but only after displaying a messagebox giving the user some information
  //   about what went wrong.  If the error is NON-CRITICAL, you don't *have*
  //   to return; just make sure that the rest of the code will be still safely
  //   run (albeit with degraded features).
  // If you run out of video memory, you might want to show a short messagebox
  //   saying what failed to allocate and that the reason is a lack of video
  //   memory, and then call SuggestHowToFreeSomeMem(), which will show them
  //   a *second* messagebox that (intelligently) suggests how they can free up
  //   some video memory.
  // Don't forget to account for each object you create/allocate here by cleaning
  //   it up in CleanUpMyDX9Stuff!
  // IMPORTANT:
  //   Note that the code here isn't just run at program startup!
  //   When the user toggles between fullscreen and windowed modes
  //   or resizes the window, the base class calls this function before
  //   destroying & recreating the plugin window and DirectX object, and then
  //   calls AllocateMyDX9Stuff afterwards, to get your plugin running again.

  wchar_t buf[32768], title[64];

  m_nFramesSinceResize = 0;

  int nNewCanvasStretch = (m_nCanvasStretch == 0) ? 100 : m_nCanvasStretch;

  DWORD PSVersion = GetCaps()->PixelShaderVersion & 0xFFFF;  // 0x0300, etc.
  if (PSVersion >= 0x0300)
    m_nMaxPSVersion_DX9 = MD2_PS_3_0;
  else if (PSVersion > 0x0200)
    m_nMaxPSVersion_DX9 = MD2_PS_2_X;
  else if (PSVersion >= 0x0200)
    m_nMaxPSVersion_DX9 = MD2_PS_2_0;
  else
    m_nMaxPSVersion_DX9 = MD2_PS_NONE;

  if (m_nMaxPSVersion_ConfigPanel == -1)
    m_nMaxPSVersion = m_nMaxPSVersion_DX9;
  else {
    // to still limit their choice by what HW reports:
    //m_nMaxPSVersion = min(m_nMaxPSVersion_DX9, m_nMaxPSVersion_ConfigPanel);

    // to allow them to override:
    m_nMaxPSVersion = m_nMaxPSVersion_ConfigPanel;
  }

  /*
     Auto mode: do a check against a few known, *SLOW* DX9/ps_2_0 cards to see
      if we should run them without pixel shaders instead.
     Here is valve's list of the cards they run DX8 on (mostly because they're too slow under DX9 + ps_2_0):
          NVIDIA GeForce FX 5200  31.12%
          ATI Radeon 9200         21.29%
          NVIDIA GeForce FX 5500  11.27%
          NVIDIA GeForce4          7.74%
          NVIDIA GeForce FX 5700   7.12%
          NVIDIA GeForce FX 5600   5.16%
          SiS 661FX_760_741        3.34%
          NVIDIA GeForce FX 5900   3.24%
          NVIDIA GeForce3          2.09%
          ATI Radeon 9000          1.98%
          other                    5.66%
          [ from http://www.steampowered.com/status/survey.html ]
          see also:
              http://en.wikipedia.org/wiki/Radeon
              http://en.wikipedia.org/wiki/Geforce_fx
  */

  const char* szGPU = GetDriverDescription();
  /* known examples of this string:
      "ATI MOBILITY RADEON X600"
      "RADEON X800 Series   "     <- note the spaces at the end
      "Sapphire RADEON X700"
      "NVIDIA GeForce Go 6200  "  <- note the spaces at the end
      "NVIDIA GeForce 6800 GT"
      "Intel(R) 82865G Graphics Controller"
      "Mobile Intel(R) 915GM/GMS,910GML Express Chipset Family"

  might want to consider adding these to the list: [from http://www.intel.com/support/graphics/sb/cs-014257.htm ]
    (...they should support PS2.0, but not sure if they're fast...)
      "Mobile Intel(R) 945GM Express Chipset Family"
      "Mobile Intel(R) 915GM/GMS,910GML Express Chipset"
      "Intel(R) 945G Express Chipset"
      "Intel(R) 82915G/82910GL Express Chipset Family"

  or these, if they seem to be reporting that they do support ps_2_0, which would be very bogus info:
      "Intel(R) 82865G Graphics Controller"
      "Intel(R) 82852/82855 Graphics Controller Family"
      "Intel(R) 82845G Graphics Controller"
      "Intel(R) 82830M Graphics Controller"
      "Intel(R) 82815 Graphics Controller"
      "Intel(R) 82810 Graphics Controller"
  */

  // GREY LIST - slow ps_2_0 cards
  // In Canvas Stretch==Auto mode, for these cards, if they (claim to) run ps_2_0,
  //   we run at half-res (cuz they're slow).
  // THE GENERAL GUIDELINE HERE:
  //   It should be at least as fast as a GeForce FX 5700 or my GeForce 6200 (TC)
  //   if it's to run without stretch.
  if (m_nCanvasStretch == 0)// && m_nMaxPSVersion_DX9 > 0)
  {
    // put cards on this list if you see them successfully run ps_2_0 (using override)
    // and they run well at a low resolution (512x512 or less).
    if (
      strstr(szGPU, "GeForce 4") ||    // probably not even ps_2_0
      strstr(szGPU, "GeForce FX 52") ||    // chip's computer (FX 5200) - does do ps_2_0, but slow
      strstr(szGPU, "GeForce FX 53") ||
      strstr(szGPU, "GeForce FX 54") ||
      strstr(szGPU, "GeForce FX 55") ||   //GeForce FX 5600 is 13 GB/s - 2.5x as fast as my 6200!
      strstr(szGPU, "GeForce FX 56") ||
      //...GeForce FX 5700 and up, we let those run at full-res on ps_2_0...
      strstr(szGPU, "GeForce FX 56") ||
      strstr(szGPU, "GeForce FX 56") ||
      strstr(szGPU, "SiS 300/305/630/540/730") ||    // mom's computer - just slow.
      strstr(szGPU, "Radeon 8") ||    // no shader model 2.
      strstr(szGPU, "Radeon 90") ||    // from Valve.  no shader model 2.
      strstr(szGPU, "Radeon 91") ||    // no shader model 2.
      strstr(szGPU, "Radeon 92") ||    // from Valve.  no shader model 2.
      strstr(szGPU, "Radeon 93") ||    // no shader model 2.
      strstr(szGPU, "Radeon 94") ||    // no shader model 2.
      // guessing that 9500+ are ok - they're all ps_2_0 and the 9600 is like an FX 5900.
      strstr(szGPU, "Radeon 9550") ||  // *maybe* - kiv - super budget R200 chip.  def. ps_2_0 but possibly very slow.
      strstr(szGPU, "Radeon X300") ||  // *maybe* - kiv - super budget R200 chip   def. ps_2_0 but possibly very slow.
      0) {
      nNewCanvasStretch = 200;
    }
  }

  /*                           pix pipes
                             core    Fill(G)  membw(GB/s)
      Radeon 9600 Pro	        400	 4	1.6	     9.6
      Radeon 9600 XT	        500	 4	2.0	     9.6
      GeForce FX 5600 Ultra	400	 4	1.6	    12.8
      GeForce FX 5700 Ultra	475	 4	1.9	    14.4
      GeForce FX 5900 XT	    400	 4	1.6	    22.4
      GeForce FX 5900	        450	 4	1.8	    27.2
      GeForce FX 5950 Ultra 	475  4  2.9     30
      GeForce 6200 TC-32 	    350  4  1.1      5.6 (TurboDonkey)
      GeForce 6600 GT 	    500  8  2.0     16
      GeForce 6800 Ultra 	    400 16  6.4     35
      ATI Radeon X800 XT PE 	520 16  8.3     36
      ATI Radeon X850 XT PE   540 16  8.6     38

      Entry-level GPU 	5200, 5300, 5500
      Mid-Range GPU 	    5600, 5700
      High-end GPU 	    5800, 5900, 5950

      Entry-level GPU 	6200, 6500
      Mid-Range GPU 	    6600
      High-end GPU 	    6800

      Entry-level GPU
      Mid-Range GPU
      High-end GPU

      R200: only ps_1_4.  Radeon 8500-9250.
      R300: ps_2_0.       Radeon 9500-9800, X300-X600, X1050.  9600 fast enough (~FX5900).  9500/9700 ~ GeForce 4 Ti.
      R420: ps_2_0        Radeon X700-8750 - ALL FAST ENOUGH.  X700 is same speed as a GeForce 6600.

      6600		    ~	X700
      GeForce 4		<	X300 / X600 / 9600
      GeForce 4 Ti	>	Radeon 8500
      FX 5900		    =	Radeon 9600
      FX 5900 Ultra	<< [half]	Radeon 9800 Pro
      GeForce FX		<	Radeon 9700/9800
  */

  // BLACK LIST
  // In Pixel Shaders==Auto mode, for these cards, we avoid ps_2_0 completely.
  // There shouldn't be much on this list... feel free to put anything you KNOW doesn't do ps_2_0 (why not),
  // and to put anything that is slow to begin with, and HAS BUGGY DRIVERS (INTEL).
  if (m_nMaxPSVersion_ConfigPanel == -1) {
    if (strstr(szGPU, "GeForce2") ||    // from Valve
      strstr(szGPU, "GeForce3") ||    // from Valve
      strstr(szGPU, "GeForce4") ||    // from Valve
      strstr(szGPU, "Radeon 7") ||    // from Valve
      strstr(szGPU, "Radeon 8") ||
      strstr(szGPU, "SiS 661FX_760_741") ||    // from Valve
      //FOR NOW, FOR THESE, ASSUME INTEL EITHER DOESN'T DO PS_2_0,
      //OR DRIVERS SUCK AND IT WOULDN'T WORK ANYWAY!
      (strstr(szGPU, "Intel") && strstr(szGPU, "945G")) ||
      (strstr(szGPU, "Intel") && strstr(szGPU, "915G")) ||  // ben allison's laptop - snow, freezing when you try ps_2_0
      (strstr(szGPU, "Intel") && strstr(szGPU, "910G")) ||
      (strstr(szGPU, "Intel") && strstr(szGPU, "8291")) ||     // gonna guess that this supports ps_2_0 but is SLOW
      (strstr(szGPU, "Intel") && strstr(szGPU, "8281")) ||     // definitely DOESN'T support pixel shaders
      (strstr(szGPU, "Intel") && strstr(szGPU, "8283")) ||     // definitely DOESN'T support pixel shaders
      (strstr(szGPU, "Intel") && strstr(szGPU, "8284")) ||     // definitely DOESN'T support pixel shaders
      (strstr(szGPU, "Intel") && strstr(szGPU, "8285")) ||     // definitely DOESN'T support pixel shaders
      (strstr(szGPU, "Intel") && strstr(szGPU, "8286")) ||     // definitely DOESN'T support pixel shaders.  Ben Allison's desktop (865) - no image w/ps_2_0.  Plus Nes's desktop - no ps_2_0.
      0) {
      m_nMaxPSVersion = MD2_PS_NONE;
      //if (m_nCanvasStretch==0)
      //    nNewCanvasStretch = 100;
    }
  }

  /*char fname[512];
  sprintf(fname, "%s%s", GetPluginsDirPath(), TEXTURE_NAME);
  if (D3DXCreateTextureFromFile(GetDevice(), fname, &m_object_tex) != S_OK)
  {
      // just give a warning, and move on
      m_object_tex = NULL;    // (make sure pointer wasn't mangled by some bad driver)

      char msg[1024];
      sprintf(msg, "Unable to load texture:\r%s", fname);
      MessageBox(GetPluginWindow(), msg, "WARNING", MB_OK|MB_SETFOREGROUND|MB_TOPMOST);
      //return false;
  }*/

  // Note: this code used to be in OnResizeGraphicsWindow().

  // SHADERS
  //-------------------------------------
  if (m_nMaxPSVersion > MD2_PS_NONE) {
    // Create vertex declarations (DX9 only — skipped when no DX9 device)
    if (GetDevice()) {
      if (D3D_OK != GetDevice()->CreateVertexDeclaration(g_MyVertDecl, &m_pMyVertDecl)) {
        wasabiApiLangString(IDS_COULD_NOT_CREATE_MY_VERTEX_DECLARATION, buf, sizeof(buf));
        dumpmsg(buf);
        MessageBoxW(GetPluginWindow(), buf, wasabiApiLangString(IDS_MILKDROP_ERROR, title, sizeof(title)), MB_OK | MB_SETFOREGROUND | MB_TOPMOST);
        return false;
      }
      if (D3D_OK != GetDevice()->CreateVertexDeclaration(g_WfVertDecl, &m_pWfVertDecl)) {
        wasabiApiLangString(IDS_COULD_NOT_CREATE_WF_VERTEX_DECLARATION, buf, sizeof(buf));
        dumpmsg(buf);
        MessageBoxW(GetPluginWindow(), buf, wasabiApiLangString(IDS_MILKDROP_ERROR, title, sizeof(title)), MB_OK | MB_SETFOREGROUND | MB_TOPMOST);
        return false;
      }
      if (D3D_OK != GetDevice()->CreateVertexDeclaration(g_SpriteVertDecl, &m_pSpriteVertDecl)) {
        wasabiApiLangString(IDS_COULD_NOT_CREATE_SPRITE_VERTEX_DECLARATION, buf, sizeof(buf));
        dumpmsg(buf);
        MessageBoxW(GetPluginWindow(), buf, wasabiApiLangString(IDS_MILKDROP_ERROR, title, sizeof(title)), MB_OK | MB_SETFOREGROUND | MB_TOPMOST);
        return false;
      }
    }

    // Load the FALLBACK shaders...
    int PSVersion = 2;
    if (!RecompilePShader(m_szDefaultWarpPShaderText, &m_fallbackShaders_ps.warp, SHADER_WARP, true, PSVersion, false)) {
      wchar_t szSM[64];
      switch (m_nMaxPSVersion_DX9) {
      case MD2_PS_2_0:
      case MD2_PS_2_X:
        wasabiApiLangString(IDS_SHADER_MODEL_2, szSM, 64); break;
      case MD2_PS_3_0: wasabiApiLangString(IDS_SHADER_MODEL_3, szSM, 64); break;
      case MD2_PS_4_0: wasabiApiLangString(IDS_SHADER_MODEL_4, szSM, 64); break;
      default:
        swprintf(szSM, wasabiApiLangString(IDS_UKNOWN_CASE_X), m_nMaxPSVersion_DX9);
        break;
      }
      if (m_nMaxPSVersion_ConfigPanel >= MD2_PS_NONE && m_nMaxPSVersion_DX9 < m_nMaxPSVersion_ConfigPanel)
        swprintf(buf, wasabiApiLangString(IDS_FAILED_TO_COMPILE_PIXEL_SHADERS_USING_X), szSM, PSVersion);
      else
        swprintf(buf, wasabiApiLangString(IDS_FAILED_TO_COMPILE_PIXEL_SHADERS_HARDWARE_MIS_REPORT), szSM, PSVersion);
      dumpmsg(buf);
      MessageBoxW(GetPluginWindow(), buf, wasabiApiLangString(IDS_MILKDROP_ERROR, title, 64), MB_OK | MB_SETFOREGROUND | MB_TOPMOST);
      return false;
    }
    if (!RecompileVShader(m_szDefaultWarpVShaderText, &m_fallbackShaders_vs.warp, SHADER_WARP, true, false)) {
      wasabiApiLangString(IDS_COULD_NOT_COMPILE_FALLBACK_WV_SHADER, buf, sizeof(buf));
      dumpmsg(buf);
      MessageBoxW(GetPluginWindow(), buf, wasabiApiLangString(IDS_MILKDROP_ERROR, title, 64), MB_OK | MB_SETFOREGROUND | MB_TOPMOST);
      return false;
    }
    if (!RecompileVShader(m_szDefaultCompVShaderText, &m_fallbackShaders_vs.comp, SHADER_COMP, true, false)) {
      wasabiApiLangString(IDS_COULD_NOT_COMPILE_FALLBACK_CV_SHADER, buf, sizeof(buf));
      dumpmsg(buf);
      MessageBoxW(GetPluginWindow(), buf, wasabiApiLangString(IDS_MILKDROP_ERROR, title, 64), MB_OK | MB_SETFOREGROUND | MB_TOPMOST);
      return false;
    }

    if (!RecompilePShader(m_szDefaultCompPShaderText, &m_fallbackShaders_ps.comp, SHADER_COMP, true, PSVersion, false)) {
      wasabiApiLangString(IDS_COULD_NOT_COMPILE_FALLBACK_CP_SHADER, buf, sizeof(buf));
      dumpmsg(buf);
      MessageBoxW(GetPluginWindow(), buf, wasabiApiLangString(IDS_MILKDROP_ERROR, title, 64), MB_OK | MB_SETFOREGROUND | MB_TOPMOST);
      return false;
    }

    // DX12: Create fallback PSOs from compiled fallback shader bytecodes
    if (m_lpDX && m_lpDX->m_device.Get() && m_lpDX->m_rootSignature.Get()) {
      ID3D12Device* dev = m_lpDX->m_device.Get();
      ID3D12RootSignature* rs = m_lpDX->m_rootSignature.Get();
      DXGI_FORMAT fmt = DXGI_FORMAT_R8G8B8A8_UNORM;

      if (m_fallbackShaders_ps.warp.bytecodeBlob && g_pWarpVSBlob) {
        m_dx12FallbackWarpPSO = DX12CreatePresetPSO(
          dev, rs, fmt, g_pWarpVSBlob,
          m_fallbackShaders_ps.warp.bytecodeBlob->GetBufferPointer(),
          (UINT)m_fallbackShaders_ps.warp.bytecodeBlob->GetBufferSize(),
          g_MyVertexLayout, _countof(g_MyVertexLayout), false);
        DebugLogA(m_dx12FallbackWarpPSO ? "DX12: Fallback warp PSO created" : "DX12: Fallback warp PSO FAILED");
      }

      if (m_fallbackShaders_ps.comp.bytecodeBlob && g_pCompVSBlob) {
        m_dx12FallbackCompPSO = DX12CreatePresetPSO(
          dev, rs, fmt, g_pCompVSBlob,
          m_fallbackShaders_ps.comp.bytecodeBlob->GetBufferPointer(),
          (UINT)m_fallbackShaders_ps.comp.bytecodeBlob->GetBufferSize(),
          g_MyVertexLayout, _countof(g_MyVertexLayout), false);
        DebugLogA(m_dx12FallbackCompPSO ? "DX12: Fallback comp PSO created" : "DX12: Fallback comp PSO FAILED");
      }
    }

    // Load the BLUR shaders...
    if (!RecompileVShader(m_szBlurVS, &m_BlurShaders[0].vs, SHADER_BLUR, true, false)) {
      wasabiApiLangString(IDS_COULD_NOT_COMPILE_BLUR1_VERTEX_SHADER, buf, sizeof(buf));
      dumpmsg(buf);
      MessageBoxW(GetPluginWindow(), buf, wasabiApiLangString(IDS_MILKDROP_ERROR, title, 64), MB_OK | MB_SETFOREGROUND | MB_TOPMOST);
      return false;
    }

    if (!RecompilePShader(m_szBlurPSX, &m_BlurShaders[0].ps, SHADER_BLUR, true, PSVersion, false)) {
      wasabiApiLangString(IDS_COULD_NOT_COMPILE_BLUR1_PIXEL_SHADER, buf, sizeof(buf));
      dumpmsg(buf);
      MessageBoxW(GetPluginWindow(), buf, wasabiApiLangString(IDS_MILKDROP_ERROR, title, 64), MB_OK | MB_SETFOREGROUND | MB_TOPMOST);
      return false;
    }
    if (!RecompileVShader(m_szBlurVS, &m_BlurShaders[1].vs, SHADER_BLUR, true, false)) {
      wasabiApiLangString(IDS_COULD_NOT_COMPILE_BLUR2_VERTEX_SHADER, buf, sizeof(buf));
      dumpmsg(buf);
      MessageBoxW(GetPluginWindow(), buf, wasabiApiLangString(IDS_MILKDROP_ERROR, title, 64), MB_OK | MB_SETFOREGROUND | MB_TOPMOST);
      return false;
    }

    if (!RecompilePShader(m_szBlurPSY, &m_BlurShaders[1].ps, SHADER_BLUR, true, PSVersion, false)) {
      wasabiApiLangString(IDS_COULD_NOT_COMPILE_BLUR2_PIXEL_SHADER, buf, sizeof(buf));
      dumpmsg(buf);
      MessageBoxW(GetPluginWindow(), buf, wasabiApiLangString(IDS_MILKDROP_ERROR, title, 64), MB_OK | MB_SETFOREGROUND | MB_TOPMOST);
      return false;
    }

    // DX12: Create blur PSOs from compiled blur shader bytecodes
    // Use blur root signature (s0 = CLAMP) because SM5.0 assigns the blur
    // shader's single sampler to s0, and blur passes require CLAMP addressing.
    if (m_lpDX && m_lpDX->m_device.Get() && m_lpDX->m_blurRootSignature.Get() && g_pBlurVSBlob) {
      ID3D12Device* dev = m_lpDX->m_device.Get();
      ID3D12RootSignature* rs = m_lpDX->m_blurRootSignature.Get();
      DXGI_FORMAT fmt = DXGI_FORMAT_R8G8B8A8_UNORM;
      for (int bi = 0; bi < 2; bi++) {
        m_dx12BlurPSO[bi].Reset();
        if (m_BlurShaders[bi].ps.bytecodeBlob) {
          m_dx12BlurPSO[bi] = DX12CreatePresetPSO(
            dev, rs, fmt, g_pBlurVSBlob,
            m_BlurShaders[bi].ps.bytecodeBlob->GetBufferPointer(),
            (UINT)m_BlurShaders[bi].ps.bytecodeBlob->GetBufferSize(),
            g_MyVertexLayout, _countof(g_MyVertexLayout), false);
          char dbg[128];
          sprintf(dbg, "DX12: Blur PSO[%d] %s", bi, m_dx12BlurPSO[bi] ? "created" : "FAILED");
          DebugLogA(dbg);
        }
      }
    }
  }

  // create m_lpVS[2]
  {
    int log2texsize = GetNearestPow2Size(GetWidth(), GetHeight());

    // auto-guess texsize
    if (m_bTexSizeWasAutoExact) {
      // note: in windowed mode, the winamp window could be weird sizes,
      //        so the plugin shell now gives us a slightly enlarged size,
      //        which pads it out to the nearest 32x32 block size,
      //        and then on display, it intelligently crops the image.
      //       This is pretty likely to work on non-shitty GPUs.
      //        but some shitty ones will still only do powers of 2!
      //       So if we are running out of video memory here or experience
      //        other problems, though, we can make our VS's smaller;
      //        which will work, although it will lead to stretching.
      m_nTexSizeX = GetWidth();
      m_nTexSizeY = GetHeight();
    }
    else if (m_bTexSizeWasAutoPow2) {
      m_nTexSizeX = log2texsize;
      m_nTexSizeY = log2texsize;
    }

    // clip texsize by max. from caps
    if ((DWORD)m_nTexSizeX > GetCaps()->MaxTextureWidth && GetCaps()->MaxTextureWidth > 0)
      m_nTexSizeX = GetCaps()->MaxTextureWidth;
    if ((DWORD)m_nTexSizeY > GetCaps()->MaxTextureHeight && GetCaps()->MaxTextureHeight > 0)
      m_nTexSizeY = GetCaps()->MaxTextureHeight;

    // apply canvas stretch
    m_nTexSizeX = (m_nTexSizeX * 100) / nNewCanvasStretch;
    m_nTexSizeY = (m_nTexSizeY * 100) / nNewCanvasStretch;

    // re-compute closest power-of-2 size, now that we've factored in the stretching...
    log2texsize = GetNearestPow2Size(m_nTexSizeX, m_nTexSizeY);
    if (m_bTexSizeWasAutoPow2) {
      m_nTexSizeX = log2texsize;
      m_nTexSizeY = log2texsize;
    }

    // snap to 16x16 blocks
    m_nTexSizeX = ((m_nTexSizeX + 15) / 16) * 16;
    m_nTexSizeY = ((m_nTexSizeY + 15) / 16) * 16;

    // determine format for VS1/VS2
    D3DFORMAT fmt;
    switch (m_nTexBitsPerCh) {
    case 5:  fmt = D3DFMT_R5G6B5; break;
    case 8:  fmt = D3DFMT_X8R8G8B8; break;
    case 10: fmt = D3DFMT_A2R10G10B10; break;  // D3DFMT_A2W10V10U10 or D3DFMT_A2R10G10B10 or D3DFMT_A2B10G10R10
    case 16: fmt = D3DFMT_A16B16G16R16F; break;
    case 32: fmt = D3DFMT_A32B32G32R32F; break; //FIXME
    default: fmt = D3DFMT_X8R8G8B8; break;
    }

    // reallocate
    bool bSuccess = false;
    DWORD vs_flags = D3DUSAGE_RENDERTARGET;// | D3DUSAGE_AUTOGENMIPMAP;//FIXME! (make automipgen optional)
    bool bRevertedBitDepth = false;
    if (!GetDevice()) {
      // DX9 device not available (DX12 migration) — skip DX9 texture creation.
      // DX12 render targets are created below.
      bSuccess = true;
    }
    else do {
      SafeRelease(m_lpVS[0]);
      SafeRelease(m_lpVS[1]);

      // create VS1
      bSuccess = (GetDevice()->CreateTexture(m_nTexSizeX, m_nTexSizeY, 1, vs_flags, fmt, D3DPOOL_DEFAULT, &m_lpVS[0], NULL) == D3D_OK);
      if (!bSuccess) {
        bSuccess = (GetDevice()->CreateTexture(m_nTexSizeX, m_nTexSizeY, 1, vs_flags, GetBackBufFormat(), D3DPOOL_DEFAULT, &m_lpVS[0], NULL) == D3D_OK);
        if (bSuccess)
          fmt = GetBackBufFormat();
      }

      // create VS2
      if (bSuccess)
        bSuccess = (GetDevice()->CreateTexture(m_nTexSizeX, m_nTexSizeY, 1, vs_flags, fmt, D3DPOOL_DEFAULT, &m_lpVS[1], NULL) == D3D_OK);

      if (!bSuccess) {
        if (m_bTexSizeWasAutoExact) {
          if (m_nTexSizeX > 256 || m_nTexSizeY > 256) {
            m_nTexSizeX /= 2;
            m_nTexSizeY /= 2;
            m_nTexSizeX = ((m_nTexSizeX + 15) / 16) * 16;
            m_nTexSizeY = ((m_nTexSizeY + 15) / 16) * 16;
          }
          else {
            m_nTexSizeX = log2texsize;
            m_nTexSizeY = log2texsize;
            m_bTexSizeWasAutoExact = false;
            m_bTexSizeWasAutoPow2 = true;
          }
        }
        else if (m_bTexSizeWasAutoPow2) {
          if (m_nTexSizeX > 256) {
            m_nTexSizeX /= 2;
            m_nTexSizeY /= 2;
          }
          else
            break;
        }
      }
    } while (!bSuccess);// && m_nTexSizeX >= 256 && (m_bTexSizeWasAutoExact || m_bTexSizeWasAutoPow2));

    if (!bSuccess) {
      wchar_t buf[2048];
      UINT err_id = IDS_COULD_NOT_CREATE_INTERNAL_CANVAS_TEXTURE_NOT_ENOUGH_VID_MEM;

      if (!(m_bTexSizeWasAutoExact || m_bTexSizeWasAutoPow2))
        err_id = IDS_COULD_NOT_CREATE_INTERNAL_CANVAS_TEXTURE_NOT_ENOUGH_VID_MEM_RECOMMENDATION;

      wasabiApiLangString(err_id, buf, sizeof(buf));
      dumpmsg(buf);
      MessageBoxW(GetPluginWindow(), buf, wasabiApiLangString(IDS_MILKDROP_ERROR, title, 64), MB_OK | MB_SETFOREGROUND | MB_TOPMOST);
      return false;
    }
    else {
      swprintf(buf, wasabiApiLangString(IDS_SUCCESSFULLY_CREATED_VS0_VS1), m_nTexSizeX, m_nTexSizeY, GetWidth(), GetHeight());
      dumpmsg(buf);
    }

    /*
      if (m_nTexSizeX != GetWidth() || m_nTexSizeY != GetHeight())
    {
          char buf[2048];
      sprintf(buf, "warning - canvas size adjusted from %dx%d to %dx%d.", GetWidth(), GetHeight(), m_nTexSizeX, m_nTexSizeY);
      dumpmsg(buf);
          AddError(buf, 3.2f, ERR_INIT, true);
    }/**/

    // create blur textures w/same format.  A complete mip chain costs 33% more video mem then 1 full-sized VS.
#if (NUM_BLUR_TEX>0)
    {
      int w = m_nTexSizeX;
      int h = m_nTexSizeY;
      for (int i = 0; i < NUM_BLUR_TEX; i++) {
        if (!(i & 1) || (i < 2)) {
          w = max(16, w / 2);
          h = max(16, h / 2);
        }
        m_nBlurTexW[i] = ((w + 3) / 16) * 16;
        m_nBlurTexH[i] = ((h + 3) / 4) * 4;
      }
    }

    if (GetDevice()) {
      DWORD blurtex_flags = D3DUSAGE_RENDERTARGET;
      for (int i = 0; i < NUM_BLUR_TEX; i++) {
        int w2 = m_nBlurTexW[i];
        int h2 = m_nBlurTexH[i];
        bSuccess = (GetDevice()->CreateTexture(w2, h2, 1, blurtex_flags, fmt, D3DPOOL_DEFAULT, &m_lpBlur[i], NULL) == D3D_OK);
        if (!bSuccess) {
          m_nBlurTexW[i] = 1;
          m_nBlurTexH[i] = 1;
          MessageBoxW(GetPluginWindow(), wasabiApiLangString(IDS_ERROR_CREATING_BLUR_TEXTURES, buf, sizeof(buf)),
            wasabiApiLangString(IDS_MILKDROP_WARNING, title, sizeof(title)), MB_OK | MB_SETFOREGROUND | MB_TOPMOST);
          break;
        }

        // add it to m_textures[].
        TexInfo x;
        swprintf(x.texname, L"blur%d%s", i / 2 + 1, (i % 2) ? L"" : L"doNOTuseME");
        x.texptr = m_lpBlur[i];
        x.w = w2;
        x.h = h2;
        x.d = 1;
        x.bEvictable = false;
        x.nAge = m_nPresetsLoadedTotal;
        x.nSizeInBytes = 0;
        m_textures.push_back(x);
      }
    }
#endif
  }

  // DX12 render targets — create alongside the (now-inert) DX9 textures above
  if (m_lpDX && m_lpDX->m_device) {
    m_dx12VS[0] = m_lpDX->CreateRenderTargetTexture(m_nTexSizeX, m_nTexSizeY, DXGI_FORMAT_R8G8B8A8_UNORM);
    m_dx12VS[1] = m_lpDX->CreateRenderTargetTexture(m_nTexSizeX, m_nTexSizeY, DXGI_FORMAT_R8G8B8A8_UNORM);
    // No binding blocks needed — per-frame bindings (UpdatePerFrameBindings) handle VS textures

#if (NUM_BLUR_TEX > 0)
    for (int i = 0; i < NUM_BLUR_TEX; i++) {
      int bw = m_nBlurTexW[i] > 0 ? m_nBlurTexW[i] : 16;
      int bh = m_nBlurTexH[i] > 0 ? m_nBlurTexH[i] : 16;
      m_dx12Blur[i] = m_lpDX->CreateRenderTargetTexture(bw, bh, DXGI_FORMAT_R8G8B8A8_UNORM);
    }
#endif

    // Inject effect intermediate texture — must match back buffer dimensions for CopyResource
    {
      UINT bbW = (UINT)max(1, m_lpDX->m_backbuffer_width);
      UINT bbH = (UINT)max(1, m_lpDX->m_backbuffer_height);
      m_injectEffectTex = m_lpDX->CreateRenderTargetTexture(bbW, bbH, DXGI_FORMAT_R8G8B8A8_UNORM);
      m_lpDX->CreateBindingBlockForTexture(m_injectEffectTex);
      DebugLogA(m_injectEffectTex.IsValid() ? "DX12: Inject effect texture: created" : "DX12: Inject effect texture: FAILED");
    }

    // Feedback buffers for Shadertoy temporal reprojection (ping-pong pair, VS-resolution)
    // Must match m_nTexSizeX/Y because comp shader's texsize constant = VS size,
    // and Shadertoy shaders compute fragCoord / texelFetch using texsize.
    {
      UINT fbW = (UINT)max(1, m_nTexSizeX);
      UINT fbH = (UINT)max(1, m_nTexSizeY);
      m_dx12Feedback[0] = m_lpDX->CreateRenderTargetTexture(fbW, fbH, DXGI_FORMAT_R32G32B32A32_FLOAT);
      m_dx12Feedback[1] = m_lpDX->CreateRenderTargetTexture(fbW, fbH, DXGI_FORMAT_R32G32B32A32_FLOAT);
      // Binding blocks needed for the blit pass (feedback → backbuffer for display)
      if (m_dx12Feedback[0].IsValid()) m_lpDX->CreateBindingBlockForTexture(m_dx12Feedback[0]);
      if (m_dx12Feedback[1].IsValid()) m_lpDX->CreateBindingBlockForTexture(m_dx12Feedback[1]);
      m_nFeedbackIdx = 0;
      DebugLogA(m_dx12Feedback[0].IsValid() && m_dx12Feedback[1].IsValid()
                ? "DX12: Feedback buffers: created (ping-pong pair)"
                : "DX12: Feedback buffers: FAILED");
    }

    // Inject effect pixel shader PSO
    // mode.x = F11 inject effect (0=off, 1=brighten, 2=darken, 3=solarize, 4=invert)
    // mode.y = per-preset effect bitmask (bit0=brighten, bit1=darken, bit2=solarize, bit3=invert)
    //          Per-preset effects use DX9-compatible math (blend-state equivalent).
    if (m_lpDX->m_rootSignature.Get() && g_pBlurVSBlob) {
      static const char szInjectPS[] =
        "Texture2D<float4> tex : register(t0);\n"
        "SamplerState samp : register(s0);\n"
        "cbuffer cbInject : register(b0) { uint4 mode; }\n"
        "float4 main(float2 uv : TEXCOORD0) : SV_Target {\n"
        "    float4 ret = tex.Sample(samp, uv);\n"
        "    // Per-preset post-process effects (DX9 blend-state equivalent math)\n"
        "    if (mode.y & 1u) ret.rgb = ret.rgb * (2.0 - ret.rgb);\n"         // brighten = invert→square→invert
        "    if (mode.y & 2u) ret.rgb = ret.rgb * ret.rgb;\n"                  // darken = square
        "    if (mode.y & 4u) ret.rgb = ret.rgb * (1.0 - ret.rgb) * 2.0;\n"   // solarize = invdest + dest*dest
        "    if (mode.y & 8u) ret.rgb = 1.0 - ret.rgb;\n"                     // invert
        "    // F11 inject effect (global, user-toggled)\n"
        "    if (mode.x == 1u) ret.rgb = sqrt(max(ret.rgb, 0.0));\n"
        "    else if (mode.x == 2u) ret.rgb = ret.rgb * ret.rgb;\n"
        "    else if (mode.x == 3u) ret.rgb = ret.rgb * (1.0 - ret.rgb) * 4.0;\n"
        "    else if (mode.x == 4u) ret.rgb = 1.0 - ret.rgb;\n"
        "    return ret;\n"
        "}\n";
      ID3DBlob* psBlob = nullptr;
      ID3DBlob* pErrors = nullptr;
      HRESULT hr = D3DCompile(szInjectPS, strlen(szInjectPS), nullptr, nullptr, nullptr,
                              "main", "ps_5_0",
                              D3DCOMPILE_ENABLE_BACKWARDS_COMPATIBILITY, 0, &psBlob, &pErrors);
      if (FAILED(hr)) {
        if (pErrors) { DebugLogA((const char*)pErrors->GetBufferPointer(), LOG_ERROR); pErrors->Release(); }
        DebugLogA("DX12: Inject effect PSO: PS compile FAILED", LOG_ERROR);
      } else {
        if (pErrors) pErrors->Release();
        m_pInjectEffectPSO = DX12CreatePresetPSO(
          m_lpDX->m_device.Get(), m_lpDX->m_rootSignature.Get(),
          DXGI_FORMAT_R8G8B8A8_UNORM, g_pBlurVSBlob,
          psBlob->GetBufferPointer(), (UINT)psBlob->GetBufferSize(),
          g_MyVertexLayout, _countof(g_MyVertexLayout), false);
        psBlob->Release();
        DebugLogA(m_pInjectEffectPSO ? "DX12: Inject effect PSO: created" : "DX12: Inject effect PSO: create FAILED");
      }
    }

    // Spout video input luma-key PSO (alpha blended)
    CompileSpoutInputPSO();
  }

  m_fAspectX = (m_nTexSizeY > m_nTexSizeX) ? m_nTexSizeX / (float)m_nTexSizeY : 1.0f;
  m_fAspectY = (m_nTexSizeX > m_nTexSizeY) ? m_nTexSizeY / (float)m_nTexSizeX : 1.0f;
  m_fInvAspectX = 1.0f / m_fAspectX;
  m_fInvAspectY = 1.0f / m_fAspectY;


  // BUILD VERTEX LIST for final composite blit
//   note the +0.5-texel offset!
//   (otherwise, a 1-pixel-wide line of the image would wrap at the top and left edges).
  ZeroMemory(m_comp_verts, sizeof(MYVERTEX) * FCGSX * FCGSY);
  //float fOnePlusInvWidth  = 1.0f + 1.0f/(float)GetWidth();
  //float fOnePlusInvHeight = 1.0f + 1.0f/(float)GetHeight();
  // DX9 half-texel offset: needed because DX9 pixel centers are at integers.
  // DX12 pixel centers are at integer+0.5, so the offset is NOT needed and causes
  // sub-pixel sampling misalignment (universal blur with bilinear filtering).
  float fHalfTexelW = (m_lpDX && m_lpDX->m_device) ? 0.0f : 0.5f / (float)GetWidth();
  float fHalfTexelH = (m_lpDX && m_lpDX->m_device) ? 0.0f : 0.5f / (float)GetHeight();
  float fDivX = 1.0f / (float)(FCGSX - 2);
  float fDivY = 1.0f / (float)(FCGSY - 2);
  for (int j = 0; j < FCGSY; j++) {
    int j2 = j - j / (FCGSY / 2);
    float v = j2 * fDivY;
    v = SquishToCenter(v, 3.0f);
    float sy = -((v - fHalfTexelH) * 2 - 1);//fOnePlusInvHeight*v*2-1;
    for (int i = 0; i < FCGSX; i++) {
      int i2 = i - i / (FCGSX / 2);
      float u = i2 * fDivX;
      u = SquishToCenter(u, 3.0f);
      float sx = (u - fHalfTexelW) * 2 - 1;//fOnePlusInvWidth*u*2-1;
      MYVERTEX* p = &m_comp_verts[i + j * FCGSX];
      p->x = sx;
      p->y = sy;
      p->z = 0;
      float rad, ang;
      UvToMathSpace(u, v, &rad, &ang);
      // fix-ups:
      if (i == FCGSX / 2 - 1) {
        if (j < FCGSY / 2 - 1)
          ang = 3.1415926535898f * 1.5f;
        else if (j == FCGSY / 2 - 1)
          ang = 3.1415926535898f * 1.25f;
        else if (j == FCGSY / 2)
          ang = 3.1415926535898f * 0.75f;
        else
          ang = 3.1415926535898f * 0.5f;
      }
      else if (i == FCGSX / 2) {
        if (j < FCGSY / 2 - 1)
          ang = 3.1415926535898f * 1.5f;
        else if (j == FCGSY / 2 - 1)
          ang = 3.1415926535898f * 1.75f;
        else if (j == FCGSY / 2)
          ang = 3.1415926535898f * 0.25f;
        else
          ang = 3.1415926535898f * 0.5f;
      }
      else if (j == FCGSY / 2 - 1) {
        if (i < FCGSX / 2 - 1)
          ang = 3.1415926535898f * 1.0f;
        else if (i == FCGSX / 2 - 1)
          ang = 3.1415926535898f * 1.25f;
        else if (i == FCGSX / 2)
          ang = 3.1415926535898f * 1.75f;
        else
          ang = 3.1415926535898f * 2.0f;
      }
      else if (j == FCGSY / 2) {
        if (i < FCGSX / 2 - 1)
          ang = 3.1415926535898f * 1.0f;
        else if (i == FCGSX / 2 - 1)
          ang = 3.1415926535898f * 0.75f;
        else if (i == FCGSX / 2)
          ang = 3.1415926535898f * 0.25f;
        else
          ang = 3.1415926535898f * 0.0f;
      }
      p->tu = u;
      p->tv = v;
      //p->tu_orig = u;
      //p->tv_orig = v;
      p->rad = rad;
      p->ang = ang;
      p->Diffuse = 0xFFFFFFFF;
    }
  }

  // build index list for final composite blit -
  // order should be friendly for interpolation of 'ang' value!
  int* cur_index = &m_comp_indices[0];
  for (int y = 0; y < FCGSY - 1; y++) {
    if (y == FCGSY / 2 - 1)
      continue;
    for (int x = 0; x < FCGSX - 1; x++) {
      if (x == FCGSX / 2 - 1)
        continue;
      bool left_half = (x < FCGSX / 2);
      bool top_half = (y < FCGSY / 2);
      bool center_4 = ((x == FCGSX / 2 || x == FCGSX / 2 - 1) && (y == FCGSY / 2 || y == FCGSY / 2 - 1));

      if (((int)left_half + (int)top_half + (int)center_4) % 2) {
        *(cur_index + 0) = (y)*FCGSX + (x);
        *(cur_index + 1) = (y)*FCGSX + (x + 1);
        *(cur_index + 2) = (y + 1) * FCGSX + (x + 1);
        *(cur_index + 3) = (y + 1) * FCGSX + (x + 1);
        *(cur_index + 4) = (y + 1) * FCGSX + (x);
        *(cur_index + 5) = (y)*FCGSX + (x);
      }
      else {
        *(cur_index + 0) = (y + 1) * FCGSX + (x);
        *(cur_index + 1) = (y)*FCGSX + (x);
        *(cur_index + 2) = (y)*FCGSX + (x + 1);
        *(cur_index + 3) = (y)*FCGSX + (x + 1);
        *(cur_index + 4) = (y + 1) * FCGSX + (x + 1);
        *(cur_index + 5) = (y + 1) * FCGSX + (x);
      }
      cur_index += 6;
    }
  }

  // -----------------

/*if (m_bFixSlowText && !m_bSeparateTextWindow)
{
      if (D3DXCreateTexture(GetDevice(), GetWidth(), GetHeight(), 1, D3DUSAGE_RENDERTARGET, GetBackBufFormat(), D3DPOOL_DEFAULT, &m_lpDDSText) != D3D_OK)
  {
          char buf[2048];
    dumpmsg("Init: -WARNING-:");
    sprintf(buf, "WARNING: Not enough video memory to make a dedicated text surface; \rtext will still be drawn directly to the back buffer.\r\rTo avoid seeing this error again, uncheck the 'fix slow text' option.");
    dumpmsg(buf);
    if (!m_bWarningsDisabled)
      MessageBox(GetPluginWindow(), buf, "WARNING", MB_OK|MB_SETFOREGROUND|MB_TOPMOST );
    m_lpDDSText = NULL;
  }
}*/

// -----------------

// Allocate DX12 title textures for song titles + custom messages
  {
    m_nTitleTexSizeX = max(m_nTexSizeX, m_nTexSizeY);
    m_nTitleTexSizeY = m_nTitleTexSizeX / 4;

    UINT tw = (UINT)m_nTitleTexSizeX;
    UINT th = (UINT)m_nTitleTexSizeY;

    // Create GDI DC + DIB section for title text rendering
    if (m_titleDC) { DeleteDC(m_titleDC); m_titleDC = nullptr; }
    if (m_titleDIB) { DeleteObject(m_titleDIB); m_titleDIB = nullptr; }
    m_titleDIBBits = nullptr;

    BITMAPINFO bmi = {};
    bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bmi.bmiHeader.biWidth = tw;
    bmi.bmiHeader.biHeight = -(int)th;  // top-down
    bmi.bmiHeader.biPlanes = 1;
    bmi.bmiHeader.biBitCount = 32;
    bmi.bmiHeader.biCompression = BI_RGB;

    m_titleDC = CreateCompatibleDC(nullptr);
    if (m_titleDC) {
      m_titleDIB = CreateDIBSection(m_titleDC, &bmi, DIB_RGB_COLORS, (void**)&m_titleDIBBits, nullptr, 0);
      if (m_titleDIB) {
        SelectObject(m_titleDC, m_titleDIB);
        SetBkMode(m_titleDC, TRANSPARENT);
      }
    }

    // Create DX12 textures + SRVs + binding blocks for each supertext slot
    ID3D12Device* dev = GetDX12Device();
    bool titleOK = (dev != nullptr && m_titleDC != nullptr && m_titleDIB != nullptr);
    for (int i = 0; i < NUM_SUPERTEXTS; i++) {
      m_lpDDSTitle[i] = NULL;  // DX9 textures no longer used
      m_dx12Title[i].Reset();

      if (!titleOK) continue;

      D3D12_RESOURCE_DESC desc = {};
      desc.Dimension        = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
      desc.Width            = tw;
      desc.Height           = th;
      desc.DepthOrArraySize = 1;
      desc.MipLevels        = 1;
      desc.Format           = DXGI_FORMAT_B8G8R8A8_UNORM;
      desc.SampleDesc.Count = 1;

      D3D12_HEAP_PROPERTIES heapProps = {};
      heapProps.Type = D3D12_HEAP_TYPE_DEFAULT;

      HRESULT hr = dev->CreateCommittedResource(
          &heapProps, D3D12_HEAP_FLAG_NONE, &desc,
          D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
          nullptr, IID_PPV_ARGS(&m_dx12Title[i].resource));
      if (FAILED(hr)) { titleOK = false; continue; }

      m_dx12Title[i].width  = tw;
      m_dx12Title[i].height = th;
      m_dx12Title[i].format = DXGI_FORMAT_B8G8R8A8_UNORM;
      m_dx12Title[i].currentState = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;

      // Allocate SRV
      D3D12_CPU_DESCRIPTOR_HANDLE srvCpu = m_lpDX->AllocateSrvCpu();
      m_dx12Title[i].srvIndex = m_lpDX->m_nextFreeSrvSlot;

      CreateSRV2D(dev, m_dx12Title[i].resource.Get(), DXGI_FORMAT_B8G8R8A8_UNORM, srvCpu);
      m_lpDX->AllocateSrvGpu();

      m_lpDX->CreateBindingBlockForTexture(m_dx12Title[i]);
    }

    // Create per-slot upload buffers (one per supertext slot).
    // A single shared buffer caused corruption when multiple supertexts redrew
    // in the same frame: both CopyTextureRegion commands executed after the last
    // CPU write, so the first texture got the second message's data.
    if (titleOK) {
      UINT rowPitch = (tw * 4 + D3D12_TEXTURE_DATA_PITCH_ALIGNMENT - 1)
                      & ~(D3D12_TEXTURE_DATA_PITCH_ALIGNMENT - 1);
      UINT64 uploadSize = (UINT64)rowPitch * th;

      D3D12_HEAP_PROPERTIES uploadHeap = {};
      uploadHeap.Type = D3D12_HEAP_TYPE_UPLOAD;

      D3D12_RESOURCE_DESC bufDesc = {};
      bufDesc.Dimension        = D3D12_RESOURCE_DIMENSION_BUFFER;
      bufDesc.Width            = uploadSize;
      bufDesc.Height           = 1;
      bufDesc.DepthOrArraySize = 1;
      bufDesc.MipLevels        = 1;
      bufDesc.SampleDesc.Count = 1;
      bufDesc.Layout           = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

      for (int i = 0; i < NUM_SUPERTEXTS; i++) {
        m_dx12TitleUploadBuf[i].Reset();
        dev->CreateCommittedResource(
            &uploadHeap, D3D12_HEAP_FLAG_NONE, &bufDesc,
            D3D12_RESOURCE_STATE_GENERIC_READ,
            nullptr, IID_PPV_ARGS(&m_dx12TitleUploadBuf[i]));
      }
    }

    for (int i = 0; i < NUM_SUPERTEXTS; i++) {
      if (m_supertexts[i].fStartTime != -1.0f) {
        m_supertexts[i].bRedrawSuperText = true;
      }
    }
  }

  // -----------------

  // create 'm_gdi_title_font_doublesize'
  int songtitle_font_size = m_fontinfo[SONGTITLE_FONT].nSize * m_nTitleTexSizeX / 256;
  if (songtitle_font_size < 6) songtitle_font_size = 6;
  if (!(m_gdi_title_font_doublesize = CreateFontW(songtitle_font_size, 0, 0, 0, m_fontinfo[SONGTITLE_FONT].bBold ? 900 : 400,
    m_fontinfo[SONGTITLE_FONT].bItalic, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, m_fontinfo[SONGTITLE_FONT].bAntiAliased ? ANTIALIASED_QUALITY : DEFAULT_QUALITY, DEFAULT_PITCH, m_fontinfo[SONGTITLE_FONT].szFace))) {
    MessageBoxW(NULL, wasabiApiLangString(IDS_ERROR_CREATING_DOUBLE_SIZED_GDI_TITLE_FONT),
      wasabiApiLangString(IDS_MILKDROP_ERROR, title, sizeof(title)),
      MB_OK | MB_SETFOREGROUND | MB_TOPMOST);
    return false;
  }

  if (GetDevice()) {
    if (D3DXCreateFontW(GetDevice(),
      songtitle_font_size,
      0,
      m_fontinfo[SONGTITLE_FONT].bBold ? 900 : 400,
      1,
      m_fontinfo[SONGTITLE_FONT].bItalic,
      DEFAULT_CHARSET,
      OUT_DEFAULT_PRECIS,
      ANTIALIASED_QUALITY,//DEFAULT_QUALITY,
      DEFAULT_PITCH,
      m_fontinfo[SONGTITLE_FONT].szFace,
      &m_d3dx_title_font_doublesize
    ) != D3D_OK) {
      MessageBoxW(GetPluginWindow(), wasabiApiLangString(IDS_ERROR_CREATING_DOUBLE_SIZED_D3DX_TITLE_FONT),
        wasabiApiLangString(IDS_MILKDROP_ERROR, title, sizeof(title)), MB_OK | MB_SETFOREGROUND | MB_TOPMOST);
      return false;
    }
  }

  // -----------------

  m_texmgr.Init(GetDevice());
  m_texmgr.InitDX12(m_lpDX);

  //dumpmsg("Init: mesh allocation");
  m_verts = new MYVERTEX[(m_nGridX + 1) * (m_nGridY + 1)];
  m_verts_temp = new MYVERTEX[(m_nGridX + 2) * 4];
  m_vertinfo = new td_vertinfo[(m_nGridX + 1) * (m_nGridY + 1)];
  m_indices_strip = new int[(m_nGridX + 2) * (m_nGridY * 2)];
  m_indices_list = new int[m_nGridX * m_nGridY * 6];
  if (!m_verts || !m_vertinfo) {
    swprintf(buf, L"couldn't allocate mesh - out of memory");
    dumpmsg(buf);
    MessageBoxW(GetPluginWindow(), buf, wasabiApiLangString(IDS_MILKDROP_ERROR, title, 64), MB_OK | MB_SETFOREGROUND | MB_TOPMOST);
    return false;
  }

  int nVert = 0;
  // DX9 half-texel offset for UV alignment; not needed in DX12 (pixel centers at +0.5).
  float texel_offset_x = (m_lpDX && m_lpDX->m_device) ? 0.0f : 0.5f / (float)m_nTexSizeX;
  float texel_offset_y = (m_lpDX && m_lpDX->m_device) ? 0.0f : 0.5f / (float)m_nTexSizeY;
  for (int y = 0; y <= m_nGridY; y++) {
    for (int x = 0; x <= m_nGridX; x++) {
      // precompute x,y,z
      m_verts[nVert].x = x / (float)m_nGridX * 2.0f - 1.0f;
      m_verts[nVert].y = y / (float)m_nGridY * 2.0f - 1.0f;
      m_verts[nVert].z = 0.0f;

      // precompute rad, ang, being conscious of aspect ratio
      if (m_bScreenDependentRenderMode)
        m_vertinfo[nVert].rad = sqrtf(m_verts[nVert].x * m_verts[nVert].x + m_verts[nVert].y * m_verts[nVert].y);
      else
        m_vertinfo[nVert].rad = sqrtf(m_verts[nVert].x * m_verts[nVert].x * m_fAspectX * m_fAspectX + m_verts[nVert].y * m_verts[nVert].y * m_fAspectY * m_fAspectY);

      if (y == m_nGridY / 2 && x == m_nGridX / 2)
        m_vertinfo[nVert].ang = 0.0f;
      else
        if (m_bScreenDependentRenderMode)
          m_vertinfo[nVert].ang = atan2f(m_verts[nVert].y, m_verts[nVert].x);
        else
          m_vertinfo[nVert].ang = atan2f(m_verts[nVert].y * m_fAspectY, m_verts[nVert].x * m_fAspectX);

      m_vertinfo[nVert].a = 1;
      m_vertinfo[nVert].c = 0;

      m_verts[nVert].rad = m_vertinfo[nVert].rad;
      m_verts[nVert].ang = m_vertinfo[nVert].ang;
      m_verts[nVert].tu_orig = m_verts[nVert].x * 0.5f + 0.5f + texel_offset_x;
      m_verts[nVert].tv_orig = -m_verts[nVert].y * 0.5f + 0.5f + texel_offset_y;

      nVert++;
    }
  }

  // generate triangle strips for the 4 quadrants.
  // each quadrant has m_nGridY/2 strips.
  // each strip has m_nGridX+2 *points* in it, or m_nGridX/2 polygons.
  int xref, yref;
  int nVert_strip = 0;
  for (int quadrant = 0; quadrant < 4; quadrant++) {
    for (int slice = 0; slice < m_nGridY / 2; slice++) {
      for (int i = 0; i < m_nGridX + 2; i++) {
        // quadrants:	2 3
        //				0 1
        xref = i / 2;
        yref = (i % 2) + slice;

        if (quadrant & 1)
          xref = m_nGridX - xref;
        if (quadrant & 2)
          yref = m_nGridY - yref;

        int v = xref + (yref) * (m_nGridX + 1);

        m_indices_strip[nVert_strip++] = v;
      }
    }
  }

  // also generate triangle lists for drawing the main warp mesh.
  int nVert_list = 0;
  for (int quadrant = 0; quadrant < 4; quadrant++) {
    for (int slice = 0; slice < m_nGridY / 2; slice++) {
      for (int i = 0; i < m_nGridX / 2; i++) {
        // quadrants:	2 3
        //				0 1
        xref = i;
        yref = slice;

        if (quadrant & 1)
          xref = m_nGridX - 1 - xref;
        if (quadrant & 2)
          yref = m_nGridY - 1 - yref;

        int v = xref + (yref) * (m_nGridX + 1);

        m_indices_list[nVert_list++] = v;
        m_indices_list[nVert_list++] = v + 1;
        m_indices_list[nVert_list++] = v + m_nGridX + 1;
        m_indices_list[nVert_list++] = v + 1;
        m_indices_list[nVert_list++] = v + m_nGridX + 1;
        m_indices_list[nVert_list++] = v + m_nGridX + 1 + 1;
      }
    }
  }

  // GENERATED TEXTURES FOR SHADERS
  //-------------------------------------
  if (m_nMaxPSVersion > 0) {
    // Generate noise textures
    if (!AddNoiseTex(L"noise_lq", 256, 1)) return false;
    if (!AddNoiseTex(L"noise_lq_lite", 32, 1)) return false;
    if (!AddNoiseTex(L"noise_mq", 256, 4)) return false;
    if (!AddNoiseTex(L"noise_hq", 256, 8)) return false;

    if (!AddNoiseVol(L"noisevol_lq", 32, 1)) return false;
    if (!AddNoiseVol(L"noisevol_hq", 32, 4)) return false;
  }

  if (!m_bInitialPresetSelected) {
    UpdatePresetList(true); //...just does its initial burst!
    if (m_bEnablePresetStartup && wcslen(m_szPresetStartup) > 0) {
      LoadPreset(m_szPresetStartup, 0.0f);

      std::wstring message(m_szPresetStartup);
      size_t pos = message.find_last_of(L"\\/");
      std::wstring sPath;
      std::wstring sFilename;
      if (pos != std::wstring::npos) {
        // Extract the path up to and including the last separator
        sPath = message.substr(0, pos + 1);
        // Extract the filename after the last separator
        sFilename = message.substr(pos + 1);
      }
      else {
        // If no separator is found, assume the fullPath is just a filename
        sFilename = message;
      }

      // try to set the current preset index
      for (size_t i = 0; i < m_presets.size(); i++) {
        if (wcscmp(m_presets[i].szFilename.c_str(), sFilename.c_str()) == 0) {
          m_nCurrentPreset = (int)i;
          break;
        }
      }
    }
    else {
      LoadRandomPreset(0.0f);
    }
    if (m_bAutoLockPresetWhenNoMusic)
      m_bPresetLockedByUser = false;
    m_bInitialPresetSelected = true;
  }
  else {
    LoadShaders(&m_shaders, m_pState, false, false);  // Also force-load the shaders - otherwise they'd only get compiled on a preset switch.
    CreateDX12PresetPSOs();
  }

  // Re-wrap backbuffers for Spout if active (render targets were just recreated)
  if (bSpoutOut && bInitialized && !m_bSpoutDX12Ready && m_lpDX) {
    for (int n = 0; n < DXC_FRAME_COUNT; n++) {
      if (!spoutsender.WrapDX12Resource(
              m_lpDX->m_renderTargets[n].Get(),
              &m_pWrappedBackBuffers[n],
              D3D12_RESOURCE_STATE_RENDER_TARGET)) {
        DebugLogA("Spout: Re-wrap failed after resize", LOG_ERROR);
        SpoutReleaseWraps();
        break;
      }
    }
    if (m_pWrappedBackBuffers[0] && m_pWrappedBackBuffers[DXC_FRAME_COUNT - 1])
      m_bSpoutDX12Ready = true;
  }

  return true;
}

float fCubicInterpolate(float y0, float y1, float y2, float y3, float t) {
  float a0, a1, a2, a3, t2;

  t2 = t * t;
  a0 = y3 - y2 - y0 + y1;
  a1 = y0 - y1 - a0;
  a2 = y2 - y0;
  a3 = y1;

  return(a0 * t * t2 + a1 * t2 + a2 * t + a3);
}

DWORD dwCubicInterpolate(DWORD y0, DWORD y1, DWORD y2, DWORD y3, float t) {
  // performs cubic interpolation on a D3DCOLOR value.
  DWORD ret = 0;
  DWORD shift = 0;
  for (int i = 0; i < 4; i++) {
    float f = fCubicInterpolate(
      ((y0 >> shift) & 0xFF) / 255.0f,
      ((y1 >> shift) & 0xFF) / 255.0f,
      ((y2 >> shift) & 0xFF) / 255.0f,
      ((y3 >> shift) & 0xFF) / 255.0f,
      t
    );
    if (f < 0)
      f = 0;
    if (f > 1)
      f = 1;
    ret |= ((DWORD)(f * 255)) << shift;
    shift += 8;
  }
  return ret;
}

//----------------------------------------------------------------------

void Engine::CleanUpMyDX9Stuff(int final_cleanup) {
  // Clean up all your DX9 and D3DX textures, fonts, buffers, etc. here.
  // EVERYTHING CREATED IN ALLOCATEMYDX9STUFF() SHOULD BE CLEANED UP HERE.
  // The input parameter, 'final_cleanup', will be 0 if this is
  //   a routine cleanup (part of a window resize or switch between
  //   fullscr/windowed modes), or 1 if this is the final cleanup
  //   and the plugin is exiting.  Note that even if it is a routine
  //   cleanup, *you still have to release ALL your DirectX stuff,
  //   because the DirectX device is being destroyed and recreated!*
  // Also set all the pointers back to NULL;
  //   this is important because if we go to reallocate the DX9
  //   stuff later, and something fails, then CleanUp will get called,
  //   but it will then be trying to clean up invalid pointers.)
  // The SafeRelease() and SafeDelete() macros make your code prettier;
  //   they are defined here in utility.h as follows:
  //       #define SafeRelease(x) if (x) {x->Release(); x=NULL;}
  //       #define SafeDelete(x)  if (x) {delete x; x=NULL;}
  // IMPORTANT:
  //   This function ISN'T only called when the plugin exits!
  //   It is also called whenever the user toggles between fullscreen and
  //   windowed modes, or resizes the window.  Basically, on these events,
  //   the base class calls CleanUpMyDX9Stuff before Reset()ing the DirectX
  //   device, and then calls AllocateMyDX9Stuff afterwards.



  // Release Spout wrapped backbuffers before render targets are destroyed
  SpoutReleaseWraps();

  // One funky thing here: if we're switching between fullscreen and windowed,
  //  or doing any other thing that causes all this stuff to get reloaded in a second,
  //  then if we were blending 2 presets, jump fully to the new preset.
  // Otherwise the old preset wouldn't get all reloaded, and it app would crash
  //  when trying to use its stuff.
  if (m_nLoadingPreset != 0) {
    // finish up the pre-load — must wait for bg thread to complete.
    // Use a timed wait: D3DCompile can stall indefinitely on malformed shaders,
    // which would hang CleanUpMyDX9Stuff (and the render thread) forever.
    if (m_presetLoadThread.joinable()) {
      HANDLE hThread = (HANDLE)m_presetLoadThread.native_handle();
      DWORD waitResult = WaitForSingleObject(hThread, 12000); // 12-second timeout
      if (waitResult == WAIT_TIMEOUT) {
        // Shader compiler stalled — detach so cleanup can continue.
        mdropdx12->LogInfo(L"CleanUpMyDX9Stuff: preset load thread timed out (D3DCompile stall?), detaching");
        m_presetLoadThread.detach();
      } else {
        m_presetLoadThread.join();
      }
    }
    m_bPresetLoadReady = true;
    LoadPresetTick();
  }
  // just force this:
  m_pState->m_bBlending = false;



  for (size_t i = 0; i < m_textures.size(); i++)
    if (m_textures[i].texptr) {
      // notify all CShaderParams classes that we're releasing a bindable texture!!
      size_t N = global_CShaderParams_master_list.size();
      for (size_t j = 0; j < N; j++)
        global_CShaderParams_master_list[j]->OnTextureEvict(m_textures[i].texptr);

      SafeRelease(m_textures[i].texptr);
    }
  m_textures.clear();

  // DON'T RELEASE blur textures - they were already released because they're in m_textures[].
#if (NUM_BLUR_TEX>0)
  for (int i = 0; i < NUM_BLUR_TEX; i++)
    m_lpBlur[i] = NULL;//SafeRelease(m_lpBlur[i]);
#endif

  // NOTE: not necessary; shell does this for us.
  /*if (GetDevice())
  {
      GetDevice()->SetTexture(0, NULL);
      GetDevice()->SetTexture(1, NULL);
  }*/

  SafeRelease(m_pSpriteVertDecl);
  SafeRelease(m_pWfVertDecl);
  SafeRelease(m_pMyVertDecl);

  m_shaders.comp.Clear();
  m_shaders.warp.Clear();
  m_shaders.bufferA.Clear();
  m_OldShaders.comp.Clear();
  m_OldShaders.warp.Clear();
  m_OldShaders.bufferA.Clear();
  m_NewShaders.comp.Clear();
  m_NewShaders.warp.Clear();
  m_NewShaders.bufferA.Clear();
  m_fallbackShaders_vs.comp.Clear();
  m_fallbackShaders_ps.comp.Clear();
  m_fallbackShaders_vs.warp.Clear();
  m_fallbackShaders_ps.warp.Clear();
  m_BlurShaders[0].vs.Clear();
  m_BlurShaders[0].ps.Clear();
  m_BlurShaders[1].vs.Clear();
  m_BlurShaders[1].ps.Clear();
  m_dx12BlurPSO[0].Reset();
  m_dx12BlurPSO[1].Reset();
  m_dx12BufferAPSO.Reset();
  /*
  SafeRelease( m_shaders.comp.ptr );
  SafeRelease( m_shaders.warp.ptr );
  SafeRelease( m_OldShaders.comp.ptr );
  SafeRelease( m_OldShaders.warp.ptr );
  SafeRelease( m_NewShaders.comp.ptr );
  SafeRelease( m_NewShaders.warp.ptr );
  SafeRelease( m_fallbackShaders_vs.comp.ptr );
  SafeRelease( m_fallbackShaders_ps.comp.ptr );
  SafeRelease( m_fallbackShaders_vs.warp.ptr );
  SafeRelease( m_fallbackShaders_ps.warp.ptr );
  */
  SafeRelease(m_pShaderCompileErrors);
  //SafeRelease( m_pCompiledFragments );
  //SafeRelease( m_pFragmentLinker );

  // 2. release DX12 dynamic textures and reclaim descriptor heap slots
  // (caller CleanUpDX9Stuff already called WaitForGpu before us)
  if (m_lpDX) {
    m_dx12VS[0].Reset();
    m_dx12VS[1].Reset();
    m_injectEffectTex.Reset();
    m_dx12Feedback[0].Reset();
    m_dx12Feedback[1].Reset();
    m_dx12BufferAPSO.Reset();
    m_pInjectEffectPSO.Reset();
    m_pSpoutInputPSO.Reset();
    DestroySpoutInput();
    DestroyVideoCapture();
#if (NUM_BLUR_TEX > 0)
    for (int bi = 0; bi < NUM_BLUR_TEX; bi++)
      m_dx12Blur[bi].Reset();
#endif
    m_lpDX->ResetDynamicDescriptors();
  }

  // 2b. release stuff
  SafeRelease(m_lpVS[0]);
  SafeRelease(m_lpVS[1]);

  for (int i = 0; i < NUM_SUPERTEXTS; i++) {
    SafeRelease(m_lpDDSTitle[i]);
  }

  SafeRelease(m_d3dx_title_font_doublesize);

  // NOTE: THIS CODE IS IN THE RIGHT PLACE.
  if (m_gdi_title_font_doublesize) {
    DeleteObject(m_gdi_title_font_doublesize);
    m_gdi_title_font_doublesize = NULL;
  }

  m_texmgr.Finish();

  if (m_verts != NULL) {
    delete m_verts;
    m_verts = NULL;
  }

  if (m_verts_temp != NULL) {
    delete m_verts_temp;
    m_verts_temp = NULL;
  }

  if (m_vertinfo != NULL) {
    delete m_vertinfo;
    m_vertinfo = NULL;
  }

  if (m_indices_list != NULL) {
    delete m_indices_list;
    m_indices_list = NULL;
  }

  if (m_indices_strip != NULL) {
    delete m_indices_strip;
    m_indices_strip = NULL;
  }

  ClearErrors();
}

//----------------------------------------------------------------------
//----------------------------------------------------------------------
//----------------------------------------------------------------------

void Engine::MyRenderFn(int redraw) {

  EnterCriticalSection(&g_cs);

  // Render a frame of animation here.
  // This function is called each frame just AFTER BeginScene().
  // For timing information, call 'GetTime()' and 'GetFps()'.
  // The usual formula is like this (but doesn't have to be):
  //   1. take care of timing/other paperwork/etc. for new frame
  //   2. clear the background
  //   3. get ready for 3D drawing
  //   4. draw your 3D stuff
  //   5. call PrepareFor2DDrawing()
  //   6. draw your 2D stuff (overtop of your 3D scene)
  // If the 'redraw' flag is 1, you should try to redraw
  //   the last frame; GetTime, GetFps, and GetFrame should
  //   all return the same values as they did on the last
  //   call to MyRenderFn().  Otherwise, the redraw flag will
  //   be zero, and you can draw a new frame.  The flag is
  //   used to force the desktop to repaint itself when
  //   running in desktop mode and Winamp is paused or stopped.

  // Flush any queued sprite loads (must happen after BeginFrame opens the command list)
  if (!m_pendingSpriteLoads.empty()) {
    for (auto& ps : m_pendingSpriteLoads)
      LaunchSprite(ps.nSpriteNum, ps.nSlot);
    m_pendingSpriteLoads.clear();
  }

  //   1. take care of timing/other paperwork/etc. for new frame
  if (!redraw) {
    // Force settings window open if config needs attention (once, on first frame)
    if (m_bSettingsNeedAttention && m_UI_mode == UI_REGULAR) {
      m_bSettingsNeedAttention = false; // only force once
      OpenSettingsWindow();
      AddError(L"Preset directory not found. Press F8 to open Settings.", 8.0f, ERR_MISC, true);
    }

    // Self-bootstrap: open settings to About tab, notify user about verbose logging
    if (m_bSelfBootstrapped && m_UI_mode == UI_REGULAR) {
      m_bSelfBootstrapped = false; // only once
      // Set ActiveTab to About (page 10) so settings opens there
      WritePrivateProfileStringW(L"Settings", L"ActiveTab", L"10", GetConfigIniFile());
      OpenSettingsWindow();
      AddError(L"First run: debug logging set to max (verbose). Add presets to resources\\presets\\.", 10.0f, ERR_MISC, true);
    }

    float dt = GetTime() - m_prev_time;
    m_prev_time = GetTime(); // note: m_prev_time is not for general use!
    m_bPresetLockedByCode = (m_UI_mode != UI_REGULAR);
    if (m_bPresetLockedByUser || m_bPresetLockedByCode) {
      // to freeze time (at current preset time value) when menus are up or Scroll Lock is on:
  //m_fPresetStartTime += dt;
  //m_fNextPresetTime += dt;
      // OR, to freeze time @ [preset] zero, so that when you exit menus,
      //   you don't run the risk of it changing the preset on you right away:
      m_fPresetStartTime = GetTime();
      m_bPresetDiagLogged = false;
      m_fNextPresetTime = -1.0f;		// flags UpdateTime() to recompute this.
    }

    //if (!m_bPresetListReady)
    //    UpdatePresetList(true);//UpdatePresetRatings(); // read in a few each frame, til they're all in
  }

  m_bHadFocus = m_bHasFocus;

  HWND plugin = GetPluginWindow();
  HWND focus = GetFocus();
  HWND cur = plugin;

  timetick += 1 / GetFps(); //Now these timeticks variables are now became FPS-independent.
  timetick2 += 1 / GetFps();

  //HardCut Modes (controlled via F11 hotkey)
  if (HardcutMode == 2) //Bass Blend
  {
    if (GetFps() > 1.0f && !m_bPresetLockedByUser && !m_bPresetLockedByCode) {
      if ((double)mysound.imm_rel[0] > 1.75 && timetick >= 0.99) {
        if (m_nLoadingPreset == 0)
          NextPreset(0.95f);
        timetick = 0;
      }
    }
  }
  if (HardcutMode == 3) //Bass
  {
    if (GetFps() > 1.0f && !m_bPresetLockedByUser && !m_bPresetLockedByCode)
      if ((double)mysound.imm_rel[0] > 1.75) {
        if (m_nLoadingPreset == 0)
          NextPreset(0.0f);
      }
  }
  if (HardcutMode == 4) //Middle
  {
    if (GetFps() > 1.0f && !m_bPresetLockedByUser && !m_bPresetLockedByCode)
      if ((double)mysound.imm_rel[1] > 1.75) {
        if (m_nLoadingPreset == 0)
          NextPreset(0.0f);
      }
  }
  if (HardcutMode == 5) //Treble
  {
    if (GetFps() > 1.0f && !m_bPresetLockedByUser && !m_bPresetLockedByCode)
      if ((double)mysound.imm_rel[2] > 1.75) {
        if (m_nLoadingPreset == 0)
          NextPreset(0.0f);
      }
  }
  if (HardcutMode == 6) //Bass Fast Blend
  {
    if (GetFps() > 1.0f && !m_bPresetLockedByUser && !m_bPresetLockedByCode)
      if ((double)mysound.imm_rel[0] > 1.75 && timetick >= 0.49) {
        if (m_nLoadingPreset == 0)
          NextPreset(0.4f);
        timetick = 0;
      }
  }
  if (HardcutMode == 7) //Treble Fast Blend
  {
    if (GetFps() > 1.0f && !m_bPresetLockedByUser && !m_bPresetLockedByCode)
      if ((double)mysound.imm_rel[2] > 1.75 && timetick2 >= 0.49) {
        if (m_nLoadingPreset == 0)
          NextPreset(0.4f);
        timetick2 = 0;
      }
  }
  if (HardcutMode == 8) //Bass Blend and Hard Cut Treble
  {
    if (GetFps() > 1.0f && !m_bPresetLockedByUser && !m_bPresetLockedByCode) {
      if ((double)mysound.imm_rel[0] > 1.75 && timetick >= 0.48) {
        if (m_nLoadingPreset == 0)
          NextPreset(0.24f);
        timetick = 0;
      }
      if ((double)mysound.imm_rel[2] > 1.75 && timetick2 >= 0.48) {
        if (m_nLoadingPreset == 0)
          NextPreset(0.0f);
        timetick2 = 0;
      }
    }
  }

  if (HardcutMode == 9) //Rhythmic Hardcut
  {
    if (GetFps() > 1.0f && !m_bPresetLockedByUser && !m_bPresetLockedByCode) {
      if (((double)mysound.imm_rel[0] > 1.75 || (double)mysound.imm_rel[2] > 1.75) && timetick >= 0.23) {
        if (m_nLoadingPreset == 0)
          NextPreset(0.0f);
        timetick = 0;
      }
    }
  }

  if (HardcutMode == 10) //2 beats
  {
    if (GetFps() > 1.0f && !m_bPresetLockedByUser && !m_bPresetLockedByCode) {
      if (((double)mysound.imm_rel[0] > 2.05 && timetick >= 0.23)) {
        beatcount++;
        if (beatcount % 2 == 0) {
          if (m_nLoadingPreset == 0)
            NextPreset(0.0f);
        }
        timetick = 0;
      }
    }
    if (timetick >= 1)
      beatcount = -1;
  }

  if (HardcutMode == 11) //4 beats
  {
    if (GetFps() > 1.0f && !m_bPresetLockedByUser && !m_bPresetLockedByCode) {
      if (((double)mysound.imm_rel[0] > 2.05 && timetick >= 0.23)) {
        beatcount++;
        if (beatcount % 4 == 0) {
          if (m_nLoadingPreset == 0)
            NextPreset(0.0f);
        }
        timetick = 0;
      }
    }
    if (timetick >= 1)
      beatcount = -1;
  }

  if (HardcutMode == 12) //Kinetronix (Vizikord) -- Probably we need BPM algorithm for getting in sync
  {
    if (GetFps() > 1.0f && !m_bPresetLockedByUser && !m_bPresetLockedByCode) {
      if (((double)mysound.imm_rel[0] > 2.05 && timetick >= 0.23)) {
        beatcount++;
        if (beatcount % 2 == 0) {
          if (m_nLoadingPreset == 0)
            NextPreset(0.0f);
        }
        else {
          if (m_nLoadingPreset == 0)
            PrevPreset(0.0f);
        }

        if (beatcount % 32 == 0) {
          {
            if (m_nLoadingPreset == 0)
              NextPreset(0.0f);
            if (m_nLoadingPreset == 0)
              NextPreset(0.0f);
          }
        } //Double the Next Preset (basically a trick to load 2 presets at the same time)
        timetick = 0;
      }
    }
    if (timetick >= 1)
      beatcount = -1;
  }
  //END

  //Auto-Lock Preset when it's silence.
  if (m_bAutoLockPresetWhenNoMusic) {
    if (((double)mysound.imm_rel[0] + (double)mysound.imm_rel[1] + (double)mysound.imm_rel[2]) == 0) {
      if (TimeToAutoLockPreset <= 2.5)
        TimeToAutoLockPreset += 1 / GetFps();
      else {
        if (!AutoLockedPreset) {
          m_bPresetLockedByUser = true;
          AutoLockedPreset = true;
        }
      }
    }

    else if (((double)mysound.imm_rel[0] + (double)mysound.imm_rel[1] + (double)mysound.imm_rel[2]) != 0) {
      if (AutoLockedPreset) {
        m_bPresetLockedByUser = false;
        AutoLockedPreset = false;
      }
      TimeToAutoLockPreset = 0;
    }
  }
  //END

  if (m_bEnableMouseInteraction) {
    POINT pt;
    GetCursorPos(&pt);
    ScreenToClient(GetPluginWindow(), &pt);

    RECT clientRect;
    GetClientRect(GetPluginWindow(), &clientRect);

    int clientW = clientRect.right - clientRect.left;
    int clientH = clientRect.bottom - clientRect.top;
    if (clientW <= 1) clientW = 1;
    if (clientH <= 1) clientH = 1;

    // Prefer renderer/backbuffer size if the renderer uses a different internal resolution
    int targetW = clientW;
    int targetH = clientH;
    if (g_engine.m_lpDX && g_engine.m_lpDX->m_backbuffer_width > 0 && g_engine.m_lpDX->m_backbuffer_height > 0) {
      targetW = g_engine.m_lpDX->m_backbuffer_width;
      targetH = g_engine.m_lpDX->m_backbuffer_height;
    }
    else if (g_engine.m_WindowWidth > 0 && g_engine.m_WindowHeight > 0) {
      targetW = g_engine.m_WindowWidth;
      targetH = g_engine.m_WindowHeight;
    }
    if (targetW <= 1) targetW = 1;
    if (targetH <= 1) targetH = 1;

    // Map client pixel coords to target pixel space (handles stretched/backbuffer-fixed modes)
    float sx = (static_cast<float>(pt.x) * targetW) / static_cast<float>(clientW);
    float sy = (static_cast<float>(pt.y) * targetH) / static_cast<float>(clientH);

    // Normalize [0 .. target-1] -> [0..1]
    float fx = sx / static_cast<float>(targetW - 1);
    float fy = sy / static_cast<float>(targetH - 1);

    // clamp to [0,1]
    fx = clamp(fx, 0.0f, 1.0f);
    fy = clamp(fy, 0.0f, 1.0f);

    // Convert to lower-left origin: (0,0)=lower-left, (1,1)=upper-right
    m_mouseX = fx;        // 0 = left, 1 = right
    m_mouseY = 1.0f - fy; // 0 = bottom, 1 = top
  }

  //Duration of the click called from WM_LBUTTONDOWN
  if (m_mouseClicked > 0) {
    m_mouseClicked--;
  }

  //Don't show the help message again when the "Press F1 for help" message is finished.
  //Useful when I press CTRL + T or when it reaches 250000 seconds, it shows the message again, so I did.
  if (GetTime() >= PRESS_F1_DUR)
    m_show_press_f1_msg = 0;

  m_bHasFocus = false;
  do {
    m_bHasFocus = (focus == cur);
    if (m_bHasFocus)
      break;
    cur = GetParent(cur);
  } while (cur != NULL);

  if (m_hTextWnd && focus == m_hTextWnd)
    m_bHasFocus = 1;

  //if (m_bEnablePresetStartup) 
  //    if (StartupPresetLoaded == false)
  //    {
  //        LoadPreset(m_szPresetStartup, 0.0f);
  //        StartupPresetLoaded = true;
  //    }  //The Preset Startup Implementation are reworked and moved to line 2560.

  if (GetFocus() == NULL)
    m_bHasFocus = 0;

  if (!redraw) {
    GetSongTitle(m_szSongTitle, sizeof(m_szSongTitle) - 1);
    if (wcscmp(m_szSongTitle, m_szSongTitlePrev)) {
      lstrcpynW(m_szSongTitlePrev, m_szSongTitle, 512);
      if (m_bSongTitleAnims)
        LaunchSongTitleAnim(-1);
    }

    if (m_AutoHue && m_AutoHueSeconds > 0) {
      if (GetTime() > m_AutoHueTimeLastChange + m_AutoHueSeconds) {
        m_AutoHueTimeLastChange = GetTime();
        m_ColShiftHue += 0.01f;
        if (m_ColShiftHue >= 1.0f) {
          m_ColShiftHue = -1.0f;
        }
        SendSettingsInfoToMDropDX12Remote();
      }
    }
  }

  // 2. Clear the background:
  //DWORD clear_color = (m_fog_enabled) ? FOG_COLOR : 0xFF000000;
  //GetDevice()->Clear(0, 0, D3DCLEAR_TARGET|D3DCLEAR_ZBUFFER, clear_color, 1.0f, 0);

  // 5. switch to 2D drawing mode.  2D coord system:
  //         +--------+ Y=-1
  //         |        |
  //         | screen |             Z=0: front of scene
  //         |        |             Z=1: back of scene
  //         +--------+ Y=1
  //       X=-1      X=1
  PrepareFor2DDrawing(GetDevice());

  if (!redraw)
    DoCustomSoundAnalysis();    // emulates old pre-vms milkdrop sound analysis

  RenderFrame(redraw);  // see milkdropfs.cpp

  CopyBackbufferToFeedback();  // capture comp output for Shadertoy temporal feedback (no-op when unused)

  // Swap feedback ping-pong: current write becomes next frame's read
  if (m_bCompUsesFeedback || m_bHasBufferA)
    m_nFeedbackIdx = 1 - m_nFeedbackIdx;

  RenderInjectEffect();  // F11 inject effect post-process pass (no-op when mode==0)

  if (!redraw) {
    m_nFramesSinceResize++;
    if (m_nLoadingPreset > 0) {
      LoadPresetTick();
    }
  }

  LeaveCriticalSection(&g_cs);

}

//----------------------------------------------------------------------
//----------------------------------------------------------------------
//----------------------------------------------------------------------
//----------------------------------------------------------------------

HWND CreateLabel(HWND hParent, const wchar_t* text, int x, int y, int w, int h, HFONT hFont, bool visible) {
  DWORD style = WS_CHILD | SS_LEFT | (visible ? WS_VISIBLE : 0);
  HWND hw = CreateWindowExW(0, L"STATIC", text, style,
    x, y, w, h, hParent, NULL, GetModuleHandle(NULL), NULL);
  if (hw && hFont) SendMessage(hw, WM_SETFONT, (WPARAM)hFont, TRUE);
  return hw;
}

HWND CreateEdit(HWND hParent, const wchar_t* text, int id, int x, int y, int w, int h, HFONT hFont, DWORD extraStyle, bool visible) {
  DWORD style = WS_CHILD | WS_TABSTOP | ES_AUTOHSCROLL | extraStyle | (visible ? WS_VISIBLE : 0);
  HWND hw = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", text, style,
    x, y, w, h, hParent, (HMENU)(INT_PTR)id, GetModuleHandle(NULL), NULL);
  if (hw && hFont) SendMessage(hw, WM_SETFONT, (WPARAM)hFont, TRUE);
  return hw;
}

HWND CreateCheck(HWND hParent, const wchar_t* text, int id, int x, int y, int w, int h, HFONT hFont, bool checked, bool visible) {
  DWORD style = WS_CHILD | WS_TABSTOP | BS_OWNERDRAW | (visible ? WS_VISIBLE : 0);
  HWND hw = CreateWindowExW(0, L"BUTTON", text, style,
    x, y, w, h, hParent, (HMENU)(INT_PTR)id, GetModuleHandle(NULL), NULL);
  if (hw) {
    if (hFont) SendMessage(hw, WM_SETFONT, (WPARAM)hFont, TRUE);
    // Mark as checkbox and store check state (BS_OWNERDRAW doesn't track state)
    SetPropW(hw, L"IsCheckbox", (HANDLE)(intptr_t)1);
    SetPropW(hw, L"Checked", (HANDLE)(intptr_t)(checked ? 1 : 0));
  }
  return hw;
}

void DrawOwnerCheckbox(DRAWITEMSTRUCT* pDIS, bool bDark, COLORREF colBg, COLORREF colCtrlBg, COLORREF colBorder, COLORREF colText) {
  HDC hdc = pDIS->hDC;
  RECT rc = pDIS->rcItem;
  bool bChecked = (bool)(intptr_t)GetPropW(pDIS->hwndItem, L"Checked");
  bool bFocused = (pDIS->itemState & ODS_FOCUS) != 0;

  // Fill entire background
  HBRUSH hBrBg = CreateSolidBrush(bDark ? colBg : GetSysColor(COLOR_BTNFACE));
  FillRect(hdc, &rc, hBrBg);
  DeleteObject(hBrBg);

  // Draw checkbox indicator square, scaled to control height
  int ctrlH = rc.bottom - rc.top;
  int boxSize = max(ctrlH / 2, 11);
  int boxY = rc.top + (ctrlH - boxSize) / 2;
  RECT rcBox = { rc.left + 1, boxY, rc.left + 1 + boxSize, boxY + boxSize };

  if (bDark) {
    HBRUSH hBrBox = CreateSolidBrush(colCtrlBg);
    FillRect(hdc, &rcBox, hBrBox);
    DeleteObject(hBrBox);
    HBRUSH hBrBorder = CreateSolidBrush(bFocused ? RGB(100, 150, 220) : colBorder);
    FrameRect(hdc, &rcBox, hBrBorder);
    DeleteObject(hBrBorder);
  } else {
    DrawFrameControl(hdc, &rcBox, DFC_BUTTON, DFCS_BUTTONCHECK | (bChecked ? DFCS_CHECKED : 0));
    // Draw text for light mode and return
    RECT rcText = { rcBox.right + 4, rc.top, rc.right, rc.bottom };
    SetBkMode(hdc, TRANSPARENT);
    SetTextColor(hdc, GetSysColor(COLOR_BTNTEXT));
    HFONT hFont = (HFONT)SendMessage(pDIS->hwndItem, WM_GETFONT, 0, 0);
    HFONT hOld = hFont ? (HFONT)SelectObject(hdc, hFont) : NULL;
    wchar_t szText[128] = {};
    GetWindowTextW(pDIS->hwndItem, szText, 128);
    DrawTextW(hdc, szText, -1, &rcText, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
    if (hOld) SelectObject(hdc, hOld);
    return;
  }

  // Draw checkmark in dark mode (scaled proportionally to boxSize)
  if (bChecked) {
    int pw = max(boxSize / 7, 1) + 1;
    HPEN hPen = CreatePen(PS_SOLID, pw, colText);
    HPEN hOld = (HPEN)SelectObject(hdc, hPen);
    int pad = max(boxSize / 4, 2);
    MoveToEx(hdc, rcBox.left + pad, rcBox.top + boxSize * 6 / 10, NULL);
    LineTo(hdc, rcBox.left + boxSize * 4 / 10, rcBox.bottom - pad);
    LineTo(hdc, rcBox.right - pad, rcBox.top + pad);
    SelectObject(hdc, hOld);
    DeleteObject(hPen);
  }

  // Draw text
  RECT rcText = { rcBox.right + 4, rc.top, rc.right, rc.bottom };
  SetBkMode(hdc, TRANSPARENT);
  SetTextColor(hdc, colText);
  HFONT hFont = (HFONT)SendMessage(pDIS->hwndItem, WM_GETFONT, 0, 0);
  HFONT hOldFont = hFont ? (HFONT)SelectObject(hdc, hFont) : NULL;
  wchar_t szText[128] = {};
  GetWindowTextW(pDIS->hwndItem, szText, 128);
  DrawTextW(hdc, szText, -1, &rcText, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
  if (hOldFont) SelectObject(hdc, hOldFont);

  if (bFocused) {
    RECT rcFocus = rc;
    InflateRect(&rcFocus, -1, -1);
    DrawFocusRect(hdc, &rcFocus);
  }
}

HWND CreateRadio(HWND hParent, const wchar_t* text, int id, int x, int y, int w, int h, HFONT hFont, bool checked, bool firstInGroup, bool visible) {
  DWORD style = WS_CHILD | WS_TABSTOP | BS_OWNERDRAW | (visible ? WS_VISIBLE : 0);
  if (firstInGroup) style |= WS_GROUP;
  HWND hw = CreateWindowExW(0, L"BUTTON", text, style,
    x, y, w, h, hParent, (HMENU)(INT_PTR)id, GetModuleHandle(NULL), NULL);
  if (hw) {
    if (hFont) SendMessage(hw, WM_SETFONT, (WPARAM)hFont, TRUE);
    SetPropW(hw, L"IsRadio", (HANDLE)(intptr_t)1);
    SetPropW(hw, L"Checked", (HANDLE)(intptr_t)(checked ? 1 : 0));
  }
  return hw;
}

void DrawOwnerRadio(DRAWITEMSTRUCT* pDIS, bool bDark, COLORREF colBg, COLORREF colCtrlBg, COLORREF colBorder, COLORREF colText) {
  HDC hdc = pDIS->hDC;
  RECT rc = pDIS->rcItem;
  bool bChecked = (bool)(intptr_t)GetPropW(pDIS->hwndItem, L"Checked");
  bool bFocused = (pDIS->itemState & ODS_FOCUS) != 0;

  // Fill entire background
  HBRUSH hBrBg = CreateSolidBrush(bDark ? colBg : GetSysColor(COLOR_BTNFACE));
  FillRect(hdc, &rc, hBrBg);
  DeleteObject(hBrBg);

  // Draw radio circle indicator, scaled to control height
  int ctrlH = rc.bottom - rc.top;
  int circSize = max(ctrlH / 2, 11);
  int circY = rc.top + (ctrlH - circSize) / 2;
  int cx = rc.left + 1 + circSize / 2;
  int cy = circY + circSize / 2;
  int r = circSize / 2;

  if (bDark) {
    // Draw circle background
    HBRUSH hBrCirc = CreateSolidBrush(colCtrlBg);
    HBRUSH hBrBorderBr = CreateSolidBrush(bFocused ? RGB(100, 150, 220) : colBorder);
    HPEN hPenBorder = CreatePen(PS_SOLID, 1, bFocused ? RGB(100, 150, 220) : colBorder);
    HPEN hOldPen = (HPEN)SelectObject(hdc, hPenBorder);
    HBRUSH hOldBr = (HBRUSH)SelectObject(hdc, hBrCirc);
    Ellipse(hdc, cx - r, cy - r, cx + r, cy + r);
    SelectObject(hdc, hOldBr);
    SelectObject(hdc, hOldPen);
    DeleteObject(hBrCirc);
    DeleteObject(hBrBorderBr);
    DeleteObject(hPenBorder);
  } else {
    RECT rcRadio = { cx - r, cy - r, cx + r, cy + r };
    DrawFrameControl(hdc, &rcRadio, DFC_BUTTON, DFCS_BUTTONRADIO | (bChecked ? DFCS_CHECKED : 0));
    // Draw text for light mode and return
    RECT rcText = { rc.left + 1 + circSize + 4, rc.top, rc.right, rc.bottom };
    SetBkMode(hdc, TRANSPARENT);
    SetTextColor(hdc, GetSysColor(COLOR_BTNTEXT));
    HFONT hFont = (HFONT)SendMessage(pDIS->hwndItem, WM_GETFONT, 0, 0);
    HFONT hOld = hFont ? (HFONT)SelectObject(hdc, hFont) : NULL;
    wchar_t szText[128] = {};
    GetWindowTextW(pDIS->hwndItem, szText, 128);
    DrawTextW(hdc, szText, -1, &rcText, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
    if (hOld) SelectObject(hdc, hOld);
    return;
  }

  // Draw filled dot in dark mode when selected
  if (bChecked) {
    int dotR = max(r / 2, 2);
    HBRUSH hBrDot = CreateSolidBrush(colText);
    HPEN hPenDot = CreatePen(PS_SOLID, 1, colText);
    HPEN hOldPen = (HPEN)SelectObject(hdc, hPenDot);
    HBRUSH hOldBr = (HBRUSH)SelectObject(hdc, hBrDot);
    Ellipse(hdc, cx - dotR, cy - dotR, cx + dotR, cy + dotR);
    SelectObject(hdc, hOldBr);
    SelectObject(hdc, hOldPen);
    DeleteObject(hBrDot);
    DeleteObject(hPenDot);
  }

  // Draw text
  RECT rcText = { rc.left + 1 + circSize + 4, rc.top, rc.right, rc.bottom };
  SetBkMode(hdc, TRANSPARENT);
  SetTextColor(hdc, colText);
  HFONT hFont = (HFONT)SendMessage(pDIS->hwndItem, WM_GETFONT, 0, 0);
  HFONT hOldFont = hFont ? (HFONT)SelectObject(hdc, hFont) : NULL;
  wchar_t szText[128] = {};
  GetWindowTextW(pDIS->hwndItem, szText, 128);
  DrawTextW(hdc, szText, -1, &rcText, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
  if (hOldFont) SelectObject(hdc, hOldFont);

  if (bFocused) {
    RECT rcFocus = rc;
    InflateRect(&rcFocus, -1, -1);
    DrawFocusRect(hdc, &rcFocus);
  }
}

HWND CreateBtn(HWND hParent, const wchar_t* text, int id, int x, int y, int w, int h, HFONT hFont, bool visible) {
  DWORD style = WS_CHILD | WS_TABSTOP | BS_OWNERDRAW | (visible ? WS_VISIBLE : 0);
  HWND hw = CreateWindowExW(0, L"BUTTON", text, style,
    x, y, w, h, hParent, (HMENU)(INT_PTR)id, GetModuleHandle(NULL), NULL);
  if (hw && hFont) SendMessage(hw, WM_SETFONT, (WPARAM)hFont, TRUE);
  return hw;
}

HWND CreateSlider(HWND hParent, int id, int x, int y, int w, int h,
                   int rangeMin, int rangeMax, int pos, bool visible) {
  DWORD style = WS_CHILD | WS_TABSTOP | TBS_HORZ | TBS_NOTICKS | (visible ? WS_VISIBLE : 0);
  HWND hw = CreateWindowExW(0, TRACKBAR_CLASSW, NULL, style,
    x, y, w, h, hParent, (HMENU)(INT_PTR)id, GetModuleHandle(NULL), NULL);
  if (hw) {
    SendMessage(hw, TBM_SETRANGE, TRUE, MAKELPARAM(rangeMin, rangeMax));
    SendMessage(hw, TBM_SETPOS, TRUE, pos);
  }
  return hw;
}

// Draw a single 3D edge (1px highlight on top-left, shadow on bottom-right)
void draw3DEdge(HDC hdc, const RECT& rc, COLORREF hi, COLORREF shadow, bool raised) {
  COLORREF topLeft  = raised ? hi : shadow;
  COLORREF botRight = raised ? shadow : hi;

  // Top + left edges
  HPEN pen = CreatePen(PS_SOLID, 1, topLeft);
  HPEN oldPen = (HPEN)SelectObject(hdc, pen);
  MoveToEx(hdc, rc.left, rc.top, NULL);
  LineTo(hdc, rc.right - 1, rc.top);
  MoveToEx(hdc, rc.left, rc.top, NULL);
  LineTo(hdc, rc.left, rc.bottom - 1);
  SelectObject(hdc, oldPen);
  DeleteObject(pen);

  // Bottom + right edges
  pen = CreatePen(PS_SOLID, 1, botRight);
  oldPen = (HPEN)SelectObject(hdc, pen);
  MoveToEx(hdc, rc.left, rc.bottom - 1, NULL);
  LineTo(hdc, rc.right, rc.bottom - 1);
  MoveToEx(hdc, rc.right - 1, rc.top, NULL);
  LineTo(hdc, rc.right - 1, rc.bottom);
  SelectObject(hdc, oldPen);
  DeleteObject(pen);
}

// Owner-draw button paint helper
void DrawOwnerButton(DRAWITEMSTRUCT* pDIS, bool bDark,
  COLORREF colBtnFace, COLORREF colBtnHi, COLORREF colBtnShadow, COLORREF colText) {
  HDC hdc = pDIS->hDC;
  RECT rc = pDIS->rcItem;
  bool pressed = (pDIS->itemState & ODS_SELECTED) != 0;
  bool focused = (pDIS->itemState & ODS_FOCUS) != 0;

  bool disabled = (pDIS->itemState & ODS_DISABLED) != 0;

  if (bDark) {
    // Fill button face
    HBRUSH hBrFill = CreateSolidBrush(colBtnFace);
    FillRect(hdc, &rc, hBrFill);
    DeleteObject(hBrFill);

    // 3D beveled edges (outer)
    draw3DEdge(hdc, rc, colBtnHi, colBtnShadow, !pressed);

    // Inner bevel (1px inset for thicker 3D look)
    RECT inner = { rc.left + 1, rc.top + 1, rc.right - 1, rc.bottom - 1 };
    COLORREF innerHi = RGB(75, 75, 75);   // subtle inner highlight
    COLORREF innerSh = RGB(45, 45, 45);   // subtle inner shadow
    draw3DEdge(hdc, inner, innerHi, innerSh, !pressed);

    // Focus rectangle (inside the 3D border)
    if (focused) {
      RECT rcFocus = { rc.left + 3, rc.top + 3, rc.right - 3, rc.bottom - 3 };
      DrawFocusRect(hdc, &rcFocus);
    }

    SetBkMode(hdc, TRANSPARENT);
    SetTextColor(hdc, disabled ? RGB(128, 128, 128) : colText);
  } else {
    // Light theme: standard system button look
    UINT edge = pressed ? DFCS_BUTTONPUSH | DFCS_PUSHED : DFCS_BUTTONPUSH;
    DrawFrameControl(hdc, &rc, DFC_BUTTON, edge);

    SetBkMode(hdc, TRANSPARENT);
    SetTextColor(hdc, GetSysColor(COLOR_BTNTEXT));
  }

  // Offset text when pressed
  RECT textRc = rc;
  if (pressed) OffsetRect(&textRc, 1, 1);

  wchar_t szText[128] = {};
  GetWindowTextW(pDIS->hwndItem, szText, 128);
  HFONT hFont = (HFONT)SendMessage(pDIS->hwndItem, WM_GETFONT, 0, 0);
  HFONT hOldFont = hFont ? (HFONT)SelectObject(hdc, hFont) : NULL;
  DrawTextW(hdc, szText, -1, &textRc, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
  if (hOldFont) SelectObject(hdc, hOldFont);
}


} // namespace mdrop
