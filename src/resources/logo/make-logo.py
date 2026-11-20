"""Draws the Last Download Manager logo and writes every file that carries it.

The logo is a rounded triangle pointing down with a white download arrow inside. It is drawn
from the geometry below, 8 times larger than needed, then scaled down, so each size is sharp.
Sizes up to 24 px use their own geometry, laid on the pixel grid with thicker lines: scaled
down from the large one, the arrow would blur into the triangle.

Run from anywhere, with Pillow installed:  python src/resources/logo/make-logo.py
Writes src/resources/ldm.ico (the app, its windows, tray and installer) and the browser
extension's icons in extension/icons/ (on, and grey for a tab where LDM is turned off).
"""

import io
import struct
from pathlib import Path

from PIL import Image, ImageDraw

ROOT = Path(__file__).resolve().parents[3]
BLUE = (47, 107, 255, 255)   # #2F6BFF
GREY = (138, 144, 153, 255)  # #8A9099: the extension's "off" look
WHITE = (255, 255, 255, 255)
SUPERSAMPLE = 8

# Coordinates on a grid of `grid` units across the icon.
LARGE = {
    "grid": 48,
    "triangle": [(8, 9.5), (40, 9.5), (24, 39.5)],
    "corner": 3,  # corner radius of the triangle
    "arrow": [[(24, 13), (24, 25)], [(19, 20.5), (24, 25.5), (29, 20.5)]],
    "stroke": 3.4,
}
SMALL = {
    "grid": 16,
    "triangle": [(2.6, 3.2), (13.4, 3.2), (8, 13.6)],
    "corner": 1.1,
    # The shaft is 2 units wide around x = 8, so at 16 px it covers whole pixels 7 and 8.
    "arrow": [[(8, 4.2), (8, 8.8)], [(5.2, 6.4), (8, 9.2), (10.8, 6.4)]],
    "stroke": 2.0,
}
SMALL_UP_TO = 24  # px: the largest size drawn from SMALL


def draw(size, color):
    spec = SMALL if size <= SMALL_UP_TO else LARGE
    scale = size * SUPERSAMPLE / spec["grid"]
    canvas = Image.new("RGBA", (size * SUPERSAMPLE,) * 2, (0, 0, 0, 0))
    pen = ImageDraw.Draw(canvas)

    def at(point):
        return (point[0] * scale, point[1] * scale)

    def dot(point, radius, fill):
        x, y = at(point)
        r = radius * scale
        pen.ellipse((x - r, y - r, x + r, y + r), fill=fill)

    def stroke(points, width, fill):
        # Round caps and joins: a circle at every point, a line between each pair.
        for a, b in zip(points, points[1:]):
            pen.line([at(a), at(b)], fill=fill, width=round(width * scale))
        for point in points:
            dot(point, width / 2, fill)

    # The rounded triangle: the triangle grown by `corner` all round.
    triangle = spec["triangle"]
    pen.polygon([at(p) for p in triangle], fill=color)
    stroke(triangle + [triangle[0]], spec["corner"] * 2, color)
    for part in spec["arrow"]:
        stroke(part, spec["stroke"], WHITE)
    return canvas.resize((size, size), Image.LANCZOS)


def png_bytes(image):
    out = io.BytesIO()
    image.save(out, format="PNG", optimize=True)
    return out.getvalue()


def write_ico(path, sizes):
    """An .ico with one PNG per size, as Windows Vista and later read them."""
    images = [png_bytes(draw(size, BLUE)) for size in sizes]
    header = struct.pack("<HHH", 0, 1, len(sizes))
    offset = len(header) + 16 * len(sizes)
    entries = b""
    for size, data in zip(sizes, images):
        dimension = 0 if size >= 256 else size  # 0 means 256
        entries += struct.pack("<BBBBHHII", dimension, dimension, 0, 0, 1, 32, len(data), offset)
        offset += len(data)
    path.write_bytes(header + entries + b"".join(images))


def main():
    write_ico(ROOT / "src" / "resources" / "ldm.ico", [16, 20, 24, 32, 40, 48, 64, 128, 256])
    icons = ROOT / "extension" / "icons"
    for size in (16, 32, 48, 128):
        draw(size, BLUE).save(icons / f"ldm-{size}.png", optimize=True)
    for size in (16, 32):
        draw(size, GREY).save(icons / f"ldm-off-{size}.png", optimize=True)


if __name__ == "__main__":
    main()
