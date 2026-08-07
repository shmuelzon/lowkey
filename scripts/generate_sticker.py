#!/usr/bin/env python3
"""Render the label that goes on the back of a LowKey lock.

The sticker carries everything needed to bring a unit up without opening a
terminal: the Matter onboarding card -- logo, commissioning QR and manual
pairing code, framed the way Matter's own onboarding artwork is -- alongside
the device's identity: advertised BLE name, serial number and BLE pairing
passkey.

Matter R1.4 section 5.1.2 hands the composition of that card ("colors, font,
font size, QR Code size and digit-grouping of the Manual Pairing code") to the
Matter Brand Guidelines, which are a CSA members-only document. What is
reproduced here follows the spec's own manual-code example (4-3-4 grouping,
5.1.4.2) and the published onboarding artwork, but it is NOT a certified
rendition -- see the notes on the font constants below before shipping product.

generate_factory_data.py calls render_sticker() as part of provisioning a
device, but this file also runs on its own, which is how you reprint a label
for a unit that was provisioned earlier:

    python3 scripts/generate_sticker.py factory_data

It reads <prefix>.json for the identity and <prefix>.txt for the onboarding
codes (the passcode never reaches the .json, so the codes only exist there),
and writes <prefix>.sticker.png.

--size gives the output resolution in pixels; the layout is proportional, so
any size works and nothing is hardcoded to a particular label or printer. A
landscape image puts the onboarding card beside the identity, a portrait one
stacks it above -- see render_sticker(). Print it from whatever the printer's
own app is; feed that a generous resolution and let it scale down, since a
crisp source survives its resampling far better than one rendered at the exact
head resolution.

Prerequisites:
    pip install pillow qrcode
"""

import argparse
import json
import os
import sys

SCRIPT_DIR = os.path.dirname(os.path.abspath(__file__))

# Rasterised once at 2048 px from the Wikimedia Commons SVG "Logo of Matter
# connectivity standard", because nothing in the nRF Connect SDK's Python
# environment renders SVG and this script should not need a new dependency to
# draw a label. 2048 px is several times any width the lockup will ever be
# printed at, so downscaling it is visually indistinguishable from rendering
# the vector directly. The image is cropped tight to the ink -- the layout
# below relies on there being no built-in padding.
MATTER_LOGO = os.path.join(SCRIPT_DIR, "assets", "matter_logo.png")

# Grey above this survives as white when the lockup is reduced to 1 bit. Set
# well above the midpoint on purpose: at label scale the wordmark's strokes are
# barely a pixel wide, and a 50% cut drops half of them.
LOGO_THRESHOLD = 200

# Comfortably above any label printer's own resolution, so the app it is fed
# to has room to scale down rather than up.
DEFAULT_SIZE = (640, 480)

# Every string here is fitted by width -- the manual pairing code has to sit
# inside the QR's width -- so what decides how tall the printed digits come out
# is the ratio of digit height to advance width, not the point size. Measured
# across the monospaced faces the distro ships, at the width the manual code
# has to fit at the default resolution:
#
#     Ubuntu Mono             15 px   (1.41 x advance)
#     Noto Sans Mono          13 px   (1.20)   <- in use
#     DejaVu Sans Mono        13 px   (1.20)
#     Nimbus Mono PS          11 px   (1.02)   Courier's proportions
#
# Ubuntu Mono is the tallest of them; switch FONT_REGULAR/FONT_BOLD to
# "UbuntuMono[wght].ttf" with weights 400/700 if the manual code needs the
# extra 15%. The Brand Guidelines name a specific typeface for the manual
# pairing code that is not reproduced here; point these at it if you have the
# guidelines and a licence for the font.
#
# The second element of each pair is a variable-font weight axis, or None for a
# face that ships its own weights as separate files -- see _font().
FONT_DIR = "/usr/share/fonts/truetype/noto"
FONT_REGULAR = (os.path.join(FONT_DIR, "NotoSansMono-Regular.ttf"), None)
FONT_BOLD = (os.path.join(FONT_DIR, "NotoSansMono-Bold.ttf"), None)

BLACK, WHITE = 0, 255

# Modules of white the QR carries with it. ISO/IEC 18004 asks for four; the
# card's padding and the gaps to the logo and code make up the rest, which is
# also what keeps the frame from crowding the code.
QR_QUIET_MODULES = 2

