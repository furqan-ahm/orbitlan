#!/usr/bin/env python3
"""Bake OrbitLan's rotating Earth and atmospheric cloud bands into one GIF."""

from __future__ import annotations

import math
import random
import shutil
from dataclasses import dataclass
from pathlib import Path

from PIL import Image, ImageDraw


ROOT = Path(__file__).resolve().parents[1]
SOURCE_SHEET = ROOT / "client/ui/assets/earthspin-sheet.png"
OUTPUT_GIF = ROOT / "client/ui/assets/earth-clouds.gif"
WEB_OUTPUT_GIF = ROOT / "website/assets/earth-clouds.gif"
HEADER_SHEET = ROOT / "client/ui/assets/earth-header-sheet.png"

SOURCE_SIZE = 240
SOURCE_COLUMNS = 10
FRAME_COUNT = 94
CANVAS_SIZE = 286
EARTH_SIZE = 210


@dataclass(frozen=True)
class Puff:
    progress: float
    density: float
    size: float
    lift: float
    side: int


@dataclass(frozen=True)
class Band:
    offset_y: float
    radius_x: float
    radius_y: float
    clusters: int
    phase_per_loop: float
    spacing: float
    base_size: float
    puffs: tuple[Puff, ...]
    front_color: str
    back_color: str


def make_band(
    rng: random.Random,
    *,
    offset_y: float,
    radius_x: float,
    radius_y: float,
    clusters: int,
    phase_per_loop: float,
    segments: int,
    base_size: float,
    front_color: str,
    back_color: str,
) -> Band:
    dense_at = 0.31 + rng.random() * 0.08
    dense_at_2 = 0.70 + rng.random() * 0.08
    puffs: list[Puff] = []
    for segment in range(segments):
        progress = segment / max(1, segments - 1)
        density = min(
            1.0,
            0.18
            + max(
                math.exp(-((progress - dense_at) / 0.18) ** 2),
                math.exp(-((progress - dense_at_2) / 0.14) ** 2),
            )
            * 0.72
            + rng.random() * 0.14,
        )
        puffs.append(
            Puff(
                progress=progress,
                density=density,
                size=0.76 + rng.random() * 0.58,
                lift=(rng.random() - 0.5) * 2.0,
                side=-1 if rng.random() < 0.5 else 1,
            )
        )
    return Band(
        offset_y=offset_y,
        radius_x=radius_x,
        radius_y=radius_y,
        clusters=clusters,
        phase_per_loop=phase_per_loop,
        spacing=0.043,
        base_size=base_size,
        puffs=tuple(puffs),
        front_color=front_color,
        back_color=back_color,
    )


def cloud_rectangles(band: Band, phase: float, front: bool):
    center = CANVAS_SIZE / 2
    for cluster in range(band.clusters):
        start = cluster * math.tau / band.clusters + phase
        for index, puff in enumerate(band.puffs):
            angle = start - index * band.spacing
            sine = math.sin(angle)
            if (sine >= 0) != front:
                continue

            depth = (sine + 1) / 2
            perspective = 0.72 + depth * 0.36
            envelope = 0.34 + math.sin(puff.progress * math.pi) * 0.66
            thickness = max(
                3.0,
                band.base_size * puff.size * envelope * perspective * (0.72 + puff.density * 0.38),
            )
            length = thickness * (1.18 + puff.density * 0.72 + puff.size * 0.16)
            x = center + math.cos(angle) * band.radius_x
            y = center + band.offset_y + sine * band.radius_y + puff.lift * envelope
            yield (
                round(x - length / 2),
                round(y - thickness / 2),
                round(x + length / 2),
                round(y + thickness / 2),
                puff,
                thickness,
                length,
            )


