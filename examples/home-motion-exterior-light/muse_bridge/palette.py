"""Compile a user-selected RGB and tunable-white strobe palette."""
from PIL import ImageColor


def percent(value, name):
    if type(value) is not int or not 0 <= value <= 100:
        raise ValueError(f"{name} must be an integer from 0 to 100")
    return value


def strobe_commands(colors, step_us=100000, brightness=70, white_warm=20,
                    white_cool=0, white_palette=None):
    if not isinstance(colors, list) or not 1 <= len(colors) <= 32:
        raise ValueError("colors must be an array of 1..32 RGB colors")
    if type(step_us) is not int or not 50 <= step_us <= 60000000 or step_us % 50:
        raise ValueError("step_us must be a multiple of 50 from 50 to 60000000")
    brightness = percent(brightness, "brightness")
    warm, cool = percent(white_warm, "white_warm"), percent(white_cool, "white_cool")
    if white_palette is not None:
        if not isinstance(white_palette, list) or len(white_palette) != len(colors):
            raise ValueError("white_palette must have one warm/cool object for each RGB color")
        for item in white_palette:
            if not isinstance(item, dict) or set(item) != {"warm", "cool"}:
                raise ValueError("Each white_palette item needs warm and cool percentages")
            percent(item["warm"], "warm"); percent(item["cool"], "cool")
    commands = ["PALETTE CLEAR"]
    for index, color in enumerate(colors):
        if not isinstance(color, str) or not 1 <= len(color) <= 32:
            raise ValueError("Each RGB color must be a name, #RRGGBB, or off")
        try:
            rgb = ImageColor.getrgb("#000000" if color.lower() == "off" else color)
        except ValueError:
            raise ValueError(f"Unknown RGB color: {color}") from None
        if len(rgb) != 3:
            raise ValueError("RGB colors cannot include an alpha channel")
        channels = [round(value*brightness/255) for value in rgb]
        whites = [warm, cool] if white_palette is None else [white_palette[index][key] for key in ("warm", "cool")]
        commands.append(f"PALETTE {index} " + " ".join(map(str, channels+whites)))
    return commands + [f"STROBE {step_us} {len(colors)}"]
