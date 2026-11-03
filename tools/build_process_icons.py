"""Build the Project Chronicles process icons and installer artwork from geometric brand shapes."""

from pathlib import Path
import math

from PIL import Image, ImageDraw, ImageFont


ROOT = Path(__file__).resolve().parents[1]
OUTPUT = ROOT / "resources"
GOLD = "#dbbd79"
BACKGROUND = "#101d2b"


def draw_compass(draw, center, radius, color):
    x, y = center
    width = max(1, round(radius / 22))
    draw.ellipse((x - radius, y - radius, x + radius, y + radius), outline=color, width=width)
    for index in range(4):
        angle = index * math.pi / 2
        tip = (x + math.sin(angle) * radius * 1.17, y - math.cos(angle) * radius * 1.17)
        left = (x + math.sin(angle - 0.48) * radius * 0.28,
                y - math.cos(angle - 0.48) * radius * 0.28)
        right = (x + math.sin(angle + 0.48) * radius * 0.28,
                 y - math.cos(angle + 0.48) * radius * 0.28)
        draw.polygon((tip, left, (x, y), right), fill=color)
    draw.ellipse((x - radius * 0.1, y - radius * 0.1,
                  x + radius * 0.1, y + radius * 0.1), fill=color)


def process_icon(role):
    image = Image.new("RGBA", (256, 256), (0, 0, 0, 0))
    draw = ImageDraw.Draw(image)
    draw.rounded_rectangle((8, 8, 248, 248), radius=48, fill=BACKGROUND,
                           outline=GOLD, width=5)
    draw_compass(draw, (128, 118), 76, GOLD)
    if role == "admin":
        draw.polygon(((165, 145), (222, 158), (217, 205), (193, 229), (169, 205)),
                     fill="#286d93", outline=GOLD, width=4)
        draw.line(((179, 186), (190, 197), (210, 173)), fill="#ffffff", width=7)
    elif role == "server":
        for top in (157, 183, 209):
            draw.rounded_rectangle((156, top, 230, top + 20), radius=5,
                                   fill="#386b58", outline=GOLD, width=2)
            draw.ellipse((216, top + 7, 222, top + 13), fill="#d7ffd8")
    elif role == "helper":
        draw.ellipse((168, 168, 230, 230), fill="#4c527f", outline=GOLD, width=3)
        draw.line(((181, 200), (195, 213), (219, 183)), fill="#ffffff", width=6)
    return image


def main():
    OUTPUT.mkdir(exist_ok=True)
    for role in ("launcher", "admin", "server", "helper"):
        image = process_icon(role)
        image.save(OUTPUT / f"chronicles-{role}.ico", sizes=[(16, 16), (24, 24), (32, 32),
                                                     (48, 48), (64, 64), (128, 128), (256, 256)])

    artwork = Image.new("RGB", (164, 314))
    draw = ImageDraw.Draw(artwork)
    for y in range(314):
        ratio = y / 313
        draw.line((0, y, 164, y), fill=(round(13 + 8 * ratio),
                                       round(28 + 9 * ratio), round(43 + 9 * ratio)))
    draw_compass(draw, (82, 99), 48, GOLD)
    font_path = "C:/Windows/Fonts/seguisb.ttf"
    draw.text((82, 165), "PROJECT", font=ImageFont.truetype(font_path, 14), fill=GOLD, anchor="mm")
    draw.text((82, 185), "CHRONICLES", font=ImageFont.truetype(font_path, 19), fill=GOLD, anchor="mm")
    small = ImageFont.truetype(font_path, 10)
    draw.text((82, 210), "YOUR NEXT CHAPTER", font=small, fill="#c9d2da", anchor="mm")
    draw.line((27, 237, 137, 237), fill="#716246", width=1)
    draw.text((82, 269), "LAUNCHER 0.2.0", font=small, fill=GOLD, anchor="mm")
    artwork.save(OUTPUT / "installer-welcome.bmp")


if __name__ == "__main__":
    main()
