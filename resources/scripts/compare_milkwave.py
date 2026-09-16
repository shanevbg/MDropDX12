"""Compare MDropDX12 against Milkwave, preset by preset, and rank the differences.

Milkwave 4 ships MDropDX12 v3, so both are already installed together. This
plays the same preset in each, photographs both windows, scores how far apart
they look, and writes a report with the worst offenders first -- so a human
eyeballs the top of a ranking instead of clicking through a library.

    python compare_milkwave.py                     20 presets from the current folder
    python compare_milkwave.py --presets 100
    python compare_milkwave.py --dir "D:\\presets\\mine"
    python compare_milkwave.py --list presets.txt  one path per line

Normally started for you by resources/scripts/compare-milkwave.txt, which puts
MDropDX12 into a steady state first. Run it by hand whenever that is not wanted.

Needs Pillow, and says so in a message box if it is missing, because this is
usually launched from the visualizer where a console traceback would scroll past
unseen -- or never appear at all.

WHAT IT WILL NOT DO
  * It never raises, moves or focuses either window. PrintWindow photographs a
    window that is completely covered, so the comparison runs while you use the
    machine for something else.
  * It sends Milkwave nothing but PRESET=. Milkwave's IPC vocabulary is smaller
    than MDropDX12's, and a command it does not know is not worth the risk of
    finding out the hard way.
  * It changes no setting in either program, and leaves both on whatever preset
    the last comparison used.

READING THE SCORE
  0 is identical. The metric is the mean absolute per-channel difference of both
  frames scaled to a common small size, as a percentage. Scaling down first is
  deliberate: the two engines are never on the same frame at the same instant,
  so a full-resolution diff mostly measures that the wave moved three pixels.
  What survives scaling is a difference in COLOUR, SHAPE or BRIGHTNESS, which is
  what a rendering difference looks like.

  Anything under about 8 is usually just motion. Above 25 something is really
  different -- a missing texture, a warp that did not apply, a blend mode.
  Look at the image before believing the number either way.
"""

import argparse
import ctypes
import os
import pathlib
import re
import sys
import time
from ctypes import wintypes

if sys.platform != "win32":
    sys.exit("This compares two Windows programs, so it only runs on Windows.")

user32 = ctypes.WinDLL("user32", use_last_error=True)
gdi32 = ctypes.WinDLL("gdi32", use_last_error=True)
kernel32 = ctypes.WinDLL("kernel32", use_last_error=True)


def _die(message):
    """Say it in a box AND on stdout, then stop.

    A box because this is usually started by the visualizer's LAUNCH= and there
    may be no console to read; stdout as well for anyone running it by hand or
    from a terminal.
    """
    print(message)
    try:
        user32.MessageBoxW(None, message, "MDropDX12 - compare with Milkwave",
                           0x00000010 | 0x00040000)   # MB_ICONERROR | MB_TOPMOST
    except Exception:
        pass
    sys.exit(1)


def _ask(message):
    """Yes/No box. True for Yes. Assumes No if there is no desktop to ask on."""
    try:
        MB_YESNO, MB_ICONQUESTION, MB_TOPMOST = 0x4, 0x20, 0x40000
        IDYES = 6
        return user32.MessageBoxW(None, message,
                                  "MDropDX12 - compare with Milkwave",
                                  MB_YESNO | MB_ICONQUESTION | MB_TOPMOST) == IDYES
    except Exception:
        return False


def _load_pillow():
    """Import Pillow, offering to install it if it is not there.

    Offering rather than just installing: this runs on someone else's machine,
    and putting a package into their Python without asking is not ours to do.
    Offering rather than just telling, because the alternative is a message box
    that says "run pip install pillow" to somebody who launched a visualizer.
    """
    try:
        from PIL import Image, ImageChops, ImageDraw
        return Image, ImageChops, ImageDraw
    except ImportError:
        pass

    if not _ask("This comparison needs Pillow, which is not installed.\n\n"
                "Install it now with pip?\n\n"
                "This installs into:\n%s" % sys.executable):
        _die("Pillow is needed for the comparison.\n\n"
             "Install it yourself with:\n\n    pip install pillow")

    import subprocess
    print("installing pillow into %s ..." % sys.executable)
    try:
        rc = subprocess.call([sys.executable, "-m", "pip", "install", "pillow"])
    except Exception as e:
        rc, err = 1, str(e)
    else:
        err = "pip exited with %d" % rc
    if rc != 0:
        _die("Installing Pillow did not work (%s).\n\n"
             "Install it yourself with:\n\n    %s -m pip install pillow"
             % (err, sys.executable))

    try:
        from PIL import Image, ImageChops, ImageDraw
        return Image, ImageChops, ImageDraw
    except ImportError:
        # pip can succeed into a different environment than the interpreter
        # actually imports from, and the second failure is the confusing one --
        # so name the interpreter rather than repeating the same advice.
        _die("Pillow installed but still cannot be imported by:\n\n%s\n\n"
             "That usually means pip put it somewhere else. Try:\n\n"
             "    %s -m pip install --force-reinstall pillow"
             % (sys.executable, sys.executable))


