"""Render 128x64 monochrome graphics and convert to SSD1306 page bytes."""
import base64
from functools import lru_cache
import re
import textwrap
import unicodedata

from PIL import Image, ImageDraw, ImageFont

FONT = "/usr/share/fonts/truetype/dejavu/DejaVuSansMono.ttf"


@lru_cache(maxsize=48)
def font(size):
    return ImageFont.truetype(FONT, size)


def truth(value, default=False):
    return default if value is None else value in ("true", "1")


def pack(image):
    return bytes(sum((1 << bit) for bit in range(8) if image.getpixel((x, page*8+bit)))
                 for page in range(8) for x in range(128))


def render(element):
    image = Image.new("1", (128, 64), element.get("background", "black") == "white")
    draw = ImageDraw.Draw(image)
    for shape in element:
        attrs = shape.attrib
        color = attrs.get("color", "white") == "white"
        coords = {key: int(value) for key, value in attrs.items()
                  if key in ("x", "y", "x1", "y1", "x2", "y2", "width", "height", "radius", "size")}
        x, y = coords.get("x", 0), coords.get("y", 0)
        if shape.tag == "text":
            value = shape.text or ""
            if any(ord(c) < 32 and c != "\n" for c in value):
                raise ValueError("OLED text contains unsupported control characters")
            draw.multiline_text((x, y), value, font=font(coords.get("size", 10)), fill=color, spacing=0)
        elif shape.tag == "rect":
            box = (x, y, x + coords["width"] - 1, y + coords["height"] - 1)
            draw.rectangle(box, fill=color if truth(attrs.get("fill")) else None, outline=color)
        elif shape.tag == "circle":
            r = coords["radius"]
            draw.ellipse((x-r, y-r, x+r, y+r), fill=color if truth(attrs.get("fill")) else None, outline=color)
        elif shape.tag == "line":
            draw.line((coords["x1"], coords["y1"], coords["x2"], coords["y2"]), fill=color)
        elif shape.tag == "pixel":
            draw.point((x, y), fill=color)
        elif shape.tag == "bitmap":
            data = re.sub(r"\s+", "", shape.text or "")
            try:
                raw = base64.b64decode(data, validate=True) if attrs.get("encoding", "hex") == "base64" else bytes.fromhex(data)
            except (ValueError, base64.binascii.Error):
                raise ValueError("Invalid bitmap encoding") from None
            expected = ((coords["width"] + 7)//8) * coords["height"]
            if len(raw) != expected:
                raise ValueError(f"Bitmap must contain exactly {expected} bytes (row-major, MSB first)")
            bitmap = Image.frombytes("1", (coords["width"], coords["height"]), raw)
            image.paste(bitmap, (x, y))
    return pack(image)


def response_pages(text):
    """Fit a reply into six readable content rows and a page header."""
    value = unicodedata.normalize("NFKD", text).encode("ascii", "replace").decode()
    value = re.sub(r"\[([^\]]+)\]\([^)]*\)", r"\1", value)
    value = value.replace("```", "").replace("**", "").replace("`", "")
    lines = []
    for line in value[:8192].splitlines():
        lines.extend(textwrap.wrap(line, width=25, replace_whitespace=True, drop_whitespace=True) or [""])
    if not lines:
        lines = [""]
    chunks = [lines[i:i+6] for i in range(0, len(lines), 6)][:64]
    pages = []
    for index, chunk in enumerate(chunks):
        image = Image.new("1", (128, 64))
        draw = ImageDraw.Draw(image)
        draw.rectangle((0, 0, 127, 9), fill=1)
        draw.text((2, -1), f"MUSE {index+1}/{len(chunks)}", font=font(8), fill=0)
        for row, line in enumerate(chunk):
            draw.text((1, 11+row*8), line, font=font(8), fill=1)
        pages.append(pack(image))
    return pages


def frame_commands(frame, *, on=True, contrast=160, invert=False):
    if len(frame) != 1024:
        raise ValueError("Expected exactly 1024 OLED page bytes")
    result = [f"OLED CONTRAST {contrast}", f"OLED INVERT {int(invert)}"]
    for offset in range(0, 1024, 48):
        result.append(f"BUFFER {offset} {frame[offset:offset+48].hex()}")
    result.extend(["PRESENT", "OLED ON" if on else "OLED OFF"])
    return result
