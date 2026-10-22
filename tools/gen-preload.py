#!/usr/bin/env python3
"""Generate preload.h: a P-384 private key and its certificate, which become
the firmware's factory defaults (make PRELOAD_KEY=... [PRELOAD_CERT=...]).

The certificate is either one you supply (--cert, PEM or DER, any issuer, but
its public key must match the private key) or a generated self-signed one.

The key and certificate are installed on the first boot of a blank (mass-erased)
chip and again after a PIV reset. The key ends up in the firmware image, so
anyone with the .bin has it: this is for novelty builds only.

Also writes preload-key.pem and preload-cert.pem next to the header.
Needs the openssl command-line tool.
"""
import argparse
import base64
import os
import subprocess
import sys
import tempfile

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
    return subprocess.run(["openssl", *args], check=True, env=env, stdout=subprocess.PIPE).stdout


def self_signed(key_path, cn):
    with tempfile.TemporaryDirectory() as tmp:
        cfg = os.path.join(tmp, "req.cnf")
        with open(cfg, "w") as f:
            f.write(OPENSSL_CONFIG)
        return openssl("req", "-new", "-x509", "-config", cfg, "-key", key_path, "-sha384",
                       "-days", "36500", "-subj", "/CN=" + cn, "-outform", "DER")


def load_cert(path):
    """A PEM or DER certificate file, returned as DER (openssl parses it)."""
    with open(path, "rb") as f:
        pem = f.read().lstrip().startswith(b"-----BEGIN")
    try:
        return openssl("x509", "-inform", "PEM" if pem else "DER", "-in", path, "-outform", "DER")
    except subprocess.CalledProcessError:
        sys.exit("%s: not an X.509 certificate" % path)


def cert_pubkey(der):
    with tempfile.NamedTemporaryFile(suffix=".der") as f:
        f.write(der)
        f.flush()
        return openssl("x509", "-inform", "DER", "-in", f.name, "-pubkey", "-noout")


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--key", required=True, help="private scalar in hex, e.g. 4 or 0x00..04")
    ap.add_argument("--slot", default="9a", choices=sorted(SLOTS))
    ap.add_argument("--cert", help="certificate to ship (PEM or DER) instead of a generated self-signed one")
    ap.add_argument("--cn", help="subject CN of the generated certificate")
    ap.add_argument("--out-dir", required=True)
    args = ap.parse_args()

    hexkey = args.key.lower().removeprefix("0x")
    try:
        d = int(hexkey, 16)
    except ValueError:
        sys.exit("--key must be hexadecimal")
    if len(hexkey) > 96 or not 1 <= d < N:
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
        cn = args.cn or "fore insecure key (d = 0x%x)" % d
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
    print("preload: slot %s, %s (%d byte certificate container)" % (args.slot, what, len(container)))


if __name__ == "__main__":
    main()