Image, ImageChops, ImageDraw = _load_pillow()

PIPE_DIR = r"\\.\pipe"
PW_RENDERFULLCONTENT = 0x00000002


# ───────────────────────── talking to the two programs ─────────────────────────

def _pipe_send(path, message, want_reply=True, timeout=4.0):
    """One request down a Milkwave-protocol pipe; the reply, or ''.

    Messages are UTF-16LE and null-terminated, which is the protocol both
    programs speak -- see docs/Scripts.md.
    """
    try:
        fh = open(path, "r+b", buffering=0)
    except OSError:
        return ""
    try:
        fh.write(message.encode("utf-16-le") + b"\x00\x00")
        if not want_reply:
            return ""
        fh.flush()
        deadline = time.time() + timeout
        while time.time() < deadline:
            try:
                data = fh.read(4096)
            except OSError:
                return ""
            if data:
                return data.decode("utf-16-le", "replace").rstrip("\x00").strip()
        return ""
    finally:
        try:
            fh.close()
        except OSError:
            pass


def _exe_for_pid(pid):
    """Full path of a running process, or '' -- used to tell the two apart."""
    PROCESS_QUERY_LIMITED_INFORMATION = 0x1000
    h = kernel32.OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, False, pid)
    if not h:
        return ""
    try:
        size = wintypes.DWORD(1024)
        buf = ctypes.create_unicode_buffer(size.value)
        if kernel32.QueryFullProcessImageNameW(h, 0, buf, ctypes.byref(size)):
            return buf.value
        return ""
    finally:
        kernel32.CloseHandle(h)


def discover():
    """{'mdrop': [(pid, pipe, exe)...], 'milkwave': [...]} for what is running.

    Both programs serve \\\\.\\pipe\\Milkwave_<pid>, so the pipe NAME cannot say
    which is which -- the owning executable is what separates them.

    EVERY match is returned, not the first. This tool sends DEVICE=, which
    changes which sound card an instance listens to, so quietly picking one of
    several running copies could reconfigure the wrong one. The caller refuses
    to guess instead.
    """
    found = {"mdrop": [], "milkwave": []}
    try:
        names = os.listdir(PIPE_DIR)
    except OSError:
        return found
    for name in names:
        m = re.fullmatch(r"(?:Milkwave|mdx12test)_(\d+)", name)
        if not m:
            continue
        pid = int(m.group(1))
        full = _exe_for_pid(pid)
        exe = os.path.basename(full).lower()
        if not exe:
            continue
        role = "milkwave" if "milkwave" in exe else ("mdrop" if "mdrop" in exe else None)
        if role:
            found[role].append((pid, os.path.join(PIPE_DIR, name), full))
    return found


def pick(found, role, label, want_pid=None):
    """The one instance of `role` to drive, or a message box explaining why not."""
    cands = found.get(role) or []
    if want_pid:
        cands = [c for c in cands if c[0] == want_pid]
        if not cands:
            _die("No %s is running with pid %d." % (label, want_pid))
    if not cands:
        _die("%s is not running.\n\nStart it and run the comparison again." % label)
    if len(cands) > 1:
        listing = "\n".join("    pid %d   %s" % (p, e) for p, _, e in cands)
        _die("More than one %s is running, and this would have to guess which "
             "one you meant -- it changes the audio device, so guessing is not "
             "safe:\n\n%s\n\nClose the one you do not want, or name the other "
             "with --%s-pid." % (label, listing, "mdrop" if role == "mdrop" else "milkwave"))
    return cands[0][0], cands[0][1]


