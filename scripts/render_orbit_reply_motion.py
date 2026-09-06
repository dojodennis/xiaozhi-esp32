#!/usr/bin/env python3
"""Render a deterministic before/after preview of Orbit's reply rings.

The left side preserves the removed 0.9-second radial sawtooth. The right side
mirrors the fixed geometry, opacity wave and retained star in crest_motion.h.
This is host-side visual evidence, not a capture of the AMOLED panel.
"""

from __future__ import annotations

import math
from pathlib import Path

from PIL import Image, ImageDraw, ImageFont

from generate_orbit_crest import masks


DISPLAY_SIZE = 466
PANEL_GAP = 28
HEADER_HEIGHT = 46
FPS = 30
DURATION_SECONDS = 4.6
IVORY = (0xE8, 0xE0, 0xD2)
STAR_OPACITY = 224
FIXED_RADII = (120, 154, 184)
ENTER_START = 0.5
EXIT_START = 3.8


def raised_cosine(cycles: float) -> float:
    return 0.5 - 0.5 * math.cos(2.0 * math.pi * cycles)


def smooth_step(phase: float) -> float:
    phase = min(max(phase, 0.0), 1.0)
    return phase * phase * (3.0 - 2.0 * phase)


def smoother_step(phase: float) -> float:
    phase = min(max(phase, 0.0), 1.0)
    return phase**3 * (phase * (phase * 6.0 - 15.0) + 10.0)


def activity(elapsed: float, transition_seconds: float, easing) -> float:
    if elapsed < ENTER_START:
        return 0.0
    if elapsed < ENTER_START + transition_seconds:
        return easing((elapsed - ENTER_START) / transition_seconds)
    if elapsed < EXIT_START:
        return 1.0
    if elapsed < EXIT_START + transition_seconds:
        return 1.0 - easing((elapsed - EXIT_START) / transition_seconds)
    return 0.0


