"""Regenerate the original application icon (builds use the checked-in ICO)."""
from pathlib import Path
from PIL import Image, ImageDraw

image = Image.new('RGBA', (256, 256))
draw = ImageDraw.Draw(image)
draw.rounded_rectangle((8, 8, 248, 248), 52, fill='#2056a8')
draw.rounded_rectangle((48, 75, 208, 203), 14, fill='#f2bd62')
draw.polygon([(48, 87), (74, 54), (182, 54), (208, 87)], fill='#ffdaa0')
draw.rectangle((112, 54, 144, 203), fill='#a86a28')
for y in range(61, 177, 19):
    draw.rectangle((115, y, 129, y + 9), fill='#fff6df')
    draw.rectangle((129, y + 9, 141, y + 18), fill='#fff6df')
draw.rounded_rectangle((111, 171, 145, 212), 6, fill='#fff6df')
draw.rounded_rectangle((120, 181, 136, 201), 4, fill='#a86a28')
target = Path(__file__).resolve().parents[1] / 'assets/local-archiver.ico'
target.parent.mkdir(exist_ok=True)
image.save(target, sizes=[(16, 16), (24, 24), (32, 32), (48, 48), (64, 64), (128, 128), (256, 256)])