# ──────────────────────────── photographing a window ───────────────────────────

class BITMAPINFOHEADER(ctypes.Structure):
    _fields_ = [("biSize", wintypes.DWORD), ("biWidth", wintypes.LONG),
                ("biHeight", wintypes.LONG), ("biPlanes", wintypes.WORD),
                ("biBitCount", wintypes.WORD), ("biCompression", wintypes.DWORD),
                ("biSizeImage", wintypes.DWORD),
                ("biXPelsPerMeter", wintypes.LONG),
                ("biYPelsPerMeter", wintypes.LONG),
                ("biClrUsed", wintypes.DWORD), ("biClrImportant", wintypes.DWORD)]


def window_for_pid(pid):
    """(hwnd, minimised) -- the biggest top-level window a process owns.

    Biggest, rather than first: both programs own more than one window -- tool
    windows, hidden IPC windows -- and the render window is always the largest.

    A MINIMISED window is reported separately rather than skipped. It has a 0x0
    client rect, so it can never win on area and the search comes back
    empty-handed -- which reads as "this program has no window" when the real
    answer is "restore it and try again". That is worth telling the user apart
    from a genuine failure, because it is the likely one.
    """
    best = [0, 0]                      # hwnd, area
    iconic = [0]

    @ctypes.WINFUNCTYPE(wintypes.BOOL, wintypes.HWND, wintypes.LPARAM)
    def enum(hwnd, _):
        owner = ctypes.c_uint32()
        user32.GetWindowThreadProcessId(hwnd, ctypes.byref(owner))
        if owner.value != pid or not user32.IsWindowVisible(hwnd):
            return True
        if user32.GetWindow(hwnd, 4):  # GW_OWNER: a dialog, not the main window
            return True
        if user32.IsIconic(hwnd):
            iconic[0] = hwnd
            return True
        r = wintypes.RECT()
        user32.GetClientRect(hwnd, ctypes.byref(r))
        area = (r.right - r.left) * (r.bottom - r.top)
        if area > best[1]:
            best[0], best[1] = hwnd, area
        return True

    user32.EnumWindows(enum, 0)
    if best[0]:
        return best[0], False
    return iconic[0], bool(iconic[0])


def capture(hwnd):
    """A window's CLIENT area as an RGB Image, or None.

    PrintWindow with PW_RENDERFULLCONTENT, so a window that is behind others --
    or entirely off screen -- still photographs correctly and nothing has to be
    brought to the front while the comparison runs. Pillow has no equivalent:
    ImageGrab takes the SCREEN, which would photograph whatever is covering the
    window instead of the window.
    """
    r = wintypes.RECT()
    if not user32.GetClientRect(hwnd, ctypes.byref(r)):
        return None
    w, h = r.right - r.left, r.bottom - r.top
    if w <= 0 or h <= 0:
        return None

    src = user32.GetWindowDC(hwnd)
    if not src:
        return None
    mem = gdi32.CreateCompatibleDC(src)
    bmp = gdi32.CreateCompatibleBitmap(src, w, h)
    old = gdi32.SelectObject(mem, bmp)
    try:
        if not user32.PrintWindow(hwnd, mem, PW_RENDERFULLCONTENT):
            return None
        bi = BITMAPINFOHEADER()
        bi.biSize = ctypes.sizeof(BITMAPINFOHEADER)
        bi.biWidth, bi.biHeight = w, -h        # negative: rows top-down
        bi.biPlanes, bi.biBitCount = 1, 32
        buf = ctypes.create_string_buffer(w * h * 4)
        if not gdi32.GetDIBits(mem, bmp, 0, h, buf, ctypes.byref(bi), 0):
            return None
        # GetDIBits gives BGRA bottom-up unless the height is negative, which it
        # is above; "BGRX" tells Pillow to ignore the unused fourth byte rather
        # than treat it as alpha, which PrintWindow leaves at zero.
        return Image.frombuffer("RGB", (w, h), buf.raw, "raw", "BGRX", 0, 1)
    finally:
        gdi32.SelectObject(mem, old)
        gdi32.DeleteObject(bmp)
        gdi32.DeleteDC(mem)
        user32.ReleaseDC(hwnd, src)


# ──────────────────── making the two comparable before measuring ───────────────

