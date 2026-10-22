# fore

A minimalist PIV smart card for the ST-Link V2 USB dongle. Load your own
NIST P-384 key and certificate with `yubico-piv-tool`, then use the key from
OpenSSL through OpenSC's PKCS#11 module: signing (`openssl dgst`, `req`, `cms`,
TLS client auth) and ECDH key agreement.

This is a novelty device, not a secure element. See [Limitations](#limitations).

## What it does

The STM32F103 on the dongle enumerates as a USB CCID smart card reader with a
card that is always inserted. The card runs a small PIV applet (NIST SP
800-73-4 subset) plus the Yubico extensions that `yubico-piv-tool` uses to
load keys:

| Command | INS | Notes |
| --- | --- | --- |
| SELECT | `A4` | PIV AID, by full AID or prefix |
| GET DATA / PUT DATA | `CB` / `DB` | certificates for 9A/9C/9D/9E, CHUID, CCC; discovery object |
| VERIFY | `20` | PIN, 3 tries |
| CHANGE REFERENCE DATA | `24` | PIN and PUK |
| RESET RETRY COUNTER | `2C` | unblock the PIN with the PUK |
| GENERAL AUTHENTICATE | `87` | ECDSA and ECDH with P-384 (alg `14`); 3DES management key (9B) |
| GET RESPONSE, command chaining | `C0`, CLA `10` | for certificates over 255 bytes |
| IMPORT KEY | `FE` | Yubico extension, P-384 only |
| SET MANAGEMENT KEY | `FF` | Yubico extension, 3DES only |
| GET VERSION / GET SERIAL | `FD` / `F8` | Yubico extensions |
| RESET | `FB` | Yubico extension: wipes everything once PIN and PUK are both blocked |

Key slots and default PIN policies (override per key with
`yubico-piv-tool --pin-policy=never|once|always` at import time):

| Slot | PIV name | PIN | OpenSC key label |
| --- | --- | --- | --- |
| 9A | PIV Authentication | once per session | `PIV AUTH key` |
| 9C | Digital Signature | before every signature | `SIGN key` |
| 9D | Key Management | once per session | `KEY MAN key` |
| 9E | Card Authentication | never | `CARD AUTH key` |

Defaults, same as a YubiKey: PIN `123456`, PUK `12345678`, management key
`010203040506070801020304050607080102030405060708` (3DES). Change all three.

## Hardware

Any ST-Link V2 dongle (the cheap aluminium-stick clones or the original) built
around an STM32F103C8/CB with an 8 MHz crystal. The firmware uses the USB port,
the crystal, and the LED on PA9 (lit while the card is busy). It needs 50 KiB of
flash for code and the top 14 KiB (0x0800C800-0x0800FFFF) for keys and
certificates, so it fits the 64 KiB parts.

Clones built on CKS32, CS32, APM32 or GD32 chips are register-compatible and
should work, but are untested.

## Build

Needs `arm-none-eabi-gcc` with newlib (`gcc-arm-none-eabi` and
`libnewlib-arm-none-eabi` on Debian/Ubuntu).

```sh
make                                  # build/fore.elf, fore.bin, fore.hex
make USB_VID=0x1234 USB_PID=0x5678    # use your own USB IDs
```

The default USB ID is `1209:0001`, a [pid.codes](https://pid.codes) test ID
that is fine for a private device but must not be used for anything you
distribute.

### Novelty build: a key everyone knows

```sh
make novelty          # build/novelty/fore.{elf,bin,hex}
```

This build ships with the private key
`0x000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000004`
(d = 4) in slot 9A. It also ships a self-signed certificate,
"CN=fore insecure key (d = 0x4)", so OpenSC and OpenSSL see the key straight
away. The device works like any other PIV token, PIN and all. Anyone can make
the same signatures with a key file containing the number 4, and
`build/novelty/preload-key.pem` is that file.

The key and certificate are the factory defaults. They are written on the first
boot of a blank (mass-erased) chip and come back after `yubico-piv-tool -a
reset`. Importing your own key or certificate replaces them as usual.

The general form is `make PRELOAD_KEY=<hex scalar> PRELOAD_SLOT=9a|9c|9d|9e
BUILD=<dir>`, using a separate build directory per key. A preloaded key is
compiled into the firmware image, so keep this for keys that are meant to be
public. `tools/gen-preload.py` makes the certificate, which needs the `openssl`
command-line tool.

## Flash the dongle

You need a second SWD programmer (another ST-Link, a Raspberry Pi with
OpenOCD, a Black Magic Probe...). The dongle's own STM32 has its SWD pins
(PA13 = SWDIO, PA14 = SWCLK) on pads or a header on the PCB, usually labelled
SWDIO, SWCLK, GND and 3.3V; open the case and connect those four.

Most dongles ship with read-out protection set, so unlock (which mass-erases)
first, power-cycle, then program:

```sh
openocd -f interface/stlink.cfg -f target/stm32f1x.cfg \
    -c "init; reset halt; stm32f1x unlock 0; reset halt; exit"
# unplug and replug the dongle being flashed
openocd -f interface/stlink.cfg -f target/stm32f1x.cfg \
    -c "program build/fore.elf verify reset exit"
```

`make flash` does the second step with `st-flash` instead. CKS32-based clones
report a different core ID; add `-c "set CPUTAPID 0x2ba01477"` before `-f
target/stm32f1x.cfg`.

Reflashing the firmware later leaves keys and certificates alone; a mass erase
or `unlock` wipes them.

## Host setup

Install pcscd and OpenSC (`pcscd libccid opensc` on Debian/Ubuntu), plus
`yubico-piv-tool` for loading keys and either `pkcs11-provider` or
`libengine-pkcs11-openssl` for OpenSSL.

On Linux, libccid only drives readers listed in its `Info.plist`, so register
the device once (and again after libccid package upgrades):

```sh
sudo tools/ccid-add-id.py          # or --vid/--pid if you built with your own IDs
sudo systemctl restart pcscd
opensc-tool -l                     # shows "fore PIV (<serial>) 00 00"
```

macOS and Windows ship CCID class drivers and should need nothing (untested).

## Load a key

`yubico-piv-tool` looks for a reader named "Yubikey" unless told otherwise, so
pass `-r fore`.

```sh
R="-r fore"

# Replace the defaults first. Keep the management key somewhere safe.
MGM=$(openssl rand -hex 24)
yubico-piv-tool $R -a change-pin -P 123456 -N <new PIN, 6-8 chars>
yubico-piv-tool $R -a change-puk -P 12345678 -N <new PUK, 6-8 chars>
yubico-piv-tool $R -a set-mgm-key -n $MGM

# Your P-384 key, and a certificate for it (OpenSC needs a certificate in the
# slot to expose the key; a self-signed one is fine).
openssl ecparam -name secp384r1 -genkey -noout -out key.pem
openssl req -new -x509 -key key.pem -subj "/CN=me" -days 3650 -out cert.pem

yubico-piv-tool $R --key=$MGM -a import-key -s 9a -i key.pem
yubico-piv-tool $R --key=$MGM -a import-certificate -s 9a -i cert.pem
yubico-piv-tool $R -a status
yubico-piv-tool $R -a verify-pin -a test-signature -s 9a -i cert.pem
```

To get a CA-issued certificate instead, import the key with a self-signed
certificate as above, make a CSR with OpenSSL (below), and import the issued
certificate over the self-signed one.

## Use it from OpenSSL

### pkcs11-provider (OpenSSL 3)

```ini
# fore.cnf
openssl_conf = openssl_init
[openssl_init]
providers = provider_sect
[provider_sect]
default = default_sect
pkcs11 = pkcs11_sect
[default_sect]
activate = 1
[pkcs11_sect]
module = /usr/lib/x86_64-linux-gnu/ossl-modules/pkcs11.so
pkcs11-module-path = /usr/lib/x86_64-linux-gnu/opensc-pkcs11.so
pkcs11-module-token-pin = file:/path/to/pin.txt
activate = 1
```

```sh
export OPENSSL_CONF=$PWD/fore.cnf
KEY="pkcs11:object=PIV AUTH key;type=private"

openssl dgst -sha384 -sign "$KEY" -out msg.sig msg.txt
openssl req -new -key "$KEY" -subj "/CN=me" -out me.csr
openssl pkey -pubin -in "pkcs11:object=PIV AUTH pubkey;type=public" -pubout

OPENSSL_CONF= openssl dgst -sha384 -verify <(openssl x509 -in cert.pem -pubkey -noout) \
    -signature msg.sig msg.txt
```

### libp11 engine (also does ECDH)

```ini
# fore-engine.cnf
openssl_conf = openssl_init
[openssl_init]
engines = engine_section
[engine_section]
pkcs11 = pkcs11_section
[pkcs11_section]
engine_id = pkcs11
dynamic_path = /usr/lib/x86_64-linux-gnu/engines-3/pkcs11.so
MODULE_PATH = /usr/lib/x86_64-linux-gnu/opensc-pkcs11.so
init = 0
```

```sh
export OPENSSL_CONF=$PWD/fore-engine.cnf
openssl dgst -sha384 -engine pkcs11 -keyform engine \
    -sign "pkcs11:object=PIV AUTH key;type=private" -out msg.sig msg.txt
openssl pkeyutl -derive -engine pkcs11 -keyform engine \
    -inkey "pkcs11:object=KEY MAN key;type=private" -peerkey peer-pub.pem -out shared.bin
```

ECDH through pkcs11-provider 0.3 (the version in Ubuntu 24.04) fails inside
the host stack before reaching the card (OpenSC rejects the provider's key
template); use the engine or `pkcs11-tool --derive` for ECDH there.

Slot 9C asks for the PIN before every signature, which PKCS#11 expresses as
`CKA_ALWAYS_AUTHENTICATE`; pkcs11-provider handles that with the configured
PIN. Use 9A if a tool struggles with it.

## Forgot the PIN?

Unblock it with the PUK (`yubico-piv-tool $R -a unblock-pin -P <PUK> -N <new
PIN>`). If the PUK is blocked too, `yubico-piv-tool $R -a reset` wipes the keys,
certificates and credentials back to the defaults.

## Limitations

- P-384 only. No RSA, P-256, Ed25519 or X25519.
- No on-card key generation: the STM32F103 has no hardware RNG, so keys come
  from the host. The random numbers the card does need (management key
  challenges) come from power-up SRAM contents, ADC noise and USB timing.
  ECDSA nonces are deterministic (RFC 6979) and need no randomness.
- 3DES management key only; no AES management keys, touch policies,
  attestation, key metadata or secure messaging. `ykman` will not work.
- A P-384 signature or ECDH takes roughly half a second to a second (an
  estimate from the code; not measured on hardware).
- Keys sit unencrypted in flash. Anyone with SWD access can read them unless
  read-out protection is on (`stm32f1x lock 0` in OpenOCD after provisioning,
  then power-cycle; unlocking again mass-erases). Even then, STM32F1 read-out
  protection has published bypasses, and the Cortex-M3 multiplier's timing
  depends on its operands. The arithmetic avoids secret-dependent branches and
  table lookups, but nothing here is hardened against physical attacks.
- The management key challenge is only as unpredictable as the boot-time
  entropy: power-up SRAM contents and ADC noise, neither measured on real
  chips. A guessable challenge would let someone authenticate as the management
  key without knowing it.
- The firmware has been tested against a model of the USB peripheral and the
  real host stack (below), not yet on a physical dongle.

## Tests

```sh
make test              # crypto vs OpenSSL, applet, USB driver (ASan/UBSan)
make -C test e2e       # pcscd + OpenSC + yubico-piv-tool + OpenSSL, twice
```

`make test` needs a host C compiler and OpenSSL headers. The unit suites cover:

- `test_crypto`: SHA-384/512, HMAC, 3DES, P-384 public keys, ECDSA and ECDH
  cross-checked against libcrypto, the RFC 6979 test vector, invalid scalars and
  points.
- `test_piv`: APDU parsing, chaining, GET RESPONSE, security conditions, PIN
  policies, PIN/PUK counters, management key authentication, and storage
  recovery from torn writes and corrupted retry counters.
- `test_usb`: `stm32/usb.c` and the CCID layer against a register-level model
  of the STM32 USB peripheral (`test/usb_mock.c`): enumeration, descriptors,
  multi-packet control and bulk transfers, ZLPs, data toggles, halts, time
  extensions and bus resets.
- `test_preload`: the novelty build's d = 4 key and certificate: present on a
  blank device, replaceable, and restored by a reset.

`make -C test e2e` additionally needs `pcscd libccid vsmartcard-vpcd opensc
yubico-piv-tool pkcs11-provider libengine-pkcs11-openssl libusb-1.0-0-dev`, and
starts its own pcscd (stop any running one). It runs the same scenario twice:
once with the applet behind the vpcd virtual reader, and once with libccid
talking to the firmware's own USB driver through a fake libusb
(`test/fake_libusb.c`) LD_PRELOADed into pcscd. A third, short run checks the
novelty build: the card signs as d = 4, and so does anyone else.

## Layout

```
src/        portable code: PIV applet, CCID, flash storage, P-384, SHA-384, 3DES
stm32/      STM32F103 start-up, clocks, USB driver, flash and RNG, linker script
host/       platform shims and the vpcd virtual card for running on a PC
test/       unit tests, USB peripheral model, fake libusb, end-to-end script
tools/      ccid-add-id.py: register the USB ID with libccid on Linux
            gen-preload.py: factory-default key and certificate (make novelty)
```
