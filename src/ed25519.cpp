// Ed25519 verification and SHA-512.
//
// Derived from TweetNaCl (Daniel J. Bernstein, Bernard van Gastel, Wesley
// Janssen, Tanja Lange, Peter Schwabe, Sjaak Smetsers; public domain), reduced
// to the verification path and the hash it needs. The arithmetic is left in the
// shape TweetNaCl gives it: it is published, reviewed code, and rewriting it to
// read more naturally would be trading an audited implementation for a prettier
// one. Names are TweetNaCl's for the same reason - anyone checking this against
// the original should be able to do so line by line.

#include "ed25519.h"

#include <cstring>

namespace shdkit {
namespace {

using u8  = unsigned char;
using u32 = uint32_t;
using u64 = uint64_t;
using i64 = int64_t;
using gf  = i64[16];

const u64 K[80] = {
    0x428a2f98d728ae22ULL, 0x7137449123ef65cdULL, 0xb5c0fbcfec4d3b2fULL, 0xe9b5dba58189dbbcULL,
    0x3956c25bf348b538ULL, 0x59f111f1b605d019ULL, 0x923f82a4af194f9bULL, 0xab1c5ed5da6d8118ULL,
    0xd807aa98a3030242ULL, 0x12835b0145706fbeULL, 0x243185be4ee4b28cULL, 0x550c7dc3d5ffb4e2ULL,
    0x72be5d74f27b896fULL, 0x80deb1fe3b1696b1ULL, 0x9bdc06a725c71235ULL, 0xc19bf174cf692694ULL,
    0xe49b69c19ef14ad2ULL, 0xefbe4786384f25e3ULL, 0x0fc19dc68b8cd5b5ULL, 0x240ca1cc77ac9c65ULL,
    0x2de92c6f592b0275ULL, 0x4a7484aa6ea6e483ULL, 0x5cb0a9dcbd41fbd4ULL, 0x76f988da831153b5ULL,
    0x983e5152ee66dfabULL, 0xa831c66d2db43210ULL, 0xb00327c898fb213fULL, 0xbf597fc7beef0ee4ULL,
    0xc6e00bf33da88fc2ULL, 0xd5a79147930aa725ULL, 0x06ca6351e003826fULL, 0x142929670a0e6e70ULL,
    0x27b70a8546d22ffcULL, 0x2e1b21385c26c926ULL, 0x4d2c6dfc5ac42aedULL, 0x53380d139d95b3dfULL,
    0x650a73548baf63deULL, 0x766a0abb3c77b2a8ULL, 0x81c2c92e47edaee6ULL, 0x92722c851482353bULL,
    0xa2bfe8a14cf10364ULL, 0xa81a664bbc423001ULL, 0xc24b8b70d0f89791ULL, 0xc76c51a30654be30ULL,
    0xd192e819d6ef5218ULL, 0xd69906245565a910ULL, 0xf40e35855771202aULL, 0x106aa07032bbd1b8ULL,
    0x19a4c116b8d2d0c8ULL, 0x1e376c085141ab53ULL, 0x2748774cdf8eeb99ULL, 0x34b0bcb5e19b48a8ULL,
    0x391c0cb3c5c95a63ULL, 0x4ed8aa4ae3418acbULL, 0x5b9cca4f7763e373ULL, 0x682e6ff3d6b2b8a3ULL,
    0x748f82ee5defb2fcULL, 0x78a5636f43172f60ULL, 0x84c87814a1f0ab72ULL, 0x8cc702081a6439ecULL,
    0x90befffa23631e28ULL, 0xa4506cebde82bde9ULL, 0xbef9a3f7b2c67915ULL, 0xc67178f2e372532bULL,
    0xca273eceea26619cULL, 0xd186b8c721c0c207ULL, 0xeada7dd6cde0eb1eULL, 0xf57d4f7fee6ed178ULL,
    0x06f067aa72176fbaULL, 0x0a637dc5a2c898a6ULL, 0x113f9804bef90daeULL, 0x1b710b35131c471bULL,
    0x28db77f523047d84ULL, 0x32caab7b40c72493ULL, 0x3c9ebe0a15c9bebcULL, 0x431d67c49c100d4cULL,
    0x4cc5d4becb3e42b6ULL, 0x597f299cfc657e2aULL, 0x5fcb6fab3ad6faecULL, 0x6c44198c4a475817ULL
};

const gf gf0 = {0}, gf1 = {1},
         D  = {0x78a3, 0x1359, 0x4dca, 0x75eb, 0xd8ab, 0x4141, 0x0a4d, 0x0070,
               0xe898, 0x7779, 0x4079, 0x8cc7, 0xfe73, 0x2b6f, 0x6cee, 0x5203},
         D2 = {0xf159, 0x26b2, 0x9b94, 0xebd6, 0xb156, 0x8283, 0x149a, 0x00e0,
               0xd130, 0xeef3, 0x80f2, 0x198e, 0xfce7, 0x56df, 0xd9dc, 0x2406},
         X  = {0xd51a, 0x8f25, 0x2d60, 0xc956, 0xa7b2, 0x9525, 0xc760, 0x692c,
               0xdc5c, 0xfdd6, 0xe231, 0xc0a4, 0x53fe, 0xcd6e, 0x36d3, 0x2169},
         Y  = {0x6658, 0x6666, 0x6666, 0x6666, 0x6666, 0x6666, 0x6666, 0x6666,
               0x6666, 0x6666, 0x6666, 0x6666, 0x6666, 0x6666, 0x6666, 0x6666},
         I  = {0xa0b0, 0x4a0e, 0x1b27, 0xc4ee, 0xe478, 0xad2f, 0x1806, 0x2f43,
               0xd7a7, 0x3dfb, 0x0099, 0x2b4d, 0xdf0b, 0x4fc1, 0x2480, 0x2b83};

u64 dl64(const u8* x)
{
    u64 u = 0;
    for (int i = 0; i < 8; ++i) u = (u << 8) | x[i];
    return u;
}

void ts64(u8* x, u64 u)
{
    for (int i = 7; i >= 0; --i) { x[i] = u8(u); u >>= 8; }
}

int vn(const u8* x, const u8* y, int n)
{
    u32 d = 0;
    for (int i = 0; i < n; ++i) d |= u32(x[i] ^ y[i]);
    return (1 & ((d - 1) >> 8)) - 1;
}

int crypto_verify_32(const u8* x, const u8* y) { return vn(x, y, 32); }

void set25519(gf r, const gf a) { for (int i = 0; i < 16; ++i) r[i] = a[i]; }

void car25519(gf o)
{
    for (int i = 0; i < 16; ++i)
    {
        o[i] += (1LL << 16);
        i64 c = o[i] >> 16;
        o[(i + 1) * (i < 15)] += c - 1 + 37 * (c - 1) * (i == 15);
        o[i] -= c << 16;
    }
}

void sel25519(gf p, gf q, i64 b)
{
    const i64 c = ~(b - 1);
    for (int i = 0; i < 16; ++i)
    {
        const i64 t = c & (p[i] ^ q[i]);
        p[i] ^= t;
        q[i] ^= t;
    }
}

void pack25519(u8* o, const gf n)
{
    gf m, t;
    set25519(t, n);
    car25519(t); car25519(t); car25519(t);
    for (int j = 0; j < 2; ++j)
    {
        m[0] = t[0] - 0xffed;
        for (int i = 1; i < 15; ++i)
        {
            m[i] = t[i] - 0xffff - ((m[i - 1] >> 16) & 1);
            m[i - 1] &= 0xffff;
        }
        m[15] = t[15] - 0x7fff - ((m[14] >> 16) & 1);
        const i64 b = (m[15] >> 16) & 1;
        m[14] &= 0xffff;
        sel25519(t, m, 1 - b);
    }
    for (int i = 0; i < 16; ++i)
    {
        o[2 * i]     = u8(t[i] & 0xff);
        o[2 * i + 1] = u8(t[i] >> 8);
    }
}

int neq25519(const gf a, const gf b)
{
    u8 c[32], d[32];
    pack25519(c, a);
    pack25519(d, b);
    return crypto_verify_32(c, d);
}

u8 par25519(const gf a)
{
    u8 d[32];
    pack25519(d, a);
    return d[0] & 1;
}

void unpack25519(gf o, const u8* n)
{
    for (int i = 0; i < 16; ++i) o[i] = n[2 * i] + (i64(n[2 * i + 1]) << 8);
    o[15] &= 0x7fff;
}

void A(gf o, const gf a, const gf b) { for (int i = 0; i < 16; ++i) o[i] = a[i] + b[i]; }
void Z(gf o, const gf a, const gf b) { for (int i = 0; i < 16; ++i) o[i] = a[i] - b[i]; }

void M(gf o, const gf a, const gf b)
{
    i64 t[31];
    for (int i = 0; i < 31; ++i) t[i] = 0;
    for (int i = 0; i < 16; ++i)
        for (int j = 0; j < 16; ++j) t[i + j] += a[i] * b[j];
    for (int i = 0; i < 15; ++i) t[i] += 38 * t[i + 16];
    for (int i = 0; i < 16; ++i) o[i] = t[i];
    car25519(o);
    car25519(o);
}

void S(gf o, const gf a) { M(o, a, a); }

void inv25519(gf o, const gf i)
{
    gf c;
    set25519(c, i);
    for (int a = 253; a >= 0; --a)
    {
        S(c, c);
        if (a != 2 && a != 4) M(c, c, i);
    }
    set25519(o, c);
}

void pow2523(gf o, const gf i)
{
    gf c;
    set25519(c, i);
    for (int a = 250; a >= 0; --a)
    {
        S(c, c);
        if (a != 1) M(c, c, i);
    }
    set25519(o, c);
}

int crypto_hash(u8* out, const u8* m, u64 n);

void add(gf p[4], gf q[4])
{
    gf a, b, c, d, t, e, f, g, h;
    Z(a, p[1], p[0]); Z(t, q[1], q[0]); M(a, a, t);
    A(b, p[0], p[1]); A(t, q[0], q[1]); M(b, b, t);
    M(c, p[3], q[3]); M(c, c, D2);
    M(d, p[2], q[2]); A(d, d, d);
    Z(e, b, a); Z(f, d, c); A(g, d, c); A(h, b, a);
    M(p[0], e, f); M(p[1], h, g); M(p[2], g, f); M(p[3], e, h);
}

void cswap(gf p[4], gf q[4], u8 b)
{
    for (int i = 0; i < 4; ++i) sel25519(p[i], q[i], b);
}

void pack(u8* r, gf p[4])
{
    gf tx, ty, zi;
    inv25519(zi, p[2]);
    M(tx, p[0], zi);
    M(ty, p[1], zi);
    pack25519(r, ty);
    r[31] ^= par25519(tx) << 7;
}

void scalarmult(gf p[4], gf q[4], const u8* s)
{
    set25519(p[0], gf0); set25519(p[1], gf1);
    set25519(p[2], gf1); set25519(p[3], gf0);
    for (int i = 255; i >= 0; --i)
    {
        const u8 b = (s[i / 8] >> (i & 7)) & 1;
        cswap(p, q, b);
        add(q, p);
        add(p, p);
        cswap(p, q, b);
    }
}

void scalarbase(gf p[4], const u8* s)
{
    gf q[4];
    set25519(q[0], X); set25519(q[1], Y); set25519(q[2], gf1);
    M(q[3], X, Y);
    scalarmult(p, q, s);
}

const u64 L[32] = { 0xed, 0xd3, 0xf5, 0x5c, 0x1a, 0x63, 0x12, 0x58,
                    0xd6, 0x9c, 0xf7, 0xa2, 0xde, 0xf9, 0xde, 0x14,
                    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0x10 };

void modL(u8* r, i64* x)
{
    for (int i = 63; i >= 32; --i)
    {
        i64 carry = 0;
        int j = i - 32;
        for (; j < i - 12; ++j)
        {
            x[j] += carry - 16 * x[i] * i64(L[j - (i - 32)]);
            carry = (x[j] + 128) >> 8;
            x[j] -= carry << 8;
        }
        x[j] += carry;
        x[i] = 0;
    }
    i64 carry = 0;
    for (int j = 0; j < 32; ++j)
    {
        x[j] += carry - (x[31] >> 4) * i64(L[j]);
        carry = x[j] >> 8;
        x[j] &= 255;
    }
    for (int j = 0; j < 32; ++j) x[j] -= carry * i64(L[j]);
    for (int i = 0; i < 32; ++i)
    {
        x[i + 1] += x[i] >> 8;
        r[i] = u8(x[i] & 255);
    }
}

void reduce(u8* r)
{
    i64 x[64];
    for (int i = 0; i < 64; ++i) x[i] = i64(u64(r[i]));
    for (int i = 0; i < 64; ++i) r[i] = 0;
    modL(r, x);
}

int unpackneg(gf r[4], const u8 p[32])
{
    gf t, chk, num, den, den2, den4, den6;
    set25519(r[2], gf1);
    unpack25519(r[1], p);
    S(num, r[1]);
    M(den, num, D);
    Z(num, num, r[2]);
    A(den, r[2], den);

    S(den2, den);
    S(den4, den2);
    M(den6, den4, den2);
    M(t, den6, num);
    M(t, t, den);

    pow2523(t, t);
    M(t, t, num);
    M(t, t, den);
    M(t, t, den);
    M(r[0], t, den);

    S(chk, r[0]);
    M(chk, chk, den);
    if (neq25519(chk, num)) M(r[0], r[0], I);

    S(chk, r[0]);
    M(chk, chk, den);
    if (neq25519(chk, num)) return -1;

    if (par25519(r[0]) == (p[31] >> 7)) Z(r[0], gf0, r[0]);

    M(r[3], r[0], r[1]);
    return 0;
}

}  // namespace

// ---------------------------------------------------------------- SHA-512

namespace {

u64 R(u64 x, int c) { return (x >> c) | (x << (64 - c)); }
u64 Ch(u64 x, u64 y, u64 z)  { return (x & y) ^ (~x & z); }
u64 Maj(u64 x, u64 y, u64 z) { return (x & y) ^ (x & z) ^ (y & z); }
u64 Sigma0(u64 x) { return R(x, 28) ^ R(x, 34) ^ R(x, 39); }
u64 Sigma1(u64 x) { return R(x, 14) ^ R(x, 18) ^ R(x, 41); }
u64 sigma0(u64 x) { return R(x,  1) ^ R(x,  8) ^ (x >> 7); }
u64 sigma1(u64 x) { return R(x, 19) ^ R(x, 61) ^ (x >> 6); }

int crypto_hashblocks(u8* x, const u8* m, u64 n)
{
    u64 z[8], b[8], a[8], w[16], t;

    for (int i = 0; i < 8; ++i) z[i] = a[i] = dl64(x + 8 * i);

    while (n >= 128)
    {
        for (int i = 0; i < 16; ++i) w[i] = dl64(m + 8 * i);

        for (int i = 0; i < 80; ++i)
        {
            for (int j = 0; j < 8; ++j) b[j] = a[j];
            t = a[7] + Sigma1(a[4]) + Ch(a[4], a[5], a[6]) + K[i] + w[i % 16];
            b[7] = t + Sigma0(a[0]) + Maj(a[0], a[1], a[2]);
            b[3] += t;
            for (int j = 0; j < 8; ++j) a[(j + 1) % 8] = b[j];
            if (i % 16 == 15)
                for (int j = 0; j < 16; ++j)
                    w[j] += w[(j + 9) % 16]
                          + sigma0(w[(j + 1) % 16]) + sigma1(w[(j + 14) % 16]);
        }

        for (int i = 0; i < 8; ++i) { a[i] += z[i]; z[i] = a[i]; }
        m += 128;
        n -= 128;
    }

    for (int i = 0; i < 8; ++i) ts64(x + 8 * i, z[i]);
    return 0;
}

const u8 iv[64] = {
    0x6a,0x09,0xe6,0x67,0xf3,0xbc,0xc9,0x08, 0xbb,0x67,0xae,0x85,0x84,0xca,0xa7,0x3b,
    0x3c,0x6e,0xf3,0x72,0xfe,0x94,0xf8,0x2b, 0xa5,0x4f,0xf5,0x3a,0x5f,0x1d,0x36,0xf1,
    0x51,0x0e,0x52,0x7f,0xad,0xe6,0x82,0xd1, 0x9b,0x05,0x68,0x8c,0x2b,0x3e,0x6c,0x1f,
    0x1f,0x83,0xd9,0xab,0xfb,0x41,0xbd,0x6b, 0x5b,0xe0,0xcd,0x19,0x13,0x7e,0x21,0x79
};

int crypto_hash(u8* out, const u8* m, u64 n)
{
    u8 h[64], x[256];
    const u64 b = n;

    std::memcpy(h, iv, 64);
    crypto_hashblocks(h, m, n);
    m += n;
    n &= 127;
    m -= n;

    std::memset(x, 0, 256);
    for (u64 i = 0; i < n; ++i) x[i] = m[i];
    x[n] = 128;

    n = 256 - 128 * (n < 112);
    x[n - 9] = u8(b >> 61);
    ts64(x + n - 8, b << 3);
    crypto_hashblocks(h, x, n);

    for (int i = 0; i < 64; ++i) out[i] = h[i];
    return 0;
}

}  // namespace

std::vector<unsigned char> sha512(const unsigned char* data, size_t len)
{
    std::vector<unsigned char> out(64);
    crypto_hash(out.data(), data, u64(len));
    return out;
}

bool ed25519Verify(const unsigned char* message, size_t messageLen,
                   const unsigned char* signature, size_t signatureLen,
                   const unsigned char* publicKey, size_t publicKeyLen)
{
    // Wrong lengths are a hard no rather than something to work around: a
    // truncated key must never verify.
    if (signatureLen != 64 || publicKeyLen != 32) return false;

    gf q[4];
    if (unpackneg(q, publicKey)) return false;

    // TweetNaCl verifies the concatenation signature || message, so it is built
    // here rather than asking every caller to do it.
    std::vector<u8> sm(64 + messageLen);
    std::memcpy(sm.data(), signature, 64);
    if (messageLen) std::memcpy(sm.data() + 64, message, messageLen);

    std::vector<u8> m(sm.size());
    std::memcpy(m.data(), sm.data(), sm.size());
    std::memcpy(m.data() + 32, publicKey, 32);

    u8 h[64];
    crypto_hash(h, m.data(), u64(m.size()));
    reduce(h);

    gf p[4];
    scalarmult(p, q, h);

    gf t[4];
    scalarbase(t, sm.data() + 32);
    add(p, t);

    u8 packed[32];
    pack(packed, p);
    return crypto_verify_32(sm.data(), packed) == 0;
}

bool ed25519Verify(const std::string& message,
                   const std::string& signature,
                   const std::string& publicKey)
{
    return ed25519Verify(
        reinterpret_cast<const unsigned char*>(message.data()), message.size(),
        reinterpret_cast<const unsigned char*>(signature.data()), signature.size(),
        reinterpret_cast<const unsigned char*>(publicKey.data()), publicKey.size());
}

// ---------------------------------------------------------------- base64

namespace {
const char* kAlphabet =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
}

std::string base64Encode(const unsigned char* data, size_t len)
{
    std::string out;
    out.reserve(((len + 2) / 3) * 4);
    for (size_t i = 0; i < len; i += 3)
    {
        const unsigned v = (unsigned(data[i]) << 16)
                         | (i + 1 < len ? unsigned(data[i + 1]) << 8 : 0)
                         | (i + 2 < len ? unsigned(data[i + 2]) : 0);
        out += kAlphabet[(v >> 18) & 63];
        out += kAlphabet[(v >> 12) & 63];
        out += (i + 1 < len) ? kAlphabet[(v >> 6) & 63] : '=';
        out += (i + 2 < len) ? kAlphabet[v & 63] : '=';
    }
    return out;
}

std::vector<unsigned char> base64Decode(const std::string& text)
{
    int table[256];
    for (int i = 0; i < 256; ++i) table[i] = -1;
    for (int i = 0; i < 64; ++i) table[int(u8(kAlphabet[i]))] = i;

    // URL-safe base64 is what a JSON web token carries, and a licence file may
    // arrive either way; accepting both costs two lines and removes a whole
    // class of "works in staging" bug.
    table[int(u8('-'))] = 62;
    table[int(u8('_'))] = 63;

    std::vector<unsigned char> out;
    out.reserve(text.size() * 3 / 4 + 3);

    int bits = 0, held = 0;
    for (const char ch : text)
    {
        if (ch == '=' ) break;
        if (ch == '\n' || ch == '\r' || ch == ' ' || ch == '\t') continue;
        const int v = table[int(u8(ch))];
        if (v < 0) return {};            // malformed, rather than a guess
        held = (held << 6) | v;
        bits += 6;
        if (bits >= 8)
        {
            bits -= 8;
            out.push_back(u8((held >> bits) & 0xff));
        }
    }
    return out;
}

// ── SHA-256 ───────────────────────────────────────────────────────────────
// FIPS 180-4. Written out rather than derived from the SHA-512 above: the two
// share a shape but not a word size or a constant table, and the "clever"
// unification of them is a well-known way to produce something that passes the
// short vectors and fails on a 2 GB input.

namespace {

const uint32_t K256[64] = {
    0x428a2f98u, 0x71374491u, 0xb5c0fbcfu, 0xe9b5dba5u, 0x3956c25bu, 0x59f111f1u,
    0x923f82a4u, 0xab1c5ed5u, 0xd807aa98u, 0x12835b01u, 0x243185beu, 0x550c7dc3u,
    0x72be5d74u, 0x80deb1feu, 0x9bdc06a7u, 0xc19bf174u, 0xe49b69c1u, 0xefbe4786u,
    0x0fc19dc6u, 0x240ca1ccu, 0x2de92c6fu, 0x4a7484aau, 0x5cb0a9dcu, 0x76f988dau,
    0x983e5152u, 0xa831c66du, 0xb00327c8u, 0xbf597fc7u, 0xc6e00bf3u, 0xd5a79147u,
    0x06ca6351u, 0x14292967u, 0x27b70a85u, 0x2e1b2138u, 0x4d2c6dfcu, 0x53380d13u,
    0x650a7354u, 0x766a0abbu, 0x81c2c92eu, 0x92722c85u, 0xa2bfe8a1u, 0xa81a664bu,
    0xc24b8b70u, 0xc76c51a3u, 0xd192e819u, 0xd6990624u, 0xf40e3585u, 0x106aa070u,
    0x19a4c116u, 0x1e376c08u, 0x2748774cu, 0x34b0bcb5u, 0x391c0cb3u, 0x4ed8aa4au,
    0x5b9cca4fu, 0x682e6ff3u, 0x748f82eeu, 0x78a5636fu, 0x84c87814u, 0x8cc70208u,
    0x90befffau, 0xa4506cebu, 0xbef9a3f7u, 0xc67178f2u
};

inline uint32_t ror32(uint32_t x, int n) { return (x >> n) | (x << (32 - n)); }

void sha256Block(uint32_t state[8], const unsigned char block[64])
{
    uint32_t w[64];
    for (int i = 0; i < 16; ++i) {
        w[i] = (uint32_t(block[i * 4]) << 24) | (uint32_t(block[i * 4 + 1]) << 16) |
               (uint32_t(block[i * 4 + 2]) << 8) | uint32_t(block[i * 4 + 3]);
    }
    for (int i = 16; i < 64; ++i) {
        const uint32_t s0 = ror32(w[i - 15], 7) ^ ror32(w[i - 15], 18) ^ (w[i - 15] >> 3);
        const uint32_t s1 = ror32(w[i - 2], 17) ^ ror32(w[i - 2], 19) ^ (w[i - 2] >> 10);
        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }

    uint32_t a = state[0], b = state[1], c = state[2], d = state[3];
    uint32_t e = state[4], f = state[5], g = state[6], h = state[7];

    for (int i = 0; i < 64; ++i) {
        const uint32_t S1 = ror32(e, 6) ^ ror32(e, 11) ^ ror32(e, 25);
        const uint32_t ch = (e & f) ^ (~e & g);
        const uint32_t t1 = h + S1 + ch + K256[i] + w[i];
        const uint32_t S0 = ror32(a, 2) ^ ror32(a, 13) ^ ror32(a, 22);
        const uint32_t maj = (a & b) ^ (a & c) ^ (b & c);
        const uint32_t t2 = S0 + maj;
        h = g; g = f; f = e; e = d + t1;
        d = c; c = b; b = a; a = t1 + t2;
    }

    state[0] += a; state[1] += b; state[2] += c; state[3] += d;
    state[4] += e; state[5] += f; state[6] += g; state[7] += h;
}

}  // namespace

Sha256::Sha256()
    : m_state{0x6a09e667u, 0xbb67ae85u, 0x3c6ef372u, 0xa54ff53au,
              0x510e527fu, 0x9b05688cu, 0x1f83d9abu, 0x5be0cd19u}
{
    std::memset(m_buffer, 0, sizeof(m_buffer));
}

void Sha256::update(const unsigned char* data, size_t len)
{
    m_bits += static_cast<uint64_t>(len) * 8;
    while (len > 0) {
        const size_t room = 64 - m_bufferLen;
        const size_t take = room < len ? room : len;
        std::memcpy(m_buffer + m_bufferLen, data, take);
        m_bufferLen += take;
        data += take;
        len -= take;
        if (m_bufferLen == 64) {
            sha256Block(m_state, m_buffer);
            m_bufferLen = 0;
        }
    }
}

std::string Sha256::hexDigest()
{
    // Pad: 0x80, zeros to 56 mod 64, then the message length in bits as a
    // big-endian u64. Done directly on the buffer rather than through update(),
    // which would count the padding as message content.
    const uint64_t bits = m_bits;

    m_buffer[m_bufferLen++] = 0x80;
    if (m_bufferLen > 56) {
        std::memset(m_buffer + m_bufferLen, 0, 64 - m_bufferLen);
        sha256Block(m_state, m_buffer);
        m_bufferLen = 0;
    }
    std::memset(m_buffer + m_bufferLen, 0, 56 - m_bufferLen);
    for (int i = 0; i < 8; ++i) {
        m_buffer[56 + i] = static_cast<unsigned char>((bits >> ((7 - i) * 8)) & 0xff);
    }
    sha256Block(m_state, m_buffer);
    m_bufferLen = 0;

    static const char* hex = "0123456789abcdef";
    std::string out;
    out.reserve(64);
    for (int i = 0; i < 8; ++i) {
        for (int shift = 24; shift >= 0; shift -= 8) {
            const unsigned char byte = static_cast<unsigned char>((m_state[i] >> shift) & 0xff);
            out.push_back(hex[byte >> 4]);
            out.push_back(hex[byte & 0x0f]);
        }
    }
    return out;
}

}  // namespace shdkit
