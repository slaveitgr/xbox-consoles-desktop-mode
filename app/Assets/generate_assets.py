"""Generate this project's original geometric package artwork (requires Pillow)."""
from pathlib import Path

from PIL import Image, ImageDraw


def make_asset(width: int, height: int, filename: str) -> None:
    scale = 4
    image = Image.new("RGB", (width * scale, height * scale), "#101913")
    draw = ImageDraw.Draw(image)
    unit = min(width, height) * scale
    cx, cy = width * scale / 2, height * scale / 2
    left, top = cx - unit * 0.31, cy - unit * 0.25
    right, bottom = cx + unit * 0.31, cy + unit * 0.17
    stroke = max(3, round(unit * 0.035))
    draw.rounded_rectangle((left, top, right, bottom), radius=unit * 0.045,
                           fill="#1b3021", outline="#7be09b", width=stroke)
    draw.line((cx, bottom, cx, cy + unit * 0.29), fill="#7be09b", width=stroke)
    draw.line((cx - unit * 0.16, cy + unit * 0.29,
               cx + unit * 0.16, cy + unit * 0.29), fill="#7be09b", width=stroke)
    # A small original diagnostic waveform inside the monitor.
    draw.line((cx - unit * 0.21, cy - unit * 0.025,
               cx - unit * 0.10, cy - unit * 0.025,
               cx - unit * 0.045, cy - unit * 0.13,
               cx + unit * 0.035, cy + unit * 0.07,
               cx + unit * 0.10, cy - unit * 0.025,
               cx + unit * 0.21, cy - unit * 0.025),
              fill="#d4f5df", width=stroke, joint="curve")
    image.resize((width, height), Image.Resampling.LANCZOS).save(
        Path(__file__).parent / filename, optimize=True)


if __name__ == "__main__":
    for dimensions in ((44, 44, "Square44x44Logo.png"),
                       (150, 150, "Square150x150Logo.png"),
                       (310, 150, "Wide310x150Logo.png"),
                       (50, 50, "StoreLogo.png"),
                       (620, 300, "SplashScreen.png")):
        make_asset(*dimensions)
