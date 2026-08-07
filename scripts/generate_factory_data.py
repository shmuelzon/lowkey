#!/usr/bin/env python3
"""Generate the LowKey factory data partition.

One blob serves both radios. The firmware reads it with the nRF Connect SDK
factory data parser (FactoryDataParser.c) either way: Matter consumes the
standard top-level keys, and the application reads its own per-device values
out of the CBOR "user" map (see src/factory_data.c).

Every argument has a default, so provisioning a development device is:

    python3 scripts/generate_factory_data.py -o factory_data

Values that must differ between devices -- the serial number, the BLE pairing
passkey, the Matter passcode, the commissioning discriminator and the SPAKE2+
salt -- are randomly drawn when not given. The generated <output>.json is the
ONLY record of them: the passcode is stored on the device solely as a one-way
SPAKE2+ verifier and cannot be read back. Archive it per device.

Outputs (<output> defaults to "factory_data"):
    <output>.json           the factory data set, including the drawn secrets
    <output>.hex            the partition image, at the offset/size from pm_static.yml
    <output>.png            commissioning QR code
    <output>.txt            manual pairing code
    <output>.sticker.png    the printable label (see scripts/generate_sticker.py)

Prerequisites:
    pip install cbor2 intelhex jsonschema pillow pyyaml qrcode
"""

import argparse
import base64
import datetime
import json
import logging
import os
import re
import secrets
import subprocess
import sys

SCRIPT_DIR = os.path.dirname(os.path.abspath(__file__))
PROJECT_DIR = os.path.dirname(SCRIPT_DIR)
sys.path.insert(0, SCRIPT_DIR)

import generate_sticker  # noqa: E402  (needs SCRIPT_DIR on the path first)

NCS_ROOT = os.environ.get("NCS_ROOT", os.path.expanduser("~/ncs/v3.2.4"))
MATTER_TOOLS_DIR = os.path.join(
    NCS_ROOT, "modules", "lib", "matter", "scripts", "tools", "nrfconnect"
)
SDK_GENERATOR = os.path.join(
    MATTER_TOOLS_DIR, "generate_nrfconnect_chip_factory_data.py"
)
TEST_CERTS_DIR = os.path.join(
    NCS_ROOT, "modules", "lib", "matter", "credentials", "test", "attestation"
)

PM_STATIC = os.path.join(PROJECT_DIR, "pm_static.yml")
SCHEMA = os.path.join(SCRIPT_DIR, "factory_data.schema")

# Matter test vendor/product. The bundled DAC/PAI test certificates below are
# issued for exactly this pair, which is why overriding one without the other
# is worth warning about.
DEFAULT_VENDOR_ID = 0xFFF1
DEFAULT_PRODUCT_ID = 0x8000

DEFAULT_VENDOR_NAME = "Shmuelzon"
DEFAULT_PRODUCT_NAME = "LowKey"
DEFAULT_HW_VER = 1
DEFAULT_HW_VER_STR = "1.0"
DEFAULT_BATTERY_CAPACITY_MAH = 5000

# Mirrors CONFIG_BT_DEVICE_NAME in Kconfig. Only used to print the name the
# firmware will advertise, so a sticker can be prepared without reading it back
# off the device -- build_adv_data() in src/ble/ble_radio.c appends the last
# four characters of the serial below to whatever the GAP name is.
BLE_NAME_BASE = "LowKey"

# SPAKE2+ iteration count. Not per-device -- it is a work factor, and the
# schema allows 1000..100000.
DEFAULT_SPAKE2_IT = 1000

# Matter requires a 16..32 byte salt. The schema's 68-character cap on the
# hex-encoded form puts the ceiling at exactly 32 bytes, so take it.
SPAKE2_SALT_BYTES = 32

# Passcodes the Matter spec disallows; the SDK generator asserts against the
# same list, so this only keeps us from wasting a draw.
INVALID_PASSCODES = {
    0, 11111111, 22222222, 33333333, 44444444, 55555555,
    66666666, 77777777, 88888888, 99999999, 12345678, 87654321,
}
PASSCODE_MAX = 99999998

# Same idea for the BLE passkey: a repdigit or an obvious sequence defeats the
# point of pairing even though the value is technically legal.
WEAK_BLE_PASSKEYS = {d * 111111 for d in range(10)} | {123456, 654321}
BLE_PASSKEY_MAX = 999999

