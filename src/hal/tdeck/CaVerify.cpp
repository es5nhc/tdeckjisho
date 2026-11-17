// ============================================================================
//  hal/tdeck/CaVerify.cpp – checking a server's certificates against kCaBundle
// ============================================================================
//
//  WiFiClientSecure::setCACertBundle() hands our bundle (tools/make_ca_bundle.py)
//  to three C functions, arduino_esp_crt_bundle_set/attach/detach. The Arduino core
//  has its own versions (WiFiClientSecure/src/esp_crt_bundle.c, Espressif,
//  Apache-2.0). THIS FILE REPLACES THEM, because theirs rejects many real sites.
//
//  Why: mbedTLS walks the chain the server sends (site ← intermediate ← ...) and
//  calls our callback for the topmost certificate it could not trust on its own.
//  The core's callback asks only one question: "was this certificate signed by a
//  root in the bundle?" Many servers end their chain with a CROSS-SIGNED root, e.g.
//
//      err.ee ← WE1 ← "GTS Root R4", signed by the old "GlobalSign Root CA"
//
//  GTS Root R4 is in Mozilla's list; GlobalSign Root CA has been retired from it.
//  So the core's answer is "no", and every such site fails with
//  "connection refused". A PC (OpenSSL) notices that the cross-signed certificate
//  has the same name AND the same public key as the trusted GTS Root R4: it IS
//  that root, just with a different signature on it. We add that rule:
//
//    1. the certificate has the same subject and public key as a bundle root
//       (a trust anchor is a name plus a key: RFC 5280, section 6.1.1), or
//    2. it was signed by a bundle root (the core's rule).
//
//  Every link BELOW that certificate has already been checked by mbedTLS with the
//  key above it, and mbedTLS still checks dates and the host name on its own.
//
//  How the replacement takes effect: the linker resolves a function from the
//  first place that defines it. Our objects come first; the library archive is
//  only searched for what is still missing, and since all three functions are
//  defined here, the core's esp_crt_bundle.c is never linked in. (Defining only
//  some of them would pull it in and fail with "multiple definition".)
// ============================================================================

#include <cstdint>
#include <cstring>
#include <new>   // std::nothrow

#include <esp_err.h>          // esp_crt_bundle.h uses esp_err_t without including this
#include <esp_crt_bundle.h>   // the core's declarations: the compiler checks ours match
#include <mbedtls/md.h>
#include <mbedtls/pk.h>
#include <mbedtls/ssl.h>
#include <mbedtls/x509_crt.h>

#include "hal/Hal.hpp"

