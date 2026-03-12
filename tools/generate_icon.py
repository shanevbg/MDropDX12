#!/usr/bin/env python3
"""Generate MDropDX12 icon with MilkDrop/Milkwave-inspired psychedelic design.

Creates a multi-size .ico with a cosmic swirl filled with vibrant
psychedelic colors reminiscent of MilkDrop visualizations.
"""

import math
import struct
from PIL import Image, ImageDraw, ImageFilter


def hsv_to_rgb(h, s, v):
    """Convert HSV (0-1 range) to RGB (0-255)."""
    if s == 0:
        r = g = b = int(v * 255)
        return r, g, b

    h = h % 1.0
    i = int(h * 6)
    f = h * 6 - i
    p = v * (1 - s)
    q = v * (1 - s * f)
    t = v * (1 - s * (1 - f))

    if i == 0:
        r, g, b = v, t, p
    elif i == 1:
        r, g, b = q, v, p
    elif i == 2:
        r, g, b = p, v, t
    elif i == 3:
        r, g, b = p, q, v
    elif i == 4:
        r, g, b = t, p, v
    else:
        r, g, b = v, p, q

    return int(r * 255), int(g * 255), int(b * 255)


def generate_icon_image(size):
    """Generate a single icon image at the given size."""
    # Work at higher resolution for anti-aliasing
    scale = 4
    s = size * scale
    img = Image.new('RGBA', (s, s), (0, 0, 0, 0))

    cx, cy = s / 2, s / 2
    radius = s * 0.50

    pixels = img.load()

    for y in range(s):
        for x in range(s):
            dx = (x - cx) / radius
            dy = (y - cy) / radius
            dist = math.sqrt(dx * dx + dy * dy)

            if dist > 1.12:
                continue

            angle = math.atan2(dy, dx)

            # Strong logarithmic swirl - more twist near center
            swirl = 4.0 * (1.0 - dist ** 0.6)
            sa = angle + swirl

            # Base hue: sweep through spectrum with swirl
            base_hue = (sa / (2 * math.pi) * 1.5 + dist * 0.8) % 1.0

            # Multiple interference patterns for psychedelic richness
            wave1 = math.sin(sa * 3 + dist * 6) * 0.5 + 0.5
            wave2 = math.sin(sa * 5 - dist * 4 + 1.2) * 0.5 + 0.5
            wave3 = math.cos(sa * 2 + dist * 8 - 0.5) * 0.5 + 0.5

            # Hue modulation - creates rainbow bands in the swirl
            hue = (base_hue + 0.15 * math.sin(sa * 4 + dist * 3)) % 1.0

            # Saturation: high throughout, slightly less at bright spots
            sat = 0.75 + 0.25 * wave2

            # Value: bright with dark bands for contrast
            val = 0.4 + 0.5 * wave1 * wave3 + 0.1 * wave2

            # Bright hot center
            core = max(0, 1.0 - dist * 2.0) ** 2
            val = min(1.0, val + core * 0.8)
            sat = sat * (1.0 - core * 0.7)  # Desaturate toward white at center

            # Secondary hot spots along swirl arms
            arm1 = max(0, math.sin(sa * 2 + dist * 5) * 0.5 + 0.3)
            arm1 *= max(0, 1.0 - dist * 1.5) * 0.4
            val = min(1.0, val + arm1)
            sat = max(0, sat - arm1 * 0.3)

            # Convert to RGB
            r, g, b = hsv_to_rgb(hue, max(0, min(1, sat)), max(0, min(1, val)))

            # Boost specific color channels for MilkDrop feel
            # Push toward electric purple/blue/magenta palette
            r = min(255, int(r * 1.05))
            b = min(255, int(b * 1.15))

            # Edge vignette
            if dist > 0.6:
                vignette = 1.0 - ((dist - 0.6) / 0.4) ** 1.5 * 0.5
                vignette = max(0.3, vignette)
                r = int(r * vignette)
                g = int(g * vignette)
                b = int(b * vignette)

            # Dark outer ring border for definition
            if 0.90 < dist < 0.96:
                border = 1.0 - math.sin((dist - 0.90) / 0.06 * math.pi) * 0.3
                r = int(r * border)
                g = int(g * border)
                b = int(b * border)

            # Bright outer rim
            if 0.85 < dist < 0.95:
                rim = math.sin((dist - 0.85) / 0.10 * math.pi)
                rim_angle_color = (angle / (2 * math.pi) + 0.5) % 1.0
                rr, rg, rb = hsv_to_rgb(rim_angle_color, 0.8, 1.0)
                blend = rim * 0.2
                r = min(255, int(r * (1 - blend) + rr * blend))
                g = min(255, int(g * (1 - blend) + rg * blend))
                b = min(255, int(b * (1 - blend) + rb * blend))

            # Alpha with smooth edge
            if dist < 0.90:
                alpha = 255
            elif dist < 1.05:
                alpha = int(255 * max(0, (1.05 - dist) / 0.15))
            else:
                alpha = 0

            # Outer glow (subtle)
            if dist > 0.90 and alpha < 255:
                glow_hue = (angle / (2 * math.pi) * 2) % 1.0
                gr, gg, gb = hsv_to_rgb(glow_hue, 0.9, 0.7)
                glow_alpha = int(max(0, (1.08 - dist) / 0.18) * 100)
                if glow_alpha > alpha:
                    r, g, b = gr, gg, gb
                    alpha = glow_alpha

            pixels[x, y] = (
                max(0, min(255, r)),
                max(0, min(255, g)),
                max(0, min(255, b)),
                max(0, min(255, alpha))
            )

    # Draw bright orange teardrop in center - solid color, big and bold
    drop = Image.new('RGBA', (s, s), (0, 0, 0, 0))
    drop_pixels = drop.load()

    drop_cx = s / 2
    drop_cy = s / 2 + s * 0.10
    drop_height = s * 0.88
    drop_width = s * 0.50

    for y in range(s):
        for x in range(s):
            t = (y - (drop_cy - drop_height * 0.45)) / drop_height
            if t < 0 or t > 1:
                continue

            # Teardrop profile
            if t < 0.12:
                half_w = drop_width * 0.5 * (t / 0.12) * 0.12
            elif t < 0.65:
                tt = (t - 0.12) / 0.53
                half_w = drop_width * 0.5 * (0.12 + 0.88 * math.sin(tt * math.pi * 0.5))
            else:
                tt = (t - 0.65) / 0.35
                half_w = drop_width * 0.5 * math.cos(tt * math.pi * 0.5)

            dx = x - drop_cx
            if half_w < 0.5:
                inside = abs(dx) < 0.5
            else:
                inside = abs(dx) <= half_w

            if not inside:
                continue

            # Edge AA
            if half_w > 1:
                edge_dist = 1.0 - abs(dx) / half_w
                aa = min(1.0, edge_dist / 0.06)
            else:
                aa = 1.0

            # Dark outline at edges, solid orange inside
            if half_w > 1:
                edge_dist = 1.0 - abs(dx) / half_w
            else:
                edge_dist = 1.0

            outline_thickness = 0.12
            if edge_dist < outline_thickness:
                # Dark outline
                blend = edge_dist / outline_thickness
                r_c = int(20 * (1 - blend) + 100 * blend)
                g_c = int(40 * (1 - blend) + 200 * blend)
                b_c = int(80 * (1 - blend) + 255 * blend)
                drop_pixels[x, y] = (r_c, g_c, b_c, int(255 * aa))
            else:
                # Solid light blue (100, 200, 255)
                drop_pixels[x, y] = (100, 200, 255, int(255 * aa))

    # Composite teardrop over swirl
    img = Image.alpha_composite(img, drop)

    # Downsample with high-quality filter
    img = img.resize((size, size), Image.LANCZOS)

    return img


def main():
    sizes = [16, 24, 32, 48, 64, 128, 256]

    print("Generating MDropDX12 icon images...")
    images = []
    for size in sizes:
        print(f"  Generating {size}x{size}...")
        img = generate_icon_image(size)
        images.append(img)
        img.save(f"tools/icon_{size}.png")

    output_path = "src/mDropDX12/engine_icon.ico"
    print(f"Saving icon to {output_path}...")

    # Sort descending for .ico
    images.sort(key=lambda img: img.size[0], reverse=True)
    images[0].save(
        output_path,
        format='ICO',
        sizes=[(img.size[0], img.size[1]) for img in images],
        append_images=images[1:]
    )

    # Verify
    with open(output_path, 'rb') as f:
        data = f.read(6)
        reserved, type_, count = struct.unpack('<HHH', data)
        print(f"Verification: {count} images in .ico file")

    print("Done! Preview PNGs saved in tools/")


if __name__ == '__main__':
    main()