log = logging.getLogger("factory-data")


def read_partition(name="factory_data", path=PM_STATIC):
    """Return (address, size) for a partition in the Partition Manager layout.

    The layout is the single source of truth for where factory data lives;
    hardcoding the offset here is how it silently drifts from pm_static.yml.
    """
    if not os.path.isfile(path):
        sys.exit(f"Error: partition layout not found: {path}")

    with open(path) as f:
        text = f.read()

    part = None
    try:
        import yaml

        part = (yaml.safe_load(text) or {}).get(name)
    except ImportError:
        # The layout is a flat two-level mapping, so a small parser is enough
        # to avoid making PyYAML a hard requirement.
        log.debug("PyYAML not available, parsing %s directly", os.path.basename(path))
        block = re.search(
            rf"^{re.escape(name)}:\n((?:[ \t]+.*\n|\n)*)", text, re.MULTILINE
        )
        if block:
            part = {
                m.group(1): m.group(2).strip()
                for m in re.finditer(
                    r"^[ \t]+([A-Za-z_]+):[ \t]*(.+)$", block.group(1), re.MULTILINE
                )
            }

    if not part or "address" not in part or "size" not in part:
        sys.exit(f"Error: no '{name}' partition with address and size in {path}")

    # PyYAML resolves 0x-prefixed scalars to int; the fallback parser yields
    # strings. Accept both.
    def as_int(v):
        return v if isinstance(v, int) else int(str(v), 0)

    return as_int(part["address"]), as_int(part["size"])


def random_serial():
    return "LK-" + secrets.token_hex(4).upper()


def ble_advertised_name(sn):
    """The name the firmware will advertise for this serial number.

    Mirrors build_adv_data() in src/ble/ble_radio.c: the last four characters of
    the serial, or nothing at all if the serial is too short to supply them.
    """
    return f"{BLE_NAME_BASE}-{sn[-4:]}" if len(sn) >= 4 else BLE_NAME_BASE


def random_ble_passkey():
    while True:
        value = secrets.randbelow(BLE_PASSKEY_MAX + 1)
        if value not in WEAK_BLE_PASSKEYS:
            return value


def random_passcode():
    while True:
        value = secrets.randbelow(PASSCODE_MAX) + 1
        if value not in INVALID_PASSCODES:
            return value


def random_discriminator():
    # 12-bit field in the setup code.
    return secrets.randbelow(4096)


def random_spake2_salt():
    return base64.b64encode(secrets.token_bytes(SPAKE2_SALT_BYTES)).decode()


def build_matter_args(args, offset, size, user):
    argv = [
        sys.executable, SDK_GENERATOR,
        "--sn", args.sn,
        "--vendor_id", str(args.vendor_id),
        "--product_id", str(args.product_id),
        "--vendor_name", args.vendor_name,
        "--product_name", args.product_name,
        "--date", args.date,
        "--hw_ver", str(args.hw_ver),
        "--hw_ver_str", args.hw_ver_str,
        "--passcode", str(args.passcode),
        "--discriminator", str(args.discriminator),
        "--spake2_it", str(args.spake2_it),
        "--spake2_salt", args.spake2_salt,
        "--dac_cert", args.dac_cert,
        "--dac_key", args.dac_key,
        "--pai_cert", args.pai_cert,
        "--user", json.dumps(user),
        "--schema", args.schema,
        "--offset", hex(offset),
        "--size", hex(size),
        # With a random passcode and discriminator there is no way to
        # commission the device without the generated QR / manual code.
        "--generate_onboarding",
        "-o", args.output,
    ]
    if args.include_passcode:
        argv.append("--include_passcode")
    if args.overwrite:
        argv.append("--overwrite")
    if args.verbose:
        argv.append("-v")
    return argv


