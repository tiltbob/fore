#!/usr/bin/env python3
"""Generate preload.h: a P-384 private key and its certificate, which become
the firmware's factory defaults (make PRELOAD_KEY=... [PRELOAD_CERT=...]),
and optionally a factory PIN (PRELOAD_PIN=...).

The key is a hex scalar or a P-384 private key file (PEM or DER, SEC1 or
PKCS#8).

The certificate is either one you supply (--cert, PEM or DER, any issuer, but
its public key must match the private key) or a generated self-signed one.

The key, certificate and PIN are installed on the first boot of a blank
(mass-erased) chip and again after a PIV reset. The key ends up in the firmware
image, so anyone with the .bin has it: this is for novelty builds only.

Also writes preload-key.pem and preload-cert.pem next to the header.
Needs the openssl command-line tool.
"""
import argparse
import base64
import os
import subprocess
import sys
import tempfile

SECP384R1_OID = bytes.fromhex("06052b81040022")

# P-384 group order
N = int("FFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFC7634D81F4372DDF581A0DB248B0A77AECEC196ACCC52973", 16)

# slot -> (index into piv_state.keys, certificate object tag)
SLOTS = {
    "9a": (0, 0x5FC105),
    "9c": (1, 0x5FC10A),
    "9d": (2, 0x5FC10B),
    "9e": (3, 0x5FC101),
}

OBJECT_CAPACITY = 2 * 1024 - 4  # two flash pages minus the object header

OPENSSL_CONFIG = """\
[req]
distinguished_name = dn
x509_extensions = ext
[dn]
[ext]
basicConstraints = critical, CA:FALSE
keyUsage = critical, digitalSignature, keyAgreement
subjectKeyIdentifier = hash
"""


def tlv(tag, value):
    n = len(value)
    if n < 0x80:
        length = bytes([n])
    elif n < 0x100:
        length = bytes([0x81, n])
    else:
        length = bytes([0x82, n >> 8, n & 0xFF])
    return tag + length + value


def c_array(name, data):
    lines = []
    for i in range(0, len(data), 16):
        lines.append("    " + " ".join("0x%02x," % b for b in data[i:i + 16]))
    return "static const uint8_t %s[%d] = {\n%s\n};\n" % (name, len(data), "\n".join(lines))


def openssl(*args):
    env = dict(os.environ)
    env.pop("OPENSSL_CONF", None)  # a pkcs11 provider config has no business here
    r = subprocess.run(["openssl", *args], env=env, capture_output=True)
    if r.returncode:
        sys.stderr.write(r.stderr.decode(errors="replace"))
        raise subprocess.CalledProcessError(r.returncode, r.args)
    return r.stdout


def is_pem(path):
    with open(path, "rb") as f:
        return f.read().lstrip().startswith(b"-----BEGIN")


def der_items(buf):
    """Splits DER content into (tag, value) pairs."""
    items, pos = [], 0
    while pos < len(buf):
        tag, n = buf[pos], buf[pos + 1]
        pos += 2
        if n & 0x80:
            k = n & 0x7F
            n = int.from_bytes(buf[pos:pos + k], "big")
            pos += k
        items.append((tag, buf[pos:pos + n]))
        pos += n
    return items


def load_key(spec):
    """--key: a hex scalar, or a P-384 private key file (PEM or DER, SEC1 or PKCS#8)."""
    if not os.path.isfile(spec):
        hexkey = spec.lower().removeprefix("0x")
        try:
            d = int(hexkey, 16)
        except ValueError:
            sys.exit("--key must be a hex scalar or a private key file: %s" % spec)
        if len(hexkey) > 96:
            sys.exit("--key: more than 384 bits")
        return d
    try:
        sec1 = openssl("ec", "-inform", "PEM" if is_pem(spec) else "DER", "-in", spec,
                       "-no_public", "-outform", "DER")
    except subprocess.CalledProcessError:
        sys.exit("%s: not an EC private key" % spec)
    # ECPrivateKey ::= SEQUENCE { version, privateKey OCTET STRING, [0] parameters, ... }
    try:
        fields = dict(der_items(der_items(sec1)[0][1]))
        if fields.get(0xA0) == SECP384R1_OID:
            return int.from_bytes(fields[0x04], "big")
    except (IndexError, KeyError):
        pass
    sys.exit("%s: not a P-384 (secp384r1) key" % spec)


