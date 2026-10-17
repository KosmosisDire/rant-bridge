// The passphrase proof against RFC 4231's HMAC SHA-256 vectors, and fresh nonces.

#include "auth.hpp"

#include <cstdio>
#include <string>

using namespace bridge;

static int failed = 0;
static void check(bool ok, const char* what) {
    std::printf("%s %s\n", ok ? "ok  " : "FAIL", what);
    if (!ok) failed++;
}

int main() {
    check(passphrase_proof("Jefe", "what do ya want for nothing?")
              == "5bdcc146bf60754e6a042426089575c75a003f089d2739839dec58b964ec3843",
          "the proof is RFC 4231's HMAC SHA-256, case 2");
    check(passphrase_proof(std::string(131, '\xaa'), "Test Using Larger Than Block-Size Key - Hash Key First")
              == "60e431591ee0b67f0d8a26aacbf5b77f8e0bc6213728c5140546040f0ee37f54",
          "a key longer than a block is hashed first, case 6");
    check(passphrase_proof("", "x").size() == 64, "an empty passphrase still proves");
    std::string a = new_nonce(), b = new_nonce();
    check(a.size() == 32 && a.find_first_not_of("0123456789abcdef") == std::string::npos, "a nonce is 32 hex characters");
    check(a != b, "each nonce is new");
    check(same_secret("abc", "abc") && !same_secret("abc", "abd") && !same_secret("abc", "ab"), "secrets compare whole");
    return failed ? 1 : 0;
}
