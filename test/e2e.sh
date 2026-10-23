#!/usr/bin/env bash
# End-to-end test with the real host stack: pcscd, OpenSC, yubico-piv-tool and
# OpenSSL (pkcs11-provider and the libp11 engine).
#
#   ./e2e.sh vpcd   applet behind vsmartcard's virtual reader
#   ./e2e.sh usb    full firmware USB path: the stock libccid talks to stm32/usb.c
#                   through a fake libusb (test/fake_libusb.c) and the peripheral model
#   ./e2e.sh novelty  vpcd with the factory-preloaded d = 4 key and PIN 8675309 (make novelty)
#   ./e2e.sh locked   the same, locked (make novelty PRELOAD_LOCKED=1)
#
# Starts its own pcscd, so no other pcscd may be running.
set -euo pipefail

MODE=${1:-vpcd}
HERE=$(cd "$(dirname "$0")" && pwd)
ROOT=$(dirname "$HERE")
BUILD=$ROOT/build/test
WORK=$(mktemp -d)
MULTIARCH=$(gcc -print-multiarch 2>/dev/null || echo x86_64-linux-gnu)
P11=/usr/lib/$MULTIARCH/opensc-pkcs11.so
PIDS=()

cleanup() {
    for p in "${PIDS[@]}"; do kill "$p" 2>/dev/null || true; done
    wait 2>/dev/null || true
    if [ "${KEEP:-0}" = 1 ]; then echo "work dir kept: $WORK"; else rm -rf "$WORK"; fi
}
trap cleanup EXIT

step() { printf '\n== %s\n' "$*"; }
fail() { echo "FAIL: $*" >&2; exit 1; }

if pgrep -x pcscd >/dev/null; then
    fail "another pcscd is running; stop it first"
fi

mkdir -p "$WORK/readers" "$WORK/drivers"
start_pcscd() {
    pcscd -f -a --disable-polkit -c "$WORK/readers" >"$WORK/pcscd.log" 2>&1 &
    PIDS+=($!)
}

CARD=$BUILD/vpcd_card
PIN=123456
case $MODE in
novelty | locked) CARD=$BUILD/vpcd_card_$MODE PIN=8675309 ;; # make novelty's PIN
esac

case $MODE in
vpcd | novelty | locked)
    cat >"$WORK/readers/vpcd" <<EOF
FRIENDLYNAME "Virtual PCD"
DEVICENAME   /dev/null:0x8C7B
LIBPATH      /usr/lib/pcsc/drivers/serial/libifdvpcd.so
CHANNELID    0x8C7B
EOF
    start_pcscd
    sleep 1
    "$CARD" -s "$WORK/flash.bin" >"$WORK/card.log" 2>&1 &
    PIDS+=($!)
    READER="Virtual PCD 00 00"
    ;;
usb)
    # The stock libccid bundle, unmodified: the default USB ID is on its list.
    # The reader takes the name libccid gives that ID, as with hotplug.
    cat >"$WORK/readers/fore" <<EOF
FRIENDLYNAME "RSA RSA SecurID (R) Authenticator"
DEVICENAME   usb:15e1/2007
LIBPATH      /usr/lib/pcsc/drivers/ifd-ccid.bundle/Contents/Linux/libccid.so
EOF
    FORE_FLASH=$WORK/flash.bin LD_PRELOAD=$BUILD/libfakeusb.so start_pcscd
    READER="SecurID"
    ;;
*)
    fail "mode must be vpcd, usb, novelty or locked"
    ;;
esac

for _ in $(seq 50); do
    opensc-tool -l 2>/dev/null | grep -q "Yes" && break
    sleep 0.2
done
step "readers"
opensc-tool -l
opensc-tool -l | grep -q "Yes" || fail "no card present"
opensc-tool -r 0 -a -n | tee "$WORK/atr.txt"
grep -q "Personal Identity Verification Card" "$WORK/atr.txt" || fail "OpenSC did not recognise PIV"

PIV=(yubico-piv-tool -r "$READER")
cd "$WORK"
cat >provider.cnf <<EOF
openssl_conf = openssl_init
[openssl_init]
providers = provider_sect
[provider_sect]
default = default_sect
pkcs11 = pkcs11_sect
[default_sect]
activate = 1
[pkcs11_sect]
module = /usr/lib/$MULTIARCH/ossl-modules/pkcs11.so
pkcs11-module-path = $P11
pkcs11-module-token-pin = $PIN
activate = 1
EOF