def tint_mask(mask: Image.Image, color: tuple[int, int, int], opacity: int) -> Image.Image:
    alpha = mask.point(lambda value: value * opacity // 255)
    image = Image.new("RGBA", mask.size, (*color, 0))
    image.putalpha(alpha)
    return image


def draw_rings(
    image: Image.Image,
    origin_x: int,
    radii: tuple[int, int, int],
    opacities: tuple[int, int, int],
) -> None:
    overlay = Image.new("RGBA", image.size, (0, 0, 0, 0))
    draw = ImageDraw.Draw(overlay)
    center_x = origin_x + DISPLAY_SIZE // 2
    center_y = HEADER_HEIGHT + DISPLAY_SIZE // 2
    for radius, opacity in zip(radii, opacities):
        bounds = (
            center_x - radius,
            center_y - radius,
            center_x + radius,
            center_y + radius,
        )
        draw.ellipse(bounds, outline=(*IVORY, opacity), width=3)
    image.alpha_composite(overlay)


def speaking_before(elapsed: float, level: float) -> tuple[tuple[int, int, int], tuple[int, int, int]]:
    radii = []
    opacities = []
    for index in range(3):
        phase = (elapsed / 0.9 + index / 3.0) % 1.0
        radii.append(int(116.0 + 70.0 * phase + 8.0 * level))
        opacities.append(int((0.25 + 0.65 * math.sin(math.pi * phase)) * 255.0))
    return tuple(radii), tuple(opacities)


def speaking_after(elapsed: float, level: float) -> tuple[tuple[int, int, int], tuple[int, int, int]]:
    opacities = []
    for index in range(3):
        wave = raised_cosine(elapsed / 2.2 - index * 0.16)
        opacities.append(int((0.30 + 0.36 * wave + 0.18 * level) * 255.0))
    return FIXED_RADII, tuple(opacities)


def render_frame(elapsed: float, band: Image.Image, star: Image.Image) -> Image.Image:
    width = DISPLAY_SIZE * 2 + PANEL_GAP
    frame = Image.new("RGBA", (width, DISPLAY_SIZE + HEADER_HEIGHT), (0, 0, 0, 255))
    draw = ImageDraw.Draw(frame)
    font = ImageFont.load_default()
    draw.text(
        (DISPLAY_SIZE // 2, 16),
        "BEFORE - radial reset",
        fill=(*IVORY, 255),
        font=font,
        anchor="mm",
    )
    draw.text(
        (DISPLAY_SIZE + PANEL_GAP + DISPLAY_SIZE // 2, 16),
        "AFTER - fixed geometry",
        fill=(*IVORY, 255),
        font=font,
        anchor="mm",
    )
    divider_x = DISPLAY_SIZE + PANEL_GAP // 2
    draw.line((divider_x, 0, divider_x, frame.height), fill=(55, 55, 55, 255))

    level = 0.65
    active_elapsed = max(elapsed - ENTER_START, 0.0)
    before_activity = activity(elapsed, 0.22, smooth_step)
    after_activity = activity(elapsed, 0.36, smoother_step)
    before_radii, before_opacities = speaking_before(active_elapsed, level)
    after_radii, after_opacities = speaking_after(active_elapsed, level)
    before_opacities = tuple(round(value * before_activity) for value in before_opacities)
    after_opacities = tuple(round(value * after_activity) for value in after_opacities)

    # Match LVGL stacking: outer crest below rings, center star above them.
    panels = ((0, before_activity), (DISPLAY_SIZE + PANEL_GAP, after_activity))
    for origin_x, active in panels:
        crest = tint_mask(band, IVORY, round(255 * (1.0 - active)))
        frame.alpha_composite(crest, (origin_x + 41, HEADER_HEIGHT + 41))
    draw_rings(frame, 0, before_radii, before_opacities)
    draw_rings(frame, DISPLAY_SIZE + PANEL_GAP, after_radii, after_opacities)
    before_star = tint_mask(star, IVORY, round(255 * (1.0 - before_activity)))
    after_star_alpha = round(255 + (STAR_OPACITY - 255) * after_activity)
    after_star = tint_mask(star, IVORY, after_star_alpha)
    frame.alpha_composite(before_star, (41, HEADER_HEIGHT + 41))
    frame.alpha_composite(after_star, (DISPLAY_SIZE + PANEL_GAP + 41, HEADER_HEIGHT + 41))
    return frame.convert("P", palette=Image.Palette.ADAPTIVE, colors=96)


def make_storyboard(frames: list[Image.Image], output: Path) -> None:
    # Straddle the old 0.9-second wrap so the one-frame geometry jump is visible.
    sample_times = tuple(ENTER_START + time for time in (0.833, 0.866, 0.900, 0.933))
    samples = [
        frames[min(round(time * FPS), len(frames) - 1)].convert("RGBA")
        for time in sample_times
    ]
    scale = 0.48
    sample_size = (round(samples[0].width * scale), round(samples[0].height * scale))
    sheet = Image.new("RGB", (sample_size[0] * 2, sample_size[1] * 2), "black")
    for index, sample in enumerate(samples):
        sample = sample.resize(sample_size, Image.Resampling.LANCZOS).convert("RGB")
        sheet.paste(sample, ((index % 2) * sample_size[0], (index // 2) * sample_size[1]))
    sheet.save(output, optimize=True)


def main() -> None:
    output_dir = Path(__file__).resolve().parents[1] / "docs"
    output_dir.mkdir(exist_ok=True)
    band, star = masks()
    frames = [
        render_frame(index / FPS, band, star)
        for index in range(round(DURATION_SECONDS * FPS))
    ]
    gif = output_dir / "orbit-reply-ring-motion-before-after.gif"
    frames[0].save(
        gif,
        save_all=True,
        append_images=frames[1:],
        duration=round(1000 / FPS),
        loop=0,
        optimize=True,
        disposal=2,
    )
    make_storyboard(frames, output_dir / "orbit-reply-ring-motion-storyboard.png")
    print(gif)


if __name__ == "__main__":
    main()