def self_signed(key_path, cn):
    with tempfile.TemporaryDirectory() as tmp:
        cfg = os.path.join(tmp, "req.cnf")
        with open(cfg, "w") as f:
            f.write(OPENSSL_CONFIG)
        return openssl("req", "-new", "-x509", "-config", cfg, "-key", key_path, "-sha384",
                       "-days", "36500", "-subj", "/CN=" + cn, "-outform", "DER")


def load_cert(path):
    """A PEM or DER certificate file, returned as DER (openssl parses it)."""
    try:
        return openssl("x509", "-inform", "PEM" if is_pem(path) else "DER", "-in", path, "-outform", "DER")
    except subprocess.CalledProcessError:
        sys.exit("%s: not an X.509 certificate" % path)


def cert_pubkey(der):
    with tempfile.NamedTemporaryFile(suffix=".der") as f:
        f.write(der)
        f.flush()
        return openssl("x509", "-inform", "DER", "-in", f.name, "-pubkey", "-noout")


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--key", required=True,
                    help="private scalar in hex (e.g. 4) or a P-384 private key file (PEM or DER)")
    ap.add_argument("--slot", default="9a", choices=sorted(SLOTS))
    ap.add_argument("--cert", help="certificate to ship (PEM or DER) instead of a generated self-signed one")
    ap.add_argument("--cn", help="subject CN of the generated certificate")
    ap.add_argument("--pin", help="factory PIN, 6 to 8 digits (default 123456)")
    ap.add_argument("--out-dir", required=True)
    args = ap.parse_args()

    if args.pin is not None and not (6 <= len(args.pin) <= 8 and args.pin.isascii() and args.pin.isdigit()):
        sys.exit("--pin must be 6 to 8 digits")
    d = load_key(args.key)
    if not 1 <= d < N:
        sys.exit("--key must be a P-384 private scalar: 1 <= d < n")
    scalar = d.to_bytes(48, "big")
    index, tag = SLOTS[args.slot]

    # SEC1 ECPrivateKey without the public key; OpenSSL derives it.
    sec1 = tlv(b"\x30", b"\x02\x01\x01" + tlv(b"\x04", scalar) + bytes.fromhex("a00706052b81040022"))
    b64 = base64.b64encode(sec1).decode()
    key_pem = "-----BEGIN EC PRIVATE KEY-----\n%s\n-----END EC PRIVATE KEY-----\n" % "\n".join(
        b64[i:i + 64] for i in range(0, len(b64), 64))

    os.makedirs(args.out_dir, exist_ok=True)
    key_path = os.path.join(args.out_dir, "preload-key.pem")
    with open(key_path, "w") as f:
        f.write(key_pem)

    if args.cert:
        cert = load_cert(args.cert)
        what = args.cert
    else:
        cn = args.cn or "fore preloaded key"
        cert = self_signed(key_path, cn)
        what = "self-signed, CN=" + cn
    # OpenSC takes the key's public half from the certificate, so they must agree.
    if cert_pubkey(cert) != openssl("pkey", "-in", key_path, "-pubout"):
        sys.exit("%s: certificate public key does not match the preloaded private key" % what)
    with tempfile.NamedTemporaryFile(suffix=".der") as f:
        f.write(cert)
        f.flush()
        pem = openssl("x509", "-inform", "DER", "-in", f.name)
    with open(os.path.join(args.out_dir, "preload-cert.pem"), "wb") as f:
        f.write(pem)

    # PIV certificate container, as yubico-piv-tool writes it: cert, uncompressed, no LRC.
    container = tlv(b"\x53", tlv(b"\x70", cert) + b"\x71\x01\x00\xfe\x00")
    if len(container) > OBJECT_CAPACITY:
        sys.exit("certificate too large: %d bytes, at most %d fit" % (len(container), OBJECT_CAPACITY))

    with open(os.path.join(args.out_dir, "preload.h"), "w") as f:
        f.write("/* Generated by tools/gen-preload.py: slot %s, %s. Do not edit. */\n" % (args.slot, what))
        f.write("#define PRELOAD_KEY_INDEX %d\n" % index)
        f.write("#define PRELOAD_CERT_TAG 0x%06xu\n" % tag)
        f.write(c_array("preload_key", scalar))
        f.write(c_array("preload_cert", container))
        if args.pin:
            # The PIN as VERIFY sends it: padded to 8 bytes with 0xFF.
            padded = ["'%s'" % c for c in args.pin] + ["0xff"] * (8 - len(args.pin))
            f.write("#define PRELOAD_PIN {%s}\n" % ", ".join(padded))
    print("preload: slot %s, %s (%d byte certificate container)%s"
          % (args.slot, what, len(container), ", PIN " + args.pin if args.pin else ""))


if __name__ == "__main__":
    main()