if [ "$MODE" = novelty ] || [ "$MODE" = locked ]; then
    PRE=$BUILD/preload
    step "factory key d = 4 in slot 9A"
    "${PIV[@]}" -a status | tee status.txt
    grep -q "CN=fore insecure key (d = 0x4)" status.txt || fail "preloaded certificate missing"
    if "${PIV[@]}" -P 123456 -a verify-pin; then fail "PIN 123456 accepted"; fi
    "${PIV[@]}" -P "$PIN" -a verify-pin -a test-signature -s 9a -i "$PRE/preload-cert.pem"
    echo "signed by a very secure token" >msg.txt
    OPENSSL_CONF=$WORK/provider.cnf openssl dgst -sha384 -sign "pkcs11:object=PIV AUTH key;type=private" \
        -out card.sig msg.txt
    OPENSSL_CONF=$WORK/provider.cnf openssl pkey -pubin -in "pkcs11:object=PIV AUTH pubkey;type=public" \
        -pubout -out card-pub.pem
    openssl dgst -sha384 -verify card-pub.pem -signature card.sig msg.txt

    step "and anyone can forge it"
    openssl dgst -sha384 -sign "$PRE/preload-key.pem" -out forged.sig msg.txt
    openssl dgst -sha384 -verify card-pub.pem -signature forged.sig msg.txt

    if [ "$MODE" = locked ]; then
        step "locked: the key and certificate cannot be replaced or removed"
        openssl ecparam -name secp384r1 -genkey -noout -out other.pem
        openssl req -new -x509 -key other.pem -out other.crt -days 30 -subj "/CN=other"
        for s in 9a 9c; do
            if "${PIV[@]}" -a import-key -s $s -i other.pem; then fail "key import into $s accepted"; fi
        done
        if "${PIV[@]}" -a import-certificate -s 9a -i other.crt; then fail "certificate import accepted"; fi
        if "${PIV[@]}" -a delete-certificate -s 9a; then fail "certificate delete accepted"; fi
        for _ in 1 2 3; do "${PIV[@]}" -P 000000 -a verify-pin || true; done
        for _ in 1 2 3; do "${PIV[@]}" -P 00000000 -N 123456 -a unblock-pin || true; done
        "${PIV[@]}" -a reset
        "${PIV[@]}" -a status | grep -q "CN=fore insecure key (d = 0x4)" || fail "reset removed the certificate"
        "${PIV[@]}" -P "$PIN" -a verify-pin -a test-signature -s 9a -i "$PRE/preload-cert.pem"
    fi
    echo
    echo "e2e ($MODE): all tests passed"
    exit 0
fi

step "status of a blank device"
"${PIV[@]}" -a version -a status

step "import P-384 keys and certificates (default management key)"
for s in 9a 9c 9d; do
    openssl ecparam -name secp384r1 -genkey -noout -out "key$s.pem"
    openssl req -new -x509 -key "key$s.pem" -out "cert$s.pem" -days 30 -subj "/CN=fore $s"
    "${PIV[@]}" -a import-key -s "$s" -i "key$s.pem"
    "${PIV[@]}" -a import-certificate -s "$s" -i "cert$s.pem"
done

step "management key change, then import into 9e with the new key"
NEWMGM=0f0e0d0c0b0a09080706050403020100f0e0d0c0b0a09080
"${PIV[@]}" -a set-mgm-key -n "$NEWMGM"
if "${PIV[@]}" -a import-certificate -s 9e -i cert9a.pem 2>/dev/null; then
    fail "old management key still accepted"
fi
openssl ecparam -name secp384r1 -genkey -noout -out key9e.pem
openssl req -new -x509 -key key9e.pem -out cert9e.pem -days 30 -subj "/CN=fore 9e"
"${PIV[@]}" --key="$NEWMGM" -a import-key -s 9e -i key9e.pem
"${PIV[@]}" --key="$NEWMGM" -a import-certificate -s 9e -i cert9e.pem
"${PIV[@]}" -a status | tee status.txt
[ "$(grep -c ECCP384 status.txt)" = 4 ] || fail "expected four P-384 slots"

step "yubico-piv-tool signature tests"
"${PIV[@]}" -P 123456 -a verify-pin -a test-signature -s 9a -i cert9a.pem
"${PIV[@]}" -P 123456 -a verify-pin -a test-signature -s 9c -i cert9c.pem

step "OpenSC PKCS#11"
pkcs11-tool --module "$P11" -L
pkcs11-tool --module "$P11" --login --pin 123456 -O | tee objects.txt
grep -q "PIV AUTH key" objects.txt || fail "private key not listed"
echo "fore was here" >msg.txt
pkcs11-tool --module "$P11" --login --pin 123456 --sign -m ECDSA-SHA384 --id 01 \
    --signature-format openssl -i msg.txt -o p11.sig