SWP_NOMOVE, SWP_NOZORDER, SWP_NOACTIVATE = 0x0002, 0x0004, 0x0010


def _rects(hwnd):
    wr, cr = wintypes.RECT(), wintypes.RECT()
    user32.GetWindowRect(hwnd, ctypes.byref(wr))
    user32.GetClientRect(hwnd, ctypes.byref(cr))
    return wr, cr


def match_window_sizes(a, b, want=None):
    """Give both windows the same CLIENT size. Returns (size, restore).

    Not optional, and not something padding can substitute for. Both engines
    render TO THE SHAPE THEY ARE GIVEN -- a preset on a tall window is a
    genuinely different picture from the same preset on a wide one -- so
    comparing two differently shaped windows measures the window, not the
    engine. Measured: a portrait MDropDX12 against a landscape Milkwave scored
    45 on presets that turned out to be far closer than that.

    The size chosen is the smaller of the two, so neither window has to grow and
    neither can end up off the edge of a screen. Only SIZES are set -- never
    positions, never z-order, never focus -- and `restore` puts both back.
    """
    wa, ca = _rects(a)
    wb, cb = _rects(b)
    if want is None:
        want = (min(ca.right, cb.right), min(ca.bottom, cb.bottom))
    want = (max(320, want[0]), max(180, want[1]))

    original = [(h, r.right - r.left, r.bottom - r.top) for h, r in ((a, wa), (b, wb))]

    for hwnd, wr, cr in ((a, wa, ca), (b, wb, cb)):
        # The border and title bar differ between the two programs, so the
        # WINDOW size that yields a given CLIENT size differs too. Measure each
        # one's own non-client margin rather than assuming they match.
        padw = (wr.right - wr.left) - (cr.right - cr.left)
        padh = (wr.bottom - wr.top) - (cr.bottom - cr.top)
        user32.SetWindowPos(hwnd, 0, 0, 0, want[0] + padw, want[1] + padh,
                            SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE)

    def restore():
        for hwnd, w, h in original:
            user32.SetWindowPos(hwnd, 0, 0, 0, w, h,
                                SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE)

    return want, restore


def match_audio(mdrop_pipe, mw_pipe, device=None):
    """Point both engines at the same capture device. Returns what was used.

    Without this the comparison can be measuring two different songs. Milkwave
    and MDropDX12 each remember their own device, and if one is listening to a
    silent endpoint -- a Sonar channel nothing is playing to, say -- its frames
    are of a preset sitting idle while the other's are of it reacting. That
    looks exactly like a rendering difference and is not one.

    DEVICE= is understood by both, which is what makes this fixable rather than
    merely warnable: MDropDX12's current device is read back and pushed to
    Milkwave, so the two agree without the user choosing anything.
    """
    if not device:
        reply = _pipe_send(mdrop_pipe, "GET_AUDIO_DEVICES", timeout=6.0)
        m = re.search(r"\|active=([^|]*)$", reply or "")
        device = m.group(1).strip() if m else ""
    if not device:
        return ""
    for pipe in (mdrop_pipe, mw_pipe):
        _pipe_send(pipe, "DEVICE=%s" % device, want_reply=False)
    time.sleep(1.5)          # both re-open capture; give it a moment to settle
    return device


# ───────────────────────────── comparing and reporting ─────────────────────────

# A common size, so two windows of different shapes still compare. Small on
# purpose: see READING THE SCORE at the top.
GRID = (192, 108)


