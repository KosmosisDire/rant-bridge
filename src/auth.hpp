#pragma once
/* The passphrase never crosses the wire: a client asks for a nonce and opens with a proof,
 * HMAC SHA-256 of the nonce keyed with the passphrase (Rant's spec/tunnel.md). */

#include <string>

namespace bridge {

/* 32 hex characters from 16 random bytes, good for one open on one connection. */
std::string new_nonce();

/* The proof a client sends for nonce, lowercase hex. */
std::string passphrase_proof(const std::string& passphrase, const std::string& nonce);

/* Constant time, so a secret cannot be found one character at a time. */
bool same_secret(const std::string& a, const std::string& b);

}