def generate_ble_only(args, offset, size, user):
    """Emit a blob with just the fields a non-Matter build reads.

    Uses the SDK's PartitionCreator directly rather than the Matter generator,
    which would demand certificates and SPAKE2+ material this build has no use
    for.
    """
    sys.path.insert(0, MATTER_TOOLS_DIR)
    try:
        from nrfconnect_generate_partition import PartitionCreator
    except ImportError:
        sys.exit(
            "Error: cannot import PartitionCreator.\n"
            f"Set NCS_ROOT to your nRF Connect SDK install (tried {MATTER_TOOLS_DIR})."
        )

    data = {
        "sn": args.sn,
        "vendor_name": args.vendor_name,
        "product_name": args.product_name,
        "hw_ver_str": args.hw_ver_str,
        "user": user,
    }

    json_path = args.output + ".json"
    if os.path.exists(json_path) and not args.overwrite:
        sys.exit(f"Error: {json_path} exists; pass --overwrite to replace it.")
    with open(json_path, "w") as f:
        json.dump(data, f, indent=4)

    creator = PartitionCreator(offset, size, json_path, args.output)
    cbor = creator.generate_cbor()
    creator.create_hex(cbor)
    return len(cbor)


def main():
    parser = argparse.ArgumentParser(
        description=__doc__,
        formatter_class=argparse.RawDescriptionHelpFormatter,
    )

    parser.add_argument(
        "-o", "--output", default="factory_data",
        help="output path prefix (default: factory_data)",
    )
    parser.add_argument(
        "--overwrite", action="store_true",
        help="replace existing output files",
    )
    parser.add_argument(
        "--no-matter", action="store_true", dest="no_matter",
        help="emit only the fields a CONFIG_LOCK_RADIO_MATTER=n build reads, "
             "omitting the attestation certificates and SPAKE2+ material",
    )
    parser.add_argument("-v", "--verbose", action="store_true")

    identity = parser.add_argument_group("Device identity")
    identity.add_argument(
        "--sn", default=None,
        help="serial number, max 20 bytes (default: random LK-XXXXXXXX)",
    )
    identity.add_argument("--vendor_name", default=DEFAULT_VENDOR_NAME)
    identity.add_argument("--product_name", default=DEFAULT_PRODUCT_NAME)
    identity.add_argument(
        "--vendor_id", type=lambda x: int(x, 0), default=DEFAULT_VENDOR_ID)
    identity.add_argument(
        "--product_id", type=lambda x: int(x, 0), default=DEFAULT_PRODUCT_ID)
    identity.add_argument("--hw_ver", type=lambda x: int(x, 0), default=DEFAULT_HW_VER)
    identity.add_argument("--hw_ver_str", default=DEFAULT_HW_VER_STR)
    identity.add_argument(
        "--date", default=None, help="ISO 8601 YYYY-MM-DD (default: today)")

    secrets_group = parser.add_argument_group(
        "Per-device secrets",
        "Randomly drawn when omitted. Recorded in <output>.json, which is the "
        "only copy -- the device stores the passcode only as a one-way verifier.",
    )
    secrets_group.add_argument(
        "--ble_passkey", type=int, default=None,
        help="6-digit BLE pairing passkey (default: random)",
    )
    secrets_group.add_argument(
        "--passcode", type=lambda x: int(x, 0), default=None,
        help="Matter setup passcode, 1..99999998 (default: random)",
    )
    secrets_group.add_argument(
        "--discriminator", type=lambda x: int(x, 0), default=None,
        help="12-bit commissioning discriminator (default: random)",
    )
    secrets_group.add_argument(
        "--spake2_salt", default=None,
        help="base64 SPAKE2+ salt, 16..32 bytes (default: random 32)",
    )
    secrets_group.add_argument(
        "--spake2_it", type=lambda x: int(x, 0), default=DEFAULT_SPAKE2_IT)
    secrets_group.add_argument(
        "--include-passcode", action="store_true", dest="include_passcode",
        help="also store the plaintext Matter passcode in factory data. Off by "
             "default: the device only needs the SPAKE2+ verifier to commission, "
             "and not storing the passcode means reading out the flash does not "
             "reveal it. The cost of leaving it off is that the firmware cannot "
             "reconstruct its own onboarding payload, so the codes it prints at "
             "boot are derived from Matter's example passcode and are WRONG — use "
             "the codes below and in <output>.txt instead. Turn this on if you "
             "would rather trust the boot log during development.",
    )

    sticker = parser.add_argument_group(
        "Sticker",
        "The label that goes on the unit, as a 1-bit PNG. The layout is "
        "proportional, so --sticker-size is free to be whatever suits the "
        "printer; feed its app a generous resolution and let it scale down.",
    )
    sticker.add_argument(
        "--sticker-size", dest="sticker_size", metavar="WxH",
        type=generate_sticker.parse_size, default=generate_sticker.DEFAULT_SIZE,
        help="output resolution in pixels; the layout adapts to any size and "
             "aspect ratio (default: %dx%d)" % generate_sticker.DEFAULT_SIZE,
    )
    sticker.add_argument(
        "--sticker-no-frame", dest="sticker_frame", action="store_false",
        help="drop the frame around the Matter onboarding card",
    )
    sticker.add_argument(
        "--no-sticker", dest="sticker", action="store_false",
        help="skip rendering the label",
    )

    other = parser.add_argument_group("Other")
    other.add_argument(
        "--battery_capacity_mah", type=int, default=DEFAULT_BATTERY_CAPACITY_MAH,
        help=f"pack design capacity (default: {DEFAULT_BATTERY_CAPACITY_MAH})",
    )
    other.add_argument("--dac_cert", default=None)
    other.add_argument("--dac_key", default=None)
    other.add_argument("--pai_cert", default=None)
    other.add_argument("--schema", default=SCHEMA)
    other.add_argument(
        "--offset", type=lambda x: int(x, 0), default=None,
        help="override the partition offset from pm_static.yml",
    )
    other.add_argument(
        "--size", type=lambda x: int(x, 0), default=None,
        help="override the partition size from pm_static.yml",
    )

    args = parser.parse_args()
    logging.basicConfig(
        level=logging.DEBUG if args.verbose else logging.INFO,
        format="%(levelname)s: %(message)s",
    )

    randomised = []
    if args.sn is None:
        args.sn = random_serial()
        randomised.append("sn")
    if args.ble_passkey is None:
        args.ble_passkey = random_ble_passkey()
        randomised.append("ble_passkey")
    if args.date is None:
        args.date = datetime.date.today().isoformat()

    # Matter-only material. Skipped entirely under --no-matter so the summary
    # does not claim to have generated values that never reach the blob.
    if not args.no_matter:
        if args.passcode is None:
            args.passcode = random_passcode()
            randomised.append("passcode")
        if args.discriminator is None:
            args.discriminator = random_discriminator()
            randomised.append("discriminator")
        if args.spake2_salt is None:
            args.spake2_salt = random_spake2_salt()
            randomised.append("spake2_salt")

    if not 0 <= args.ble_passkey <= BLE_PASSKEY_MAX:
        sys.exit("Error: --ble_passkey must be 000000..999999")
    if len(args.sn.encode()) > 20:
        sys.exit("Error: --sn must be at most 20 bytes (Matter certificate limit)")

    # Attestation certificates default to the SDK test set, which is issued for
    # the test VID/PID above. Using them with a different pair produces a device
    # that fails attestation, so say so rather than letting it surprise someone
    # during commissioning.
    default_certs = (args.dac_cert, args.dac_key, args.pai_cert) == (None, None, None)
    if default_certs:
        tag = f"{args.vendor_id:04X}-{args.product_id:04X}"
        args.dac_cert = os.path.join(TEST_CERTS_DIR, f"Chip-Test-DAC-{tag}-0001-Cert.der")
        args.dac_key = os.path.join(TEST_CERTS_DIR, f"Chip-Test-DAC-{tag}-0001-Key.der")
        args.pai_cert = os.path.join(TEST_CERTS_DIR, f"Chip-Test-PAI-{tag}-Cert.der")
        if (args.vendor_id, args.product_id) != (DEFAULT_VENDOR_ID, DEFAULT_PRODUCT_ID):
            log.warning(
                "Using SDK test certificates for VID/PID %s. Production devices "
                "need certificates from a real PAA.", tag
            )

    user = {
        "ble_passkey": f"{args.ble_passkey:06d}",
        "battery_capacity_mah": str(args.battery_capacity_mah),
    }

    offset, size = read_partition()
    if args.offset is not None:
        offset = args.offset
    if args.size is not None:
        size = args.size
    log.info("factory_data partition: offset %#x, size %#x", offset, size)

    if args.no_matter:
        # No schema validation on this path: factory_data.schema describes the
        # full set and requires the Matter keys this blob deliberately omits.
        cbor_len = generate_ble_only(args, offset, size, user)
        log.info("CBOR size: %d of %d bytes", cbor_len, size)
        if cbor_len > size:
            sys.exit(f"Error: factory data is {cbor_len} bytes, partition is {size}")
    else:
        for path, what in ((args.dac_cert, "DAC certificate"),
                           (args.dac_key, "DAC key"),
                           (args.pai_cert, "PAI certificate")):
            if not os.path.isfile(path):
                sys.exit(f"Error: {what} not found: {path}")
        if not os.path.isfile(SDK_GENERATOR):
            sys.exit(
                f"Error: SDK generator not found: {SDK_GENERATOR}\n"
                "Set NCS_ROOT to your nRF Connect SDK installation."
            )

        argv = build_matter_args(args, offset, size, user)
        log.debug("running: %s", " ".join(argv))
        result = subprocess.run(argv)
        if result.returncode != 0:
            sys.exit(result.returncode)

    sticker_path = None
    if args.sticker:
        manual = qr = None
        if not args.no_matter:
            manual, qr = generate_sticker.read_onboarding(args.output + ".txt")
        try:
            sticker_path = generate_sticker.render_sticker(
                args.output + ".sticker.png",
                args.sn,
                ble_advertised_name(args.sn),
                f"{args.ble_passkey:06d}",
                manual_code=manual,
                qr_payload=qr,
                size=args.sticker_size,
                frame=args.sticker_frame,
            )
        except ImportError as e:
            # A missing imaging library should not invalidate a partition that
            # is already written; the label can be redrawn from the outputs.
            log.warning("sticker not rendered (%s). Install pillow and qrcode, "
                        "then run scripts/generate_sticker.py %s", e, args.output)

    print()
    print(f"  Serial number:  {args.sn}")
    print(f"  Vendor/product: {args.vendor_name} / {args.product_name}")
    print(f"  Hardware:       {args.hw_ver_str} (rev {args.hw_ver})")
    print(f"  BLE name:       {ble_advertised_name(args.sn)}")
    print(f"  BLE passkey:    {args.ble_passkey:06d}")
    if not args.no_matter:
        print(f"  Matter VID/PID: {args.vendor_id:#06x} / {args.product_id:#06x}")
        print(f"  Matter passcode:{args.passcode:9d}")
        print(f"  Discriminator:  {args.discriminator}")
    print(f"  Battery:        {args.battery_capacity_mah} mAh")
    print(f"  Partition:      {offset:#x}, {size:#x} bytes")
    # The onboarding payload is the only durable record of the passcode: it is
    # deliberately not written into factory data (the device keeps a one-way
    # SPAKE2+ verifier instead), so it does not appear in <output>.json either.
    onboarding = args.output + ".txt"
    if not args.no_matter and os.path.isfile(onboarding):
        with open(onboarding) as f:
            for line in f:
                if line.strip():
                    print(f"  {line.strip()}")
        print()

    if randomised:
        print(f"Randomly generated: {', '.join(randomised)}")
        if args.no_matter:
            print(f"KEEP {args.output}.json — it is the only record of these values.")
        elif args.include_passcode:
            print(f"KEEP {args.output}.json and {args.output}.txt — the passcode is")
            print("recorded in both, and also stored on the device.")
        else:
            print(f"KEEP {args.output}.json and {args.output}.txt — together they are")
            print("the only record of these values. The passcode in particular is NOT")
            print("in the .json and NOT on the device (which stores only a one-way")
            print("SPAKE2+ verifier); it survives solely in the onboarding codes above.")
        print()

    if not args.no_matter and not args.include_passcode:
        print("NOTE: the passcode is not stored on the device, so the firmware cannot")
        print("      rebuild its own onboarding payload. The codes it prints at boot")
        print("      ('Manual pairing code', 'SetupQRCode') are derived from Matter's")
        print("      EXAMPLE passcode 20202021 and WILL NOT WORK. Commission with the")
        print("      codes above. Pass --include-passcode if you want the boot log to")
        print("      be authoritative instead.")
        print()
    if sticker_path:
        w, h = args.sticker_size
        print(f"Label ({w}x{h} px): {sticker_path}")
        print()

    print("Flash with:")
    print(f"  ./scripts/pad_hex.py {args.output}.hex")
    print(f"  nrfutil device program --firmware {args.output}.hex --core application \\")
    print("    --options chip_erase_mode=ERASE_RANGES_TOUCHED_BY_FIRMWARE,reset=RESET_SYSTEM")


if __name__ == "__main__":
    main()