# How much of the usable space the Matter card may claim -- by width when it
# sits beside the identity, by height when it sits above. It takes less if its
# contents do not need it (the card sizes itself to whole QR modules), so these
# are ceilings, not sizes.
CARD_WIDTH_FRACTION = 0.54
CARD_HEIGHT_FRACTION = 0.66


def parse_size(text):
    """Parse a WxH pixel size, e.g. "640x480"."""
    try:
        w, h = (int(part) for part in text.lower().split("x", 1))
    except ValueError:
        raise argparse.ArgumentTypeError(
            f"expected WIDTHxHEIGHT in pixels, got {text!r}")
    if w < 64 or h < 48:
        raise argparse.ArgumentTypeError(
            f"{text} is too small to lay out a readable label")
    return w, h


def format_manual_code(code):
    """Group the manual pairing code the way Matter displays it.

    The spec's own example (5.1.4.2) writes the 11-digit code as "1234-567-8910",
    so use 4-3-4. The 21-digit long form has no published grouping; fall back to
    fours rather than inventing one.
    """
    digits = "".join(c for c in code if c.isdigit())
    if len(digits) == 11:
        return f"{digits[:4]}-{digits[4:7]}-{digits[7:]}"
    return "-".join(digits[i:i + 4] for i in range(0, len(digits), 4))


def read_onboarding(path):
    """Pull (manual_code, qr_payload) out of the SDK's onboarding .txt."""
    manual = qr = None
    with open(path) as f:
        for line in f:
            key, sep, value = line.partition(":")
            if not sep:
                continue
            key, value = key.strip().lower(), value.strip()
            if key == "manualcode":
                manual = value
            elif key == "qrcode":
                qr = value
    return manual, qr


def _font(spec, size):
    """Load (path, weight). Weight is a variable-font axis, not a separate file."""
    from PIL import ImageFont

    path, weight = spec
    font = ImageFont.truetype(path, max(size, 1))
    if weight is not None:
        try:
            font.set_variation_by_axes([weight])
        except OSError:
            pass  # Not a variable font; the file's own weight stands.
    return font


# Text is measured and drawn top-anchored. With Pillow's default ascender
# anchor the ink starts several pixels below the draw point, so a row consumes
# more height than it measures -- which silently pushed the manual pairing code
# off the bottom of an unframed card.
TEXT_ANCHOR = "lt"
TEXT_ANCHOR_CENTRED = "mt"


def _text_size(draw, text, font):
    left, top, right, bottom = draw.textbbox(
        (0, 0), text, font=font, anchor=TEXT_ANCHOR)
    return right - left, bottom - top


def _fit_font(draw, text, spec, max_w, max_h):
    """Largest size at which `text` fits in a max_w x max_h box."""
    size = max_h
    while size > 1:
        font = _font(spec, size)
        w, h = _text_size(draw, text, font)
        if w <= max_w and h <= max_h:
            return font
        size -= 1
    return _font(spec, 1)


def _draw_text(draw, xy, text, font, anchor=TEXT_ANCHOR):
    draw.text(xy, text, font=font, fill=BLACK, anchor=anchor)


def _render_logo(width):
    """Scale the Matter lockup to `width` pixels, as pure black on white."""
    from PIL import Image

    with Image.open(MATTER_LOGO) as master:
        logo = master.convert("L").resize(
            (width, max(1, round(width * master.height / master.width))),
            Image.LANCZOS,
        )
    return logo.point(lambda v: WHITE if v > LOGO_THRESHOLD else BLACK)


def _encode_qr(payload):
    """Encode `payload` and return (code, modules across including quiet zone)."""
    import qrcode

    qr = qrcode.QRCode(border=QR_QUIET_MODULES, box_size=1)
    qr.add_data(payload)
    qr.make(fit=True)
    return qr, qr.modules_count + 2 * QR_QUIET_MODULES


def _render_qr(qr, box):
    """Render an encoded QR at `box` pixels per module.

    Module size is always whole pixels and the image is never scaled
    afterwards: a QR that has been through a resampling filter prints as grey
    mush on a 1-bit thermal head.
    """
    qr.box_size = box
    return qr.make_image(fill_color="black", back_color="white").convert("L")


