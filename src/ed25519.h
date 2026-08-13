// SPDX-License-Identifier: AGPL-3.0-or-later
// SPDX-FileCopyrightText: (c) 2026 SHD Systems Ltd
//
// The TweetNaCl-derived code below is public domain; the SPDX line above covers
// this file's place in the kit, not the vendored arithmetic.

#pragma once

// Ed25519 signature verification, SHA-512, and SHA-256.
//
// ── Why an installer needs this ────────────────────────────────────────────
// A downloading installer writes executables it fetched from the internet into
// the install directory, before any licence check exists, on every machine it
// runs on. TLS authenticates the *host*; it says nothing about the payload, and
// it does not survive a compromised bucket, a mistaken upload or a proxy that
// is "helpfully" caching. So:
//
//   1. The release manifest carries a detached Ed25519 signature, verified here
//      against a public key COMPILED INTO this binary, before anything the
//      manifest says is believed.
//   2. Every component is SHA-256'd after download and before it is unpacked.
//
// The key being compiled in is the point. A client that reads the verifying key
// from the same place it reads the manifest is checking a signature against a
// key the attacker also supplied.
//
// Verification only. This program has no business holding a private key or
// producing a signature, so the code to do either is deliberately absent - if it
// is not here it cannot be extracted from a customer's install.
//
// The implementation is derived from TweetNaCl (Bernstein, Janssen, Lange,
// Schwabe; public domain), reduced to the verify path. It is vendored rather
// than linked because the Qt build carries no OpenSSL headers and an installer
// must not gain a deployment dependency to check a signature. This is the same
// implementation the SHD Sim application uses for licence files, kept
// line-for-line so the two can be audited together.
//
// Qt-free, so it can be tested without a display.

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace shdkit {

// The 64-byte digest of `data`.
std::vector<unsigned char> sha512(const unsigned char* data, size_t len);

// True when `signature` (64 bytes) is a valid Ed25519 signature over `message`
// under `publicKey` (32 bytes). False for any wrong length, so a caller cannot
// pass a truncated key and get an accidental pass.
bool ed25519Verify(const unsigned char* message, size_t messageLen,
                   const unsigned char* signature, size_t signatureLen,
                   const unsigned char* publicKey, size_t publicKeyLen);

// The same over strings, which is how a licence file arrives.
bool ed25519Verify(const std::string& message,
                   const std::string& signature,
                   const std::string& publicKey);

// Base64, because a signature arrives as base64 text beside the manifest.
// Returns an empty vector on malformed input rather than guessing.
std::vector<unsigned char> base64Decode(const std::string& text);
std::string base64Encode(const unsigned char* data, size_t len);

// ── SHA-256, for component payloads ───────────────────────────────────────
// Separate from the signature check and doing a different job: the signature
// says the manifest is ours, the digest says the 624 MB we just downloaded is
// the 624 MB the manifest described. Both are required before anything is
// unpacked.
//
// Streaming, because the payloads are hundreds of megabytes and an installer
// that holds one in memory to hash it will fail on exactly the machines that
// can least afford it.
class Sha256 {
public:
    Sha256();
    void update(const unsigned char* data, size_t len);
    // Lower-case hex, to compare against the manifest without allocating.
    std::string hexDigest();

private:
    uint32_t m_state[8];
    uint64_t m_bits = 0;
    unsigned char m_buffer[64];
    size_t m_bufferLen = 0;
};

}  // namespace shdkit
