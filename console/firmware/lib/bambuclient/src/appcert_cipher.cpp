#include "bambuclient/appcert.hpp"

#include <mbedtls/pk.h>
#include <mbedtls/rsa.h>
#include <mbedtls/platform_util.h>

#include <cstdint>
#include <cstring>
#include <memory>
#include <string>

namespace bambuclient {

namespace {

// Public key of CN=service.bambulab.com (server wrap key).
// Wraps the ephemeral AES-GCM session key of the fetch request with RSA-PKCS#1 v1.5.
constexpr char kServerWrapKeyPem[] = R"(-----BEGIN PUBLIC KEY-----
MIIBIjANBgkqhkiG9w0BAQEFAAOCAQ8AMIIBCgKCAQEAnbPBC80CMD2VB7Tqj9W/
olQscufbk0rvTqbINriL2WZvfCoBE1WLppJZ/E7cLP02SULh1gB0VuBwRRe4khwk
EFl2A87YaGdp4JRVbP+5xbYYJ09oECRdk0Mrnpo+p9jCW0iw0lxQi+rQ6ZsWwj3S
XzMpNDN3z/Mlx+byU4GkBt6vBsR4cJB8PFRak3KsC4YmWQNybGtoCPMFdCMDIxP6
Qr4o86qEkgK7lJc/ztYaXPDyx+t0uXsna+G1enNELru+2/q4Ppqqrl/Y5pHEdFTK
p4Nj9p2JYT1B5v1qaRExxCJQnQhAlnjANIw2ll7SC5cuSXks909Tbj0G7zofl4N/
kQIDAQAB
-----END PUBLIC KEY-----
)";

// Custom AES S-box (appcert_cipher). Deliberately NOT the standard AES S-box
// (SBOX[0] = 0xC5, standard = 0x63).
constexpr unsigned char kSbox[256] = {
    0xC5, 0x57, 0x4D, 0x6C, 0x3A, 0x95, 0x05, 0xE0, 0xA3, 0xBA, 0x36, 0x1F, 0xEA, 0x51, 0x53, 0x3B,
    0x0E, 0x07, 0x4E, 0x64, 0x50, 0x04, 0x40, 0xE8, 0x62, 0x6E, 0x9F, 0x2D, 0x70, 0x8B, 0x28, 0x49,
    0xD5, 0xF9, 0x65, 0x8D, 0x74, 0x68, 0x7C, 0x6F, 0x0A, 0x6A, 0xB3, 0xAF, 0x38, 0xFE, 0x7E, 0x8A,
    0x47, 0x7F, 0xB0, 0x16, 0x00, 0xD4, 0x0F, 0x13, 0xC9, 0x80, 0x4A, 0xAC, 0x8C, 0x4F, 0xA7, 0x98,
    0x83, 0x94, 0x5D, 0x48, 0xB4, 0xE9, 0x30, 0x19, 0x03, 0x99, 0x25, 0xBF, 0x8E, 0x41, 0xA0, 0xE4,
    0xC3, 0xCF, 0x2C, 0xAB, 0xD2, 0x32, 0x1A, 0x0C, 0x11, 0xB5, 0x56, 0x63, 0x15, 0xA6, 0x69, 0x0B,
    0x88, 0xBB, 0x4C, 0x10, 0xCB, 0x75, 0xFA, 0x81, 0xF8, 0xCD, 0xA1, 0xD6, 0x97, 0xB7, 0x26, 0xC6,
    0x9E, 0xF1, 0x5F, 0xE5, 0xA9, 0x87, 0xC7, 0xDC, 0x8F, 0x7A, 0x86, 0x20, 0x9A, 0xD1, 0x08, 0xC2,
    0x84, 0x09, 0x33, 0x1B, 0xDD, 0x1E, 0xFD, 0x01, 0x71, 0xDA, 0x77, 0x0D, 0xD7, 0xDE, 0x93, 0xCA,
    0xA5, 0xD0, 0xE6, 0x60, 0x89, 0x37, 0xC8, 0x21, 0x59, 0x79, 0x96, 0xAD, 0x24, 0x34, 0xB9, 0x44,
    0xFC, 0xC1, 0xAE, 0xF3, 0x82, 0x46, 0x43, 0x31, 0xE3, 0x2E, 0x4B, 0xFB, 0x92, 0x55, 0xED, 0x45,
    0x76, 0x6D, 0xAA, 0x3F, 0xF5, 0x5A, 0x91, 0x78, 0x22, 0x06, 0xFF, 0xD9, 0x35, 0x7D, 0x7B, 0xDB,
    0x54, 0x12, 0x9C, 0xD8, 0xD3, 0xEE, 0x17, 0x42, 0x52, 0x3E, 0xA4, 0xE7, 0xDF, 0x9D, 0xF2, 0xF4,
    0xEF, 0x73, 0xF6, 0x5E, 0xB1, 0x5B, 0x18, 0xE2, 0x9B, 0x58, 0xA8, 0x2A, 0xE1, 0x3D, 0x90, 0xB6,
    0x1C, 0xBD, 0x61, 0xEB, 0x23, 0xA2, 0x67, 0x39, 0xF0, 0xBC, 0xB2, 0xF7, 0x85, 0x27, 0x72, 0xCC,
    0x29, 0xB8, 0x1D, 0xBE, 0x66, 0xC4, 0x2F, 0xCE, 0x14, 0x3C, 0x6B, 0xEC, 0x5C, 0x2B, 0xC0, 0x02,
};

// Round constants for the key schedule (RCON[0..13]).
constexpr unsigned char kRcon[14] = {
    0x01, 0x02, 0x04, 0x08, 0x10, 0x20, 0x40, 0x80, 0x1B, 0x36, 0x6C, 0xD8, 0xAB, 0x4D,
};

// Fixed AES-256 key of the appcert cipher: 00 01 02 ... 1f.
constexpr unsigned char kAppcertKey[32] = {
    0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07,
    0x08, 0x09, 0x0A, 0x0B, 0x0C, 0x0D, 0x0E, 0x0F,
    0x10, 0x11, 0x12, 0x13, 0x14, 0x15, 0x16, 0x17,
    0x18, 0x19, 0x1A, 0x1B, 0x1C, 0x1D, 0x1E, 0x1F,
};

struct ExpandedKey {
    unsigned char rk[15][16];
};

unsigned char xtime(unsigned char a)
{
    return static_cast<unsigned char>((a << 1) ^ ((a & 0x80) ? 0x1B : 0));
}

unsigned char gmul(unsigned char a, unsigned char b)
{
    unsigned char r = 0;
    while (b) {
        if (b & 1) r = static_cast<unsigned char>(r ^ a);
        a = xtime(a);
        b = static_cast<unsigned char>(b >> 1);
    }
    return r;
}

ExpandedKey key_expand(const unsigned char key[32])
{
    unsigned char w[60][4];
    for (int i = 0; i < 8; ++i)
        for (int j = 0; j < 4; ++j)
            w[i][j] = key[4 * i + j];
    for (int i = 8; i < 60; ++i) {
        unsigned char t[4] = {w[i - 1][0], w[i - 1][1], w[i - 1][2], w[i - 1][3]};
        if (i % 8 == 0) {
            const unsigned char tmp = t[0];
            t[0] = static_cast<unsigned char>(kSbox[t[1]] ^ kRcon[i / 8 - 1]);
            t[1] = kSbox[t[2]];
            t[2] = kSbox[t[3]];
            t[3] = kSbox[tmp];
        } else if (i % 8 == 4) {
            for (int k = 0; k < 4; ++k) t[k] = kSbox[t[k]];
        }
        for (int j = 0; j < 4; ++j)
            w[i][j] = static_cast<unsigned char>(w[i - 8][j] ^ t[j]);
    }
    ExpandedKey ek{};
    for (int r = 0; r < 15; ++r)
        for (int c = 0; c < 4; ++c)
            for (int j = 0; j < 4; ++j)
                ek.rk[r][4 * c + j] = w[4 * r + c][j];
    return ek;
}

void aes_encrypt_block(const unsigned char in[16], const ExpandedKey& ek,
                       unsigned char out[16])
{
    unsigned char s[16];
    for (int i = 0; i < 16; ++i)
        s[i] = static_cast<unsigned char>(in[i] ^ ek.rk[0][i]);
    for (int rnd = 1; rnd < 15; ++rnd) {
        for (int i = 0; i < 16; ++i) s[i] = kSbox[s[i]];
        unsigned char t[16];
        for (int r = 0; r < 4; ++r)
            for (int c = 0; c < 4; ++c)
                t[4 * c + r] = s[4 * ((c + r) % 4) + r];
        std::memcpy(s, t, 16);
        if (rnd < 14) {
            for (int c = 0; c < 4; ++c) {
                const unsigned char a0 = s[4 * c + 0], a1 = s[4 * c + 1];
                const unsigned char a2 = s[4 * c + 2], a3 = s[4 * c + 3];
                s[4 * c + 0] = static_cast<unsigned char>(gmul(a0, 2) ^ gmul(a1, 3) ^ a2 ^ a3);
                s[4 * c + 1] = static_cast<unsigned char>(a0 ^ gmul(a1, 2) ^ gmul(a2, 3) ^ a3);
                s[4 * c + 2] = static_cast<unsigned char>(a0 ^ a1 ^ gmul(a2, 2) ^ gmul(a3, 3));
                s[4 * c + 3] = static_cast<unsigned char>(gmul(a0, 3) ^ a1 ^ a2 ^ gmul(a3, 2));
            }
        }
        for (int i = 0; i < 16; ++i)
            s[i] = static_cast<unsigned char>(s[i] ^ ek.rk[rnd][i]);
    }
    std::memcpy(out, s, 16);
}

constexpr std::size_t kNonceLen    = 12;
constexpr std::size_t kTagLen      = 16;
constexpr std::size_t kLenFieldLen = 4;
constexpr std::size_t kCtLenOffset = kNonceLen + kTagLen;              // 28
constexpr std::size_t kBlobHdrLen  = kCtLenOffset + kLenFieldLen;      // 32
constexpr std::size_t kPtHdrLen    = 4 + 4 + 4 + 5 * 4;                // 32
constexpr std::size_t kLimbCount   = 5;
constexpr std::uint32_t kFirstCtBlockCounter = 2;

constexpr char kMagic[4] = {'\x59', '\x45', '\x4B', '\x53'}; // LE "SKEY"

constexpr char kB64Tbl[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";


} // namespace

const char* server_wrap_key_pem()
{
    return kServerWrapKeyPem;
}

namespace detail {

std::string b64url_encode(const std::string& data)
{
    std::string out;
    out.reserve(((data.size() + 2) / 3) * 4);
    for (std::size_t i = 0; i < data.size(); i += 3) {
        std::uint32_t w = static_cast<std::uint32_t>(
                              static_cast<unsigned char>(data[i])) << 16;
        if (i + 1 < data.size())
            w |= static_cast<std::uint32_t>(
                     static_cast<unsigned char>(data[i + 1])) << 8;
        if (i + 2 < data.size())
            w |= static_cast<unsigned char>(data[i + 2]);
        const char c0 = kB64Tbl[(w >> 18) & 63];
        const char c1 = kB64Tbl[(w >> 12) & 63];
        out.push_back(c0 == '+' ? '-' : c0 == '/' ? '_' : c0);
        out.push_back(c1 == '+' ? '-' : c1 == '/' ? '_' : c1);
        if (i + 1 < data.size()) {
            const char c2 = kB64Tbl[(w >> 6) & 63];
            out.push_back(c2 == '+' ? '-' : c2 == '/' ? '_' : c2);
        } else {
            out.push_back('=');
        }
        if (i + 2 < data.size()) {
            const char c3 = kB64Tbl[w & 63];
            out.push_back(c3 == '+' ? '-' : c3 == '/' ? '_' : c3);
        } else {
            out.push_back('=');
        }
    }
    return out;
}

bool b64_decode_any(const std::string& in, std::string* out, std::string* err)
{
    auto val = [](char c) -> int {
        if (c >= 'A' && c <= 'Z') return c - 'A';
        if (c >= 'a' && c <= 'z') return c - 'a' + 26;
        if (c >= '0' && c <= '9') return c - '0' + 52;
        if (c == '+' || c == '-') return 62;
        if (c == '/' || c == '_') return 63;
        return -1;
    };
    std::string s;
    s.reserve(in.size());
    for (char c : in) {
        if (c == ' ' || c == '\t' || c == '\r' || c == '\n') continue;
        s.push_back(c == '-' ? '+' : c == '_' ? '/' : c);
    }
    while (s.size() % 4 != 0) s.push_back('=');
    std::string res;
    res.reserve(s.size() / 4 * 3);
    for (std::size_t i = 0; i < s.size(); i += 4) {
        int q[4];
        for (int j = 0; j < 4; ++j) {
            if (s[i + j] == '=') { q[j] = -2; continue; }
            q[j] = val(s[i + j]);
            if (q[j] < 0) {
                if (err) *err = "invalid base64 character";
                return false;
            }
        }
        if (q[0] < 0 || q[1] < 0) {
            if (err) *err = "truncated base64";
            return false;
        }
        const std::uint32_t w =
            (static_cast<std::uint32_t>(q[0]) << 18) |
            (static_cast<std::uint32_t>(q[1]) << 12) |
            (static_cast<std::uint32_t>(q[2] >= 0 ? q[2] : 0) << 6) |
            static_cast<std::uint32_t>(q[3] >= 0 ? q[3] : 0);
        res.push_back(static_cast<char>((w >> 16) & 0xFF));
        if (q[2] >= 0) res.push_back(static_cast<char>((w >> 8) & 0xFF));
        if (q[3] >= 0) res.push_back(static_cast<char>(w & 0xFF));
    }
    if (out) *out = std::move(res);
    return true;
}

std::string ctr_xor(const unsigned char key[32], const std::string& nonce,
                    const std::string& data)
{
    if (nonce.size() != kNonceLen) return {};
    const ExpandedKey ek = key_expand(key);
    std::string out(data.size(), '\0');
    const std::size_t blocks = (data.size() + 15) / 16;
    for (std::size_t i = 0; i < blocks; ++i) {
        unsigned char blk[16];
        std::memcpy(blk, nonce.data(), kNonceLen);
        const std::uint32_t ctr =
            kFirstCtBlockCounter + static_cast<std::uint32_t>(i);
        blk[12] = static_cast<unsigned char>(ctr >> 24);
        blk[13] = static_cast<unsigned char>(ctr >> 16);
        blk[14] = static_cast<unsigned char>(ctr >> 8);
        blk[15] = static_cast<unsigned char>(ctr);
        unsigned char ks[16];
        aes_encrypt_block(blk, ek, ks);
        const std::size_t base = i * 16;
        const std::size_t n = data.size() - base < 16 ? data.size() - base : 16;
        for (std::size_t j = 0; j < n; ++j)
            out[base + j] = static_cast<char>(
                static_cast<unsigned char>(data[base + j]) ^ ks[j]);
    }
    return out;
}

bool decode_key_blob(const std::string& blob_b64,
                     std::array<std::string, 5>* limbs, std::string* err)
{
    std::string blob;
    if (!b64_decode_any(blob_b64, &blob, err)) return false;
    if (blob.size() < kBlobHdrLen + kPtHdrLen + kLimbCount) {
        if (err) *err = "key blob too short: " + std::to_string(blob.size()) + " bytes";
        return false;
    }
    std::uint32_t ct_len = 0;
    for (int i = 3; i >= 0; --i)
        ct_len = (ct_len << 8) |
                 static_cast<unsigned char>(blob[kCtLenOffset + i]); // u32le
    if (kBlobHdrLen + ct_len > blob.size())
        ct_len = static_cast<std::uint32_t>(blob.size() - kBlobHdrLen);
    const std::string pt = ctr_xor(
        kAppcertKey, blob.substr(0, kNonceLen),
        blob.substr(kBlobHdrLen, ct_len));
    if (pt.size() < kPtHdrLen + kLimbCount ||
        std::memcmp(pt.data(), kMagic, 4) != 0) {
        if (err) *err = "SKEY magic missing (wrong cipher / framing)";
        return false;
    }
    const std::size_t body = pt.size() - kPtHdrLen;
    if (body == 0 || body % kLimbCount != 0) {
        if (err)
            *err = "SKEY plaintext is not 5 equal CRT limbs: " +
                   std::to_string(pt.size()) + " bytes";
        return false;
    }
    const std::size_t limb = body / kLimbCount;
    for (std::size_t i = 0; i < kLimbCount; ++i)
        (*limbs)[i] = pt.substr(kPtHdrLen + i * limb, limb);
    return true;
}

std::string rsa_pem_from_crt(const std::array<std::string, 5>& limbs,
                             std::string* err)
{
    auto fail = [err](const std::string& e) -> std::string {
        if (err) *err = e;
        return {};
    };
    for (const auto& l : limbs)
        if (l.empty()) return fail("empty CRT limb");

    // Only p and q are needed: mbedtls_rsa_complete() derives n, d (mod
    // lcm(p-1,q-1), same as the OpenSSL original), dp, dq and qinv itself,
    // so the shipped dp/dq/qinv limbs are not needed. The caller checks the
    // resulting modulus against the leaf certificate.
    static const unsigned char kE[3] = {0x01, 0x00, 0x01};   // RSA_F4
    const auto* p = reinterpret_cast<const unsigned char*>(limbs[0].data());
    const auto* q = reinterpret_cast<const unsigned char*>(limbs[1].data());

    mbedtls_pk_context pk;
    mbedtls_pk_init(&pk);
    std::string pem;
    do {
        int rc = mbedtls_pk_setup(&pk, mbedtls_pk_info_from_type(MBEDTLS_PK_RSA));
        if (rc != 0) { pem = fail("mbedtls_pk_setup failed"); break; }
        mbedtls_rsa_context* rsa = mbedtls_pk_rsa(pk);
        rc = mbedtls_rsa_import_raw(rsa, nullptr, 0, p, limbs[0].size(),
                                    q, limbs[1].size(), nullptr, 0, kE, sizeof(kE));
        if (rc != 0) { pem = fail("RSA import failed"); break; }
        rc = mbedtls_rsa_complete(rsa);
        if (rc != 0) { pem = fail("RSA complete failed (degenerate limbs)"); break; }
        if (mbedtls_rsa_check_privkey(rsa) != 0) { pem = fail("RSA key fails consistency check"); break; }

        std::string buf(4096, '\0');
        rc = mbedtls_pk_write_key_pem(&pk, reinterpret_cast<unsigned char*>(&buf[0]), buf.size());
        if (rc != 0) { pem = fail("PEM write failed"); break; }
        buf.resize(std::strlen(buf.c_str()));
        pem = std::move(buf);
    } while (false);
    mbedtls_pk_free(&pk);
    return pem;
}

} // namespace detail
} // namespace bambuclient
