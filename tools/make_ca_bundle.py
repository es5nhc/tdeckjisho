# ============================================================================
#  make_ca_bundle.py – the root certificates HTTPS checks servers against
# ============================================================================
#
#  TLS proves a server is who it claims to be with a chain of certificates
#  ending at a ROOT that we already trust. Browsers ship a list of trusted
#  roots; so must we. This script turns such a list (a PEM file, e.g. Mozilla's
#  cacert.pem) into the compact binary "x509 certificate bundle" that ESP-IDF's
#  esp_crt_bundle reads, and writes it as a C++ header:
#
#      static const unsigned kCaBundleCerts = 140;
#      static const uint8_t kCaBundle[] = { ...bundle bytes... };
#
#  hal/tdeck/Net.cpp passes kCaBundle to WiFiClientSecure::setCACertBundle().
#
#  Where the PEM comes from (you choose, the build never downloads anything):
#      tools/bundle/cacert.pem      e.g. a copy of /etc/ssl/certs/ca-certificates.crt
#                                   (Debian/Ubuntu: Mozilla's list) or curl.se/ca/cacert.pem
#  No file -> a bundle with ZERO certificates, and a loud warning. The firmware
#  then refuses every https:// request: FAIL CLOSED, never silently insecure.
#
#  Bundle format (ESP-IDF 4.4, esp_crt_bundle.c), all lengths big-endian:
#      u16 number of certificates
#      per certificate, sorted by subject:  u16 name_len, u16 key_len,
#                                           subject Name (DER), SubjectPublicKeyInfo (DER)
#  Only the subject and the public key are kept: that is all a ROOT is needed for
#  (match the issuer name of the chain's top certificate, check its signature).
#
#  Two ways to run it:
#    * by PlatformIO, as  extra_scripts = pre:tools/make_ca_bundle.py  (env:tdeck):
#      writes $BUILD_DIR/generated/ca_bundle.h and adds that folder to the include path.
#    * by hand, to look at the result:
#      python3 tools/make_ca_bundle.py tools/bundle/cacert.pem /tmp/ca_bundle.h
#
#  No third-party Python packages: the few DER fields we need are read by hand
#  (DER = the binary encoding of certificates: tag, length, value, nested).
# ============================================================================

import base64
import os
import re
import struct
import sys

MAX_CERTS = 200          # esp_crt_bundle's limit (CONFIG_MBEDTLS_CERTIFICATE_BUNDLE_MAX_CERTS)


def tlv(buf, pos, end):
    """One DER element at buf[pos]: returns (tag, start, content_start, content_end)."""
    if pos + 2 > end:
        raise ValueError("truncated DER")
    tag, length = buf[pos], buf[pos + 1]
    p = pos + 2
    if length & 0x80:                          # long form: the next n bytes are the length
        n = length & 0x7F
        if n == 0 or n > 4 or p + n > end:
            raise ValueError("bad DER length")
        length = int.from_bytes(buf[p:p + n], "big")
        p += n
    if p + length > end:
        raise ValueError("DER element runs past its parent")
    return tag, pos, p, p + length


def subject_and_key(der):
    """The raw DER of a certificate's subject Name and SubjectPublicKeyInfo."""
    tag, _, c0, c1 = tlv(der, 0, len(der))               # Certificate ::= SEQUENCE
    if tag != 0x30:
        raise ValueError("not a certificate")
    tag, _, t0, t1 = tlv(der, c0, c1)                    # tbsCertificate ::= SEQUENCE
    if tag != 0x30:
        raise ValueError("no tbsCertificate")
    fields, p = [], t0
    while p < t1:
        f = tlv(der, p, t1)
        fields.append(f)
        p = f[3]
    if fields and fields[0][0] == 0xA0:                  # [0] version: optional, skip
        fields = fields[1:]
    # serialNumber, signature, issuer, validity, subject, subjectPublicKeyInfo, ...
    if len(fields) < 6 or fields[4][0] != 0x30 or fields[5][0] != 0x30:
        raise ValueError("unexpected certificate layout")
    subject = der[fields[4][1]:fields[4][3]]
    key = der[fields[5][1]:fields[5][3]]
    return subject, key


def read_pem(path):
    text = open(path, encoding="ascii", errors="replace").read()
    blocks = re.findall(r"-----BEGIN CERTIFICATE-----(.*?)-----END CERTIFICATE-----", text, re.S)
    return [base64.b64decode("".join(b.split())) for b in blocks]


def build_bundle(pem_path):
    certs, skipped = [], 0
    for der in read_pem(pem_path) if pem_path and os.path.exists(pem_path) else []:
        try:
            subject, key = subject_and_key(der)
        except ValueError:
            skipped += 1
            continue
        if len(subject) > 0xFFFF or len(key) > 0xFFFF:
            skipped += 1
            continue
        certs.append((subject, key))
    certs = sorted(set(certs))                           # sorted by subject (then key); no duplicates
    if len(certs) > MAX_CERTS:
        raise SystemExit(f"make_ca_bundle: {len(certs)} roots, ESP-IDF accepts at most {MAX_CERTS}")
    out = bytearray(struct.pack(">H", len(certs)))
    for subject, key in certs:
        out += struct.pack(">HH", len(subject), len(key)) + subject + key
    return bytes(out), len(certs), skipped


def header(bundle, count, source):
    lines = [
        "// Generated by tools/make_ca_bundle.py – do not edit.",
        f"// Source: {source or '(none: HTTPS disabled, see README)'}",
        "#pragma once",
        "#include <cstdint>",
        f"static const unsigned kCaBundleCerts = {count};",
        "alignas(4) static const uint8_t kCaBundle[] = {",
    ]
    for i in range(0, len(bundle), 20):
        lines.append("    " + ", ".join(f"0x{b:02x}" for b in bundle[i:i + 20]) + ",")
    lines.append("};")
    return "\n".join(lines) + "\n"


def generate(pem_path, out_path):
    bundle, count, skipped = build_bundle(pem_path)
    os.makedirs(os.path.dirname(out_path) or ".", exist_ok=True)
    text = header(bundle, count, pem_path if count else None)
    if not os.path.exists(out_path) or open(out_path).read() != text:   # unchanged: no rebuild
        with open(out_path, "w") as f:
            f.write(text)
    if count == 0:
        print("*" * 78)
        print("make_ca_bundle: NO root certificates (tools/bundle/cacert.pem missing).")
        print("                The firmware will REFUSE every https:// request. See README.md.")
        print("*" * 78)
    else:
        print(f"make_ca_bundle: {count} root certificates, {len(bundle)} bytes"
              + (f" ({skipped} skipped)" if skipped else ""))


try:
    Import("env")  # noqa: F821  (only defined when PlatformIO/SCons runs this file)
except NameError:
    env = None

if env is not None:
    root = env.subst("$PROJECT_DIR")
    out_dir = os.path.join(env.subst("$BUILD_DIR"), "generated")
    generate(os.path.join(root, "tools", "bundle", "cacert.pem"), os.path.join(out_dir, "ca_bundle.h"))
    env.Append(CPPPATH=[out_dir])
elif __name__ == "__main__":
    if len(sys.argv) != 3:
        sys.exit("usage: make_ca_bundle.py <cacert.pem> <out.h>")
    generate(sys.argv[1], sys.argv[2])