def _render_matter_card(width, height, manual_code, qr_payload, frame, unit):
    """Return the onboarding card as an image that fits within width x height.

    The QR sets the card's width: it is the one element that cannot be scaled
    freely (its modules have to be whole pixels), so everything else lines up
    to it and the card ends up only as wide as it needs to be. `unit` is the
    scale the rest of the label is proportioned against.
    """
    from PIL import Image, ImageDraw

    stroke = max(1, round(0.010 * unit)) if frame else 0
    pad = round(0.025 * unit) if frame else 0
    budget_w = width - 2 * (stroke + pad)
    budget_h = height - 2 * (stroke + pad)

    scratch = ImageDraw.Draw(Image.new("L", (1, 1)))
    code_text = format_manual_code(manual_code)
    gap = round(0.035 * height)
    qr_code, modules = _encode_qr(qr_payload)

    # Walk the module size down until the whole stack fits. Everything here
    # follows from the module size -- the logo lines up with the QR's dark
    # edges, the manual code is fitted to the QR's width -- so trying the
    # candidates in order is exact. Estimating the stack height instead lands a
    # pixel or two off and silently costs a whole module of QR.
    for box in range(max(1, budget_w // modules), 0, -1):
        qr = _render_qr(qr_code, box)
        # Line the lockup up with the QR's dark edges rather than its quiet
        # zone -- matching the code's own width is what makes the card read as
        # one block.
        logo = _render_logo(qr.width - 2 * QR_QUIET_MODULES * box)
        code_font = _fit_font(
            scratch, code_text, FONT_BOLD, qr.width, round(0.11 * height))
        code_w, code_h = _text_size(scratch, code_text, code_font)
        if logo.height + gap + qr.height + gap + code_h <= budget_h:
            break

    content_w = max(qr.width, logo.width, code_w)
    card = Image.new(
        "L",
        (content_w + 2 * (stroke + pad),
         logo.height + gap + qr.height + gap + code_h + 2 * (stroke + pad)),
        WHITE,
    )
    draw = ImageDraw.Draw(card)
    if frame:
        draw.rounded_rectangle(
            (0, 0, card.width - 1, card.height - 1),
            radius=round(0.05 * unit), outline=BLACK, width=stroke,
        )

    cx = card.width // 2
    y = stroke + pad
    card.paste(logo, (cx - logo.width // 2, y))
    y += logo.height + gap
    card.paste(qr, (cx - qr.width // 2, y))
    y += qr.height + gap
    _draw_text(draw, (cx, y), code_text, code_font,
               anchor=TEXT_ANCHOR_CENTRED)
    return card


def _draw_info(draw, x, y, width, height, ble_name, sn, ble_passkey):
    """Draw the identity block into the width x height box at (x, y).

    Type sizes are fractions of the box, not of the label, so the block works
    both as a tall column beside the card and as a short band beneath it.
    """
    name_font = _fit_font(draw, ble_name, FONT_BOLD, width, round(0.15 * height))
    name_h = _text_size(draw, ble_name, name_font)[1]

    rows = [("SERIAL", sn, FONT_REGULAR), ("BLE PIN", ble_passkey, FONT_BOLD)]
    row_fonts = []
    for label, value, value_spec in rows:
        value_font = _fit_font(draw, value, value_spec, width,
                               round(0.115 * height))
        label_font = _fit_font(draw, label, FONT_REGULAR, width,
                               round(0.075 * height))
        # A heading must never outgrow the value it heads. Both are fitted to
        # the same width, so on a box that is wide for its height the shorter
        # heading wins the fit and the hierarchy inverts -- "SERIAL" set larger
        # than the serial number. Clamp only when that actually happens, so
        # ordinary proportions keep the size the cap above gives them.
        value_h = _text_size(draw, value, value_font)[1]
        if _text_size(draw, label, label_font)[1] > value_h:
            label_font = _fit_font(draw, label, FONT_REGULAR, width, value_h)
        row_fonts.append((label_font, value_font))

    rule_gap = round(0.045 * height)
    label_gap = round(0.01 * height)
    row_gap = round(0.055 * height)

    _draw_text(draw, (x, y), ble_name, name_font)
    y += name_h + rule_gap
    draw.line((x, y, x + width, y), fill=BLACK,
              width=max(1, round(0.006 * height)))
    y += rule_gap

    for (label, value, _), (label_font, value_font) in zip(rows, row_fonts):
        _draw_text(draw, (x, y), label, label_font)
        y += _text_size(draw, label, label_font)[1] + label_gap
        _draw_text(draw, (x, y), value, value_font)
        y += _text_size(draw, value, value_font)[1] + row_gap


def render_sticker(
    path,
    sn,
    ble_name,
    ble_passkey,
    manual_code=None,
    qr_payload=None,
    size=DEFAULT_SIZE,
    frame=True,
):
    """Draw the label at `size` and write it to `path` as a 1-bit PNG.

    The two blocks are placed side by side on a landscape image and stacked on
    a portrait one, which is what lets one layout cover any aspect ratio. With
    no manual_code/qr_payload -- a --no-matter build has neither -- the
    onboarding card is dropped and the identity takes the whole label.
    """
    from PIL import Image, ImageDraw

    width, height = size
    img = Image.new("L", size, WHITE)
    draw = ImageDraw.Draw(img)

    # Everything below is a fraction of the label's short side or of the box it
    # is drawn into, so a different resolution rescales the design rather than
    # clipping it.
    unit = min(width, height)
    margin = max(2, round(0.035 * unit))
    inner_w = width - 2 * margin
    inner_h = height - 2 * margin

    if manual_code is None or qr_payload is None:
        _draw_info(draw, margin, margin, inner_w, inner_h, ble_name, sn,
                   ble_passkey)
    elif height > width:
        card = _render_matter_card(
            inner_w, round(CARD_HEIGHT_FRACTION * inner_h),
            manual_code, qr_payload, frame, unit)
        img.paste(card, (margin + (inner_w - card.width) // 2, margin))
        info_y = margin + card.height + round(0.045 * height)
        _draw_info(draw, margin, info_y, inner_w, height - margin - info_y,
                   ble_name, sn, ble_passkey)
    else:
        card = _render_matter_card(
            round(CARD_WIDTH_FRACTION * inner_w), inner_h,
            manual_code, qr_payload, frame, unit)
        # Hang the text from the card's top edge rather than centring it, so
        # the name and the lockup start on the same line.
        card_y = margin + (inner_h - card.height) // 2
        img.paste(card, (margin, card_y))
        info_x = margin + card.width + round(0.045 * width)
        _draw_info(draw, info_x, card_y, width - margin - info_x,
                   height - margin - card_y, ble_name, sn, ble_passkey)

    # A thermal head is 1-bit anyway, and thresholding here rather than leaving
    # it to whatever consumes the file keeps the text and the frame from
    # breaking up.
    img.point(lambda v: WHITE if v > 127 else BLACK).convert("1").save(path)
    return path


def main():
    parser = argparse.ArgumentParser(
        description=__doc__,
        formatter_class=argparse.RawDescriptionHelpFormatter,
    )
    parser.add_argument(
        "prefix", nargs="?", default="factory_data",
        help="factory data output prefix to read (default: factory_data)",
    )
    parser.add_argument(
        "-o", "--output", default=None,
        help="sticker path (default: <prefix>.sticker.png)",
    )
    parser.add_argument(
        "-s", "--size", type=parse_size, default=DEFAULT_SIZE,
        metavar="WxH",
        help="output resolution in pixels; the layout adapts to any size and "
             "aspect ratio (default: %dx%d)" % DEFAULT_SIZE,
    )
    parser.add_argument(
        "--no-frame", dest="frame", action="store_false",
        help="drop the frame around the Matter onboarding card",
    )
    args = parser.parse_args()

    json_path = args.prefix + ".json"
    if not os.path.isfile(json_path):
        sys.exit(f"Error: {json_path} not found")
    with open(json_path) as f:
        data = json.load(f)

    manual = qr = None
    txt_path = args.prefix + ".txt"
    if os.path.isfile(txt_path):
        manual, qr = read_onboarding(txt_path)

    # ble_advertised_name() lives in the generator; importing it keeps the two
    # from drifting apart, but this script has to stand alone if that import
    # fails (the generator reaches for the SDK tooling at module scope).
    try:
        from generate_factory_data import ble_advertised_name
    except ImportError:
        def ble_advertised_name(sn):
            return f"LowKey-{sn[-4:]}" if len(sn) >= 4 else "LowKey"

    out = args.output or (args.prefix + ".sticker.png")
    render_sticker(
        out, data["sn"], ble_advertised_name(data["sn"]),
        data.get("user", {}).get("ble_passkey", ""),
        manual_code=manual, qr_payload=qr, size=args.size, frame=args.frame,
    )
    print(f"Wrote {out} ({args.size[0]}x{args.size[1]} px)")


if __name__ == "__main__":
    main()