def fit(img, size=GRID):
    """Scale into `size` KEEPING the aspect ratio, padding the rest with black.

    Stretching instead would make the score meaningless whenever the two
    windows are different shapes, and they usually are -- one of them is
    whatever the user last dragged it to. Measured here on a portrait window
    against a landscape one: squashing the tall frame flat scored 45 against a
    frame that was not that different, and the number was mostly describing the
    squash.

    Padding does not rescue a genuinely mismatched pair -- both engines render
    to the shape they are given, so a tall preset really is a different picture
    from a wide one. It stops the SCALING from adding error of its own, and the
    aspect warning below is what tells the user the rest.
    """
    w, h = img.size
    scale = min(size[0] / w, size[1] / h)
    nw, nh = max(1, int(w * scale)), max(1, int(h * scale))
    sheet = Image.new("RGB", size, (0, 0, 0))
    sheet.paste(img.resize((nw, nh), Image.LANCZOS),
                ((size[0] - nw) // 2, (size[1] - nh) // 2))
    return sheet


def aspect_gap(a, b):
    """How far apart two windows are in shape, as a percentage."""
    ra, rb = a.size[0] / a.size[1], b.size[0] / b.size[1]
    return 100.0 * abs(ra - rb) / max(ra, rb)


def difference(a, b):
    """Mean absolute per-channel difference of two same-size frames, 0..100."""
    diff = ImageChops.difference(a, b)
    hist = diff.histogram()
    total = 0
    for channel in range(3):
        band = hist[channel * 256:(channel + 1) * 256]
        total += sum(value * count for value, count in enumerate(band))
    pixels = a.width * a.height * 3
    return 100.0 * total / (pixels * 255)


def side_by_side(path, left, right, title):
    """One labelled image: MDropDX12 on the left, Milkwave on the right.

    Two label rows, not one. Crowded onto a single row the preset name ran
    straight through the word "Milkwave" and both became unreadable -- and the
    name is the thing you need when flicking through a folder of these.
    """
    gap, row = 4, 13
    bar = row * 2
    w, h = left.size
    sheet = Image.new("RGB", (w * 2 + gap, h + bar), (24, 24, 24))
    sheet.paste(left, (0, bar))
    sheet.paste(right, (w + gap, bar))
    draw = ImageDraw.Draw(sheet)
    draw.rectangle([w, bar, w + gap - 1, bar + h - 1], fill=(255, 96, 0))
    draw.text((4, 1), title[:76], fill=(235, 235, 235))
    draw.text((4, row), "MDropDX12", fill=(150, 150, 150))
    draw.text((w + gap + 4, row), "Milkwave", fill=(150, 150, 150))
    sheet.save(path)


# ─────────────────────────────────── the run ───────────────────────────────────

def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--presets", type=int, default=20,
                    help="how many presets to compare (default 20)")
    ap.add_argument("--dir", help="folder of presets; default is the one "
                                  "MDropDX12 is playing from")
    ap.add_argument("--list", help="file of preset paths, one per line")
    ap.add_argument("--settle", type=float, default=2.5,
                    help="seconds to let both engines settle before the "
                         "photograph (default 2.5)")
    ap.add_argument("--out", help="where the report goes; default is a "
                                  "'comparison' folder beside this script")
    ap.add_argument("--size", help="client size to give BOTH windows, e.g. "
                                   "960x540; default is the smaller of the two")
    ap.add_argument("--device", help="capture device to give BOTH engines; "
                                     "default is whatever MDropDX12 is using")
    ap.add_argument("--mdrop-pid", type=int,
                    help="which MDropDX12 to drive, when more than one is up")
    ap.add_argument("--milkwave-pid", type=int,
                    help="which Milkwave to drive, when more than one is up")
    ap.add_argument("--no-audio-match", action="store_true",
                    help="leave each engine on its own audio device (the "
                         "scores are then only meaningful if you know they "
                         "already match)")
    args = ap.parse_args()

    apps = discover()
    mdrop_pid, mdrop_pipe = pick(apps, "mdrop", "MDropDX12", args.mdrop_pid)
    mw_pid, mw_pipe = pick(apps, "milkwave", "Milkwave", args.milkwave_pid)
    print("MDropDX12 pid %d, Milkwave pid %d" % (mdrop_pid, mw_pid))

    # Which presets. A folder given on the command line wins; otherwise ask
    # MDropDX12 what it is playing and use that folder, which is almost always
    # the library the question is about.
    presets = []
    if args.list:
        presets = [pathlib.Path(l.strip()) for l in
                   pathlib.Path(args.list).read_text(encoding="utf-8").splitlines()
                   if l.strip()]
    else:
        folder = pathlib.Path(args.dir) if args.dir else None
        if folder is None:
            status = _pipe_send(mdrop_pipe, "GET_PRESET_STATUS")
            m = re.search(r"file=([^|]+)", status)
            if not m or not m.group(1).strip():
                _die("Could not tell which folder MDropDX12 is playing from.\n\n"
                     "Pass one with --dir.")
            folder = pathlib.Path(m.group(1).strip()).parent
        if not folder.is_dir():
            _die("Not a folder:\n\n%s" % folder)
        for ext in ("*.milk", "*.milk2"):
            presets.extend(folder.glob(ext))
        presets.sort()
    presets = [p for p in presets if p.is_file()][:max(1, args.presets)]
    if not presets:
        _die("No presets found to compare.")

    out = pathlib.Path(args.out) if args.out else \
        pathlib.Path(__file__).resolve().parent / "comparison"
    out.mkdir(parents=True, exist_ok=True)

    hw_mdrop, min_mdrop = window_for_pid(mdrop_pid)
    hw_mw, min_mw = window_for_pid(mw_pid)
    minimised = [n for n, m in (("MDropDX12", min_mdrop), ("Milkwave", min_mw)) if m]
    if minimised:
        _die("%s %s minimised, and a minimised window cannot be photographed.\n\n"
             "Restore it -- it does not need to be in front, or even visible -- "
             "and run the comparison again."
             % (" and ".join(minimised), "is" if len(minimised) == 1 else "are"))
    if not hw_mdrop or not hw_mw:
        _die("Could not find a render window for %s."
             % (" and ".join(n for n, h in (("MDropDX12", hw_mdrop),
                                            ("Milkwave", hw_mw)) if not h)))

    # Both fair before anything is measured: same client size, same audio.
    want = None
    if args.size:
        m = re.fullmatch(r"(\d+)x(\d+)", args.size.strip().lower())
        if not m:
            _die("--size wants WxH, for example 960x540")
        want = (int(m.group(1)), int(m.group(2)))
    size, restore_sizes = match_window_sizes(hw_mdrop, hw_mw, want)
    print("both windows set to %dx%d (restored at the end)" % size)

    device = "" if args.no_audio_match else match_audio(mdrop_pipe, mw_pipe, args.device)
    print("audio input: %s" % (device or "NOT MATCHED -- scores may be meaningless"))

    results, failed = [], 0
    print("comparing %d preset(s); nothing is focused or brought to the front"
          % len(presets))
    try:
      for i, preset in enumerate(presets, 1):
        _pipe_send(mw_pipe, "PRESET=%s" % preset, want_reply=False)
        _pipe_send(mdrop_pipe, "PRESET=%s" % preset, want_reply=False)
        time.sleep(args.settle)

        a, b = capture(hw_mdrop), capture(hw_mw)
        if a is None or b is None:
            failed += 1
            print("  %3d/%d  %-48s CAPTURE FAILED" % (i, len(presets), preset.name[:48]))
            continue
        gap = aspect_gap(a, b)
        a, b = fit(a), fit(b)
        score = difference(a, b)
        img = out / ("%03d_%s.png" % (i, re.sub(r"[^A-Za-z0-9_.-]", "_", preset.stem)[:60]))
        side_by_side(img, a, b, preset.stem)
        results.append((score, preset.name, img.name, gap))
        print("  %3d/%d  %6.2f  %s%s" % (i, len(presets), score, preset.name[:56],
                                         "   (shapes differ)" if gap > 2.0 else ""))
    finally:
        restore_sizes()

    results.sort(key=lambda r: -r[0])
    report = out / "report.txt"
    with report.open("w", encoding="utf-8") as fh:
        fh.write("MDropDX12 vs Milkwave -- %d preset(s), worst first\n" % len(results))
        if failed:
            fh.write("%d preset(s) could not be photographed and are not listed\n" % failed)
        fh.write("left half of each image is MDropDX12, right half is Milkwave\n")
        fh.write("windows matched to %dx%d; audio input %s\n"
                 % (size[0], size[1], device or "NOT MATCHED"))
        if not device:
            fh.write("  !! both engines may have been listening to different "
                     "audio, which alone can\n"
                     "     account for any score here. Pass --device to fix it.\n")
        if any(g > 2.0 for _, _, _, g in results):
            fh.write("  !! some frames still differ in shape after matching, so "
                     "those scores are\n"
                     "     partly measuring the window rather than the engine.\n")
        fh.write("0 = identical; under ~8 is usually just motion between frames\n\n")
        for score, name, img, gap in results:
            fh.write("%6.2f  %-58s %s%s\n"
                     % (score, name[:58], img, "  (shapes differ)" if gap > 2.0 else ""))

    print()
    print("report: %s" % report)
    if results:
        print("worst: %.2f  %s" % (results[0][0], results[0][1]))


if __name__ == "__main__":
    main()