openssl dgst -sha384 -verify <(openssl x509 -in cert9a.pem -pubkey -noout) -signature p11.sig msg.txt

step "OpenSSL via pkcs11-provider"
OPENSSL_CONF=$WORK/provider.cnf openssl dgst -sha384 -sign "pkcs11:object=PIV AUTH key;type=private" \
    -out prov9a.sig msg.txt
openssl dgst -sha384 -verify <(openssl x509 -in cert9a.pem -pubkey -noout) -signature prov9a.sig msg.txt
OPENSSL_CONF=$WORK/provider.cnf openssl dgst -sha384 -sign "pkcs11:object=SIGN key;type=private" \
    -out prov9c.sig msg.txt
openssl dgst -sha384 -verify <(openssl x509 -in cert9c.pem -pubkey -noout) -signature prov9c.sig msg.txt
OPENSSL_CONF=$WORK/provider.cnf openssl req -new -key "pkcs11:object=PIV AUTH key;type=private" \
    -subj "/CN=fore csr" -out req.pem
openssl req -in req.pem -verify -noout
[ "$(openssl req -in req.pem -pubkey -noout)" = "$(openssl x509 -in cert9a.pem -pubkey -noout)" ] ||
    fail "CSR public key does not match slot 9a"

step "OpenSSL via the libp11 engine (sign and ECDH)"
cat >engine.cnf <<EOF
openssl_conf = openssl_init
[openssl_init]
engines = engine_section
[engine_section]
pkcs11 = pkcs11_section
[pkcs11_section]
engine_id = pkcs11
dynamic_path = /usr/lib/$MULTIARCH/engines-3/pkcs11.so
MODULE_PATH = $P11
PIN = 123456
init = 0
EOF
OPENSSL_CONF=$WORK/engine.cnf openssl dgst -sha384 -engine pkcs11 -keyform engine \
    -sign "pkcs11:object=PIV AUTH key;type=private" -out eng.sig msg.txt
openssl dgst -sha384 -verify <(openssl x509 -in cert9a.pem -pubkey -noout) -signature eng.sig msg.txt
openssl ecparam -name secp384r1 -genkey -noout -out peer.pem
openssl ec -in peer.pem -pubout -out peerpub.pem
OPENSSL_CONF=$WORK/engine.cnf openssl pkeyutl -derive -engine pkcs11 -keyform engine \
    -inkey "pkcs11:object=KEY MAN key;type=private" -peerkey peerpub.pem -out shared.card
openssl pkeyutl -derive -inkey peer.pem -peerkey <(openssl ec -in key9d.pem -pubout) -out shared.ref
cmp shared.card shared.ref || fail "ECDH mismatch"

step "PIN handling"
if "${PIV[@]}" -P 999999 -a verify-pin; then fail "wrong PIN accepted"; fi
"${PIV[@]}" -a status | grep "PIN tries left:	2" || fail "retry counter not decremented"
"${PIV[@]}" -P 123456 -a verify-pin
"${PIV[@]}" -a status | grep "PIN tries left:	3" || fail "retry counter not reset"
"${PIV[@]}" -P 123456 -N 24680135 -a change-pin
"${PIV[@]}" -P 24680135 -a verify-pin
for _ in 1 2 3; do "${PIV[@]}" -P 000000 -a verify-pin || true; done
if "${PIV[@]}" -P 24680135 -a verify-pin; then fail "blocked PIN accepted"; fi
"${PIV[@]}" -P 12345678 -N 123456 -a unblock-pin
"${PIV[@]}" -P 123456 -a verify-pin -a test-signature -s 9a -i cert9a.pem

if [ "$MODE" = vpcd ]; then
    step "state survives a restart"
    kill "${PIDS[-1]}"
    sleep 0.5
    "$CARD" -s "$WORK/flash.bin" >>"$WORK/card.log" 2>&1 &
    PIDS+=($!)
    sleep 1
    "${PIV[@]}" -P 123456 -a verify-pin -a test-signature -s 9c -i cert9c.pem
fi

step "factory reset (needs PIN and PUK blocked)"
if "${PIV[@]}" -a reset 2>/dev/null; then fail "reset allowed while PIN is usable"; fi
for _ in 1 2 3; do "${PIV[@]}" -P 000000 -a verify-pin || true; done
for _ in 1 2 3; do "${PIV[@]}" -P 00000000 -N 123456 -a unblock-pin || true; done
"${PIV[@]}" -a reset
"${PIV[@]}" -a status | tee status.txt
if grep -q ECCP384 status.txt; then fail "keys survived reset"; fi
"${PIV[@]}" -P 123456 -a verify-pin

echo
echo "e2e ($MODE): all tests passed"
