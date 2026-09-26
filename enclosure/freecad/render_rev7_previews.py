"""Render documentation views from Rev7's FreeCAD-exported STL meshes.

Only NumPy and Pillow are required. These are CAD illustrations, not photos
or evidence of a physical fit test.
"""
from pathlib import Path
import math
import struct

import numpy as np
from PIL import Image, ImageDraw, ImageFont

ROOT = Path(__file__).resolve().parents[1] / "rev7"
OUT = ROOT / "images"
OUT.mkdir(exist_ok=True)
MESH_DTYPE = np.dtype([("normal", "<f4", 3), ("vertices", "<f4", (3, 3)), ("attr", "<u2")])


def mesh(name):
    raw = (ROOT / name).read_bytes()
    count = struct.unpack_from("<I", raw, 80)[0]
    triangles = np.frombuffer(raw, MESH_DTYPE, count, 84)
    return triangles["vertices"].astype(np.float64), triangles["normal"].astype(np.float64)


def render(name, file, eye, base_rgb, caption):
    vertices, normals = mesh(file)
    eye = np.asarray(eye, dtype=np.float64)
    eye /= np.linalg.norm(eye)
    right = np.cross(eye, [0, 1, 0]); right /= np.linalg.norm(right)
    up = np.cross(right, eye); up /= np.linalg.norm(up)
    center = (vertices.min(axis=(0, 1)) + vertices.max(axis=(0, 1))) / 2
    vertices -= center
    projected = np.stack((vertices @ right, vertices @ up), axis=-1)
    mins = projected.min(axis=(0, 1)); maxs = projected.max(axis=(0, 1))
    width, height, scale = 1200, 790, 2
    panel = (width * scale, height * scale)
    margin_x, margin_y = 100 * scale, 105 * scale
    factor = min((panel[0] - 2 * margin_x) / (maxs[0] - mins[0]),
                 (panel[1] - 2 * margin_y) / (maxs[1] - mins[1]))
    projected = (projected - (mins + maxs) / 2) * factor
    projected[..., 0] += panel[0] / 2
    projected[..., 1] = panel[1] / 2 - projected[..., 1]
    depth = (vertices @ eye).mean(axis=1)
    light = np.array([0.3, -0.45, 0.84]); light /= np.linalg.norm(light)
    visible = normals @ eye > 0.005
    lightness = np.clip(normals @ light, -1, 1)
    img = Image.new("RGB", panel, (240, 243, 246))
    draw = ImageDraw.Draw(img)
    for index in np.argsort(depth):
        if not visible[index]:
            continue
        shade = 0.70 + 0.27 * max(0.0, lightness[index])
        rgb = tuple(int(min(255, c * shade + 12)) for c in base_rgb)
        points = [tuple(point) for point in projected[index]]
        draw.polygon(points, fill=rgb)
    img = img.resize((width, height), Image.Resampling.LANCZOS)
    draw = ImageDraw.Draw(img)
    try:
        font = ImageFont.truetype("arial.ttf", 25)
    except OSError:
        font = ImageFont.load_default()
    draw.text((34, 30), caption, fill=(35, 45, 55), font=font)
    draw.text((34, height - 42), "Rev7  •  CAD render from FreeCAD STL export  •  fit unverified",
              fill=(85, 95, 105), font=font)
    img.save(OUT / name, optimize=True)


if __name__ == "__main__":
    render("rev7_front.png", "rev7_1_front_shell.stl", (0.32, -0.45, -1.0),
           (218, 63, 48), "Rev7 front shell • front view")
    render("rev7_rear.png", "rev7_1_front_shell.stl", (0.35, -0.5, 1.0),
           (218, 63, 48), "Rev7 front shell • rear / insert posts")
    render("rev7_back_plate.png", "rev7_2_back_plate.stl", (0.35, -0.5, 1.0),
           (74, 105, 126), "Rev7 back plate • screw side")
