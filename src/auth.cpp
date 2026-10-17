#include "auth.hpp"

#include <mbedtls/md.h>

#include <random>

namespace bridge {

static std::string hex(const unsigned char* p, size_t n) {
    static const char digits[] = "0123456789abcdef";
    std::string s(n * 2, '0');
    for (size_t i = 0; i < n; i++) { s[2 * i] = digits[p[i] >> 4]; s[2 * i + 1] = digits[p[i] & 15]; }
    return s;
}

std::string new_nonce() {
    std::random_device rd;
    unsigned char b[16];
    for (unsigned char& x : b) x = (unsigned char)rd();
    return hex(b, sizeof b);
}

std::string passphrase_proof(const std::string& passphrase, const std::string& nonce) {
    unsigned char mac[32];
    mbedtls_md_hmac(mbedtls_md_info_from_type(MBEDTLS_MD_SHA256), (const unsigned char*)passphrase.data(), passphrase.size(),
                    (const unsigned char*)nonce.data(), nonce.size(), mac);
    return hex(mac, sizeof mac);
}

bool same_secret(const std::string& a, const std::string& b) {
    unsigned diff = (unsigned)(a.size() ^ b.size());
    for (size_t i = 0; i < a.size() && i < b.size(); i++) diff |= (unsigned)(a[i] ^ b[i]);
    return diff == 0;
}

}
