"""Convert a photo to an Adafruit_GFX-compatible 1-bit 800x480 bitmap."""

from pathlib import Path
import sys

from PIL import Image, ImageEnhance, ImageOps


WIDTH = 800
HEIGHT = 480


def main() -> None:
    if len(sys.argv) != 4:
        raise SystemExit("usage: convert_photo.py INPUT OUTPUT_HEADER OUTPUT_PREVIEW")

    input_path = Path(sys.argv[1])
    header_path = Path(sys.argv[2])
    preview_path = Path(sys.argv[3])

    with Image.open(input_path) as source:
        source = ImageOps.exif_transpose(source).convert("RGB")
        source.thumbnail((WIDTH, HEIGHT), Image.Resampling.LANCZOS)

        canvas = Image.new("RGB", (WIDTH, HEIGHT), "white")
        offset = ((WIDTH - source.width) // 2, (HEIGHT - source.height) // 2)
        canvas.paste(source, offset)

    grayscale = ImageOps.autocontrast(canvas.convert("L"), cutoff=1)
    grayscale = ImageEnhance.Contrast(grayscale).enhance(1.15)
    monochrome = grayscale.convert("1", dither=Image.Dither.FLOYDSTEINBERG)

    preview_path.parent.mkdir(parents=True, exist_ok=True)
    monochrome.save(preview_path)

    packed = bytearray()
    pixels = monochrome.load()
    for y in range(HEIGHT):
        for x_byte in range(0, WIDTH, 8):
            value = 0
            for bit in range(8):
                if pixels[x_byte + bit, y] == 0:  # black pixel -> set bit
                    value |= 0x80 >> bit
            packed.append(value)

    rows = []
    for offset in range(0, len(packed), 16):
        rows.append("  " + ", ".join(f"0x{value:02X}" for value in packed[offset:offset + 16]))

    header = (
        "#pragma once\n\n"
        "#include <Arduino.h>\n\n"
        f"constexpr uint16_t PHOTO_WIDTH = {WIDTH};\n"
        f"constexpr uint16_t PHOTO_HEIGHT = {HEIGHT};\n\n"
        "const uint8_t PROGMEM PHOTO_BITMAP[] = {\n"
        + ",\n".join(rows)
        + "\n};\n"
    )
    header_path.parent.mkdir(parents=True, exist_ok=True)
    header_path.write_text(header, encoding="ascii")


if __name__ == "__main__":
    main()