def draw_cloud_layer(image: Image.Image, band: Band, phase: float, front: bool) -> None:
    draw = ImageDraw.Draw(image)
    color = band.front_color if front else band.back_color
    for left, top, right, bottom, puff, thickness, length in cloud_rectangles(band, phase, front):
        draw.rectangle((left, top, right, bottom), fill=color)
        if puff.density > 0.57 and puff.size > 0.9:
            accent_width = length * 0.52
            accent_height = max(3.0, thickness * (0.4 + puff.density * 0.2))
            accent_left = left + length * 0.30
            accent_top = (top + bottom) / 2 - thickness * (0.38 + puff.side * 0.55)
            draw.rectangle(
                (
                    round(accent_left),
                    round(accent_top),
                    round(accent_left + accent_width),
                    round(accent_top + accent_height),
                ),
                fill=color,
            )


def main() -> None:
    rng = random.Random(9261)
    bands = (
        make_band(
            rng,
            offset_y=-47,
            radius_x=103,
            radius_y=17,
            clusters=3,
            phase_per_loop=4 * math.pi / 3,
            segments=21,
            base_size=7,
            front_color="#E9EFFF",
            back_color="#AEBBDB",
        ),
        make_band(
            rng,
            offset_y=-1,
            radius_x=116,
            radius_y=27,
            clusters=2,
            phase_per_loop=-math.pi,
            segments=24,
            base_size=7,
            front_color="#F7F8FF",
            back_color="#BBC7E4",
        ),
        make_band(
            rng,
            offset_y=44,
            radius_x=104,
            radius_y=17,
            clusters=2,
            phase_per_loop=math.pi,
            segments=20,
            base_size=7,
            front_color="#DCE5FF",
            back_color="#9EADD2",
        ),
    )

    sheet = Image.open(SOURCE_SHEET).convert("RGBA")
    frames: list[Image.Image] = []
    header_frames: list[Image.Image] = []
    earth_offset = (CANVAS_SIZE - EARTH_SIZE) // 2
    for frame_index in range(FRAME_COUNT):
        source_frame = sheet.crop(
            (
                frame_index % SOURCE_COLUMNS * SOURCE_SIZE,
                frame_index // SOURCE_COLUMNS * SOURCE_SIZE,
                frame_index % SOURCE_COLUMNS * SOURCE_SIZE + SOURCE_SIZE,
                frame_index // SOURCE_COLUMNS * SOURCE_SIZE + SOURCE_SIZE,
            )
        )
        source = source_frame.resize((EARTH_SIZE, EARTH_SIZE), Image.Resampling.NEAREST)
        header_frames.append(source_frame.resize((48, 48), Image.Resampling.NEAREST))

        frame = Image.new("RGBA", (CANVAS_SIZE, CANVAS_SIZE), (0, 0, 0, 0))
        progress = frame_index / FRAME_COUNT
        for band in bands:
            draw_cloud_layer(frame, band, progress * band.phase_per_loop, front=False)
        frame.alpha_composite(source, (earth_offset, earth_offset))
        for band in bands:
            draw_cloud_layer(frame, band, progress * band.phase_per_loop, front=True)
        frames.append(frame)

    frames[0].save(
        OUTPUT_GIF,
        save_all=True,
        append_images=frames[1:],
        duration=100,
        loop=0,
        disposal=2,
        optimize=False,
        transparency=0,
    )
    shutil.copyfile(OUTPUT_GIF, WEB_OUTPUT_GIF)
    header_sheet = Image.new("RGBA", (480, 480), (0, 0, 0, 0))
    for index, frame in enumerate(header_frames):
        header_sheet.alpha_composite(frame, ((index % 10) * 48, (index // 10) * 48))
    header_sheet.save(HEADER_SHEET, optimize=True)
    print(f"wrote {OUTPUT_GIF} ({FRAME_COUNT} frames, {CANVAS_SIZE}x{CANVAS_SIZE})")
    print(f"copied {WEB_OUTPUT_GIF}")
    print(f"wrote {HEADER_SHEET} (plain Earth header atlas)")


if __name__ == "__main__":
    main()