namespace {

// Bundle layout (see tools/make_ca_bundle.py), lengths big-endian:
//   u16 count, then per root: u16 name_len, u16 key_len, subject DER, key DER.
// The roots are sorted by subject, then key.
struct Root {
    const uint8_t* name;
    size_t nameLen;
    const uint8_t* key;
    size_t keyLen;
};

Root* g_roots = nullptr;
size_t g_count = 0;

// mbedTLS refuses to verify at all without a CA list, so it gets an empty one.
// The real decision happens in verify().
mbedtls_x509_crt g_emptyCa;

// Byte-wise order, shorter first on a common prefix: the same order as
// Python's sorted() on bytes, which built the bundle.
int compare(const uint8_t* a, size_t aLen, const uint8_t* b, size_t bLen) {
    const int c = std::memcmp(a, b, aLen < bLen ? aLen : bLen);
    if (c != 0) return c;
    return aLen < bLen ? -1 : (aLen > bLen ? 1 : 0);
}

bool equal(const uint8_t* a, size_t aLen, const uint8_t* b, size_t bLen) {
    return aLen == bLen && std::memcmp(a, b, aLen) == 0;
}

// Index of the first root whose name is >= name (binary search). Several roots
// can share a name (a CA renewing its key), so callers walk forward from here.
size_t firstWithName(const mbedtls_x509_buf& name) {
    size_t lo = 0, hi = g_count;
    while (lo < hi) {
        const size_t mid = lo + (hi - lo) / 2;
        if (compare(g_roots[mid].name, g_roots[mid].nameLen, name.p, name.len) < 0) lo = mid + 1;
        else hi = mid;
    }
    return lo;
}

// Rule 1: same subject and same public key as one of our roots.
bool isBundleRoot(const mbedtls_x509_crt& crt) {
    for (size_t i = firstWithName(crt.subject_raw);
         i < g_count && equal(g_roots[i].name, g_roots[i].nameLen, crt.subject_raw.p, crt.subject_raw.len); ++i)
        if (equal(g_roots[i].key, g_roots[i].keyLen, crt.pk_raw.p, crt.pk_raw.len)) return true;
    return false;
}

// Does `key` (a SubjectPublicKeyInfo) verify crt's signature?
bool signatureOk(const mbedtls_x509_crt& crt, const Root& root) {
    mbedtls_pk_context pk;
    mbedtls_pk_init(&pk);
    bool ok = false;
    const mbedtls_md_info_t* md = mbedtls_md_info_from_type(crt.sig_md);
    unsigned char hash[MBEDTLS_MD_MAX_SIZE];
    if (md && mbedtls_pk_parse_public_key(&pk, root.key, root.keyLen) == 0 &&
        mbedtls_pk_can_do(&pk, crt.sig_pk) &&
        mbedtls_md(md, crt.tbs.p, crt.tbs.len, hash) == 0) {
        ok = mbedtls_pk_verify_ext(crt.sig_pk, crt.sig_opts, &pk, crt.sig_md, hash,
                                   mbedtls_md_get_size(md), crt.sig.p, crt.sig.len) == 0;
    }
    mbedtls_pk_free(&pk);
    return ok;
}

// Rule 2: signed by one of our roots.
bool signedByBundleRoot(const mbedtls_x509_crt& crt) {
    for (size_t i = firstWithName(crt.issuer_raw);
         i < g_count && equal(g_roots[i].name, g_roots[i].nameLen, crt.issuer_raw.p, crt.issuer_raw.len); ++i)
        if (signatureOk(crt, g_roots[i])) return true;
    return false;
}

// mbedTLS calls this once per certificate in the chain, with the problems it
// found in `flags`. Clearing them accepts the certificate; returning an error
// aborts the handshake.
int verify(void*, mbedtls_x509_crt* crt, int depth, uint32_t* flags) {
    // A weak hash (e.g. SHA-1) in the signature ON a root doesn't matter: we
    // trust the root's key directly, not its signature.
    const uint32_t problems = *flags & ~MBEDTLS_X509_BADCERT_BAD_MD;
    // Nothing wrong, or something a root can't fix (expired, wrong host name...):
    // leave the flags alone, mbedTLS fails the handshake if any are set.
    if (problems != MBEDTLS_X509_BADCERT_NOT_TRUSTED) return 0;

    // Rule 1 only above the site's own certificate (depth 0): a root is never a
    // web site's certificate.
    if ((depth > 0 && isBundleRoot(*crt)) || signedByBundleRoot(*crt)) {
        *flags = 0;
        return 0;
    }
    char issuer[128];
    if (mbedtls_x509_dn_gets(issuer, sizeof issuer, &crt->issuer) < 0) std::strcpy(issuer, "?");
    hal::log("[tls] untrusted certificate (depth %d): no bundle root for issuer %s\n", depth, issuer);
    return MBEDTLS_ERR_X509_FATAL_ERROR;
}

}  // namespace

extern "C" {

void arduino_esp_crt_bundle_set(const uint8_t* bundle) {
    delete[] g_roots;
    g_count = static_cast<size_t>(bundle[0] << 8 | bundle[1]);
    g_roots = new (std::nothrow) Root[g_count];
    if (!g_roots) { g_count = 0; return; }   // verify() then trusts nothing: fails closed
    const uint8_t* p = bundle + 2;
    for (size_t i = 0; i < g_count; ++i) {
        const size_t nameLen = static_cast<size_t>(p[0] << 8 | p[1]);
        const size_t keyLen = static_cast<size_t>(p[2] << 8 | p[3]);
        g_roots[i] = {p + 4, nameLen, p + 4 + nameLen, keyLen};
        p += 4 + nameLen + keyLen;
    }
}

esp_err_t arduino_esp_crt_bundle_attach(void* conf) {
    if (conf) {
        auto* ssl = static_cast<mbedtls_ssl_config*>(conf);
        mbedtls_x509_crt_init(&g_emptyCa);
        mbedtls_ssl_conf_ca_chain(ssl, &g_emptyCa, nullptr);
        mbedtls_ssl_conf_verify(ssl, verify, nullptr);
    }
    return ESP_OK;
}

void arduino_esp_crt_bundle_detach(mbedtls_ssl_config* conf) {
    delete[] g_roots;
    g_roots = nullptr;
    g_count = 0;
    if (conf) mbedtls_ssl_conf_verify(conf, nullptr, nullptr);
}

}  // extern "C"
