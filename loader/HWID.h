#pragma once
#include <Windows.h>
#include <iphlpapi.h>
#include <intrin.h>
#include <string>
#include <sstream>
#include <iomanip>
#include <cstdlib>
#include <cstring>

#pragma comment(lib, "iphlpapi.lib")

namespace sha256x {

struct SHA256_CTX {
    uint32_t state[8];
    uint64_t bitlen;
    uint8_t data[64];
    uint32_t datalen;
};

static const uint32_t k[64] = {
    0x428a2f98,0x71374491,0xb5c0fbcf,0xe9b5dba5,0x3956c25b,0x59f111f1,0x923f82a4,0xab1c5ed5,
    0xd807aa98,0x12835b01,0x243185be,0x550c7dc3,0x72be5d74,0x80deb1fe,0x9bdc06a7,0xc19bf174,
    0xe49b69c1,0xefbe4786,0x0fc19dc6,0x240ca1cc,0x2de92c6f,0x4a7484aa,0x5cb0a9dc,0x76f988da,
    0x983e5152,0xa831c66d,0xb00327c8,0xbf597fc7,0xc6e00bf3,0xd5a79147,0x06ca6351,0x14292967,
    0x27b70a85,0x2e1b2138,0x4d2c6dfc,0x53380d13,0x650a7354,0x766a0abb,0x81c2c92e,0x92722c85,
    0xa2bfe8a1,0xa81a664b,0xc24b8b70,0xc76c51a3,0xd192e819,0xd6990624,0xf40e3585,0x106aa070,
    0x19a4c116,0x1e376c08,0x2748774c,0x34b0bcb5,0x391c0cb3,0x4ed8aa4a,0x5b9cca4f,0x682e6ff3,
    0x748f82ee,0x78a5636f,0x84c87814,0x8cc70208,0x90befffa,0xa4506ceb,0xbef9a3f7,0xc67178f2
};

inline uint32_t rotr(uint32_t a, uint32_t b) { return (a >> b) | (a << (32 - b)); }

inline void transform(SHA256_CTX* c, const uint8_t* d) {
    uint32_t a, b, cc, dd, e, f, g, h, i, j, t1, t2, m[64];
    for (i = 0, j = 0; i < 16; ++i, j += 4)
        m[i] = (d[j] << 24) | (d[j+1] << 16) | (d[j+2] << 8) | (d[j+3]);
    for (; i < 64; ++i)
        m[i] = (rotr(m[i-2],17) ^ rotr(m[i-2],19) ^ (m[i-2] >> 10)) + m[i-7] +
               (rotr(m[i-15],7) ^ rotr(m[i-15],18) ^ (m[i-15] >> 3)) + m[i-16];
    a=c->state[0]; b=c->state[1]; cc=c->state[2]; dd=c->state[3];
    e=c->state[4]; f=c->state[5]; g=c->state[6]; h=c->state[7];
    for (i = 0; i < 64; ++i) {
        t1 = h + (rotr(e,6) ^ rotr(e,11) ^ rotr(e,25)) + ((e & f) ^ (~e & g)) + k[i] + m[i];
        t2 = (rotr(a,2) ^ rotr(a,13) ^ rotr(a,22)) + ((a & b) ^ (a & cc) ^ (b & cc));
        h = g; g = f; f = e; e = dd + t1;
        dd = cc; cc = b; b = a; a = t1 + t2;
    }
    c->state[0]+=a; c->state[1]+=b; c->state[2]+=cc; c->state[3]+=dd;
    c->state[4]+=e; c->state[5]+=f; c->state[6]+=g; c->state[7]+=h;
}

inline void init(SHA256_CTX* c) {
    c->datalen = 0; c->bitlen = 0;
    c->state[0]=0x6a09e667; c->state[1]=0xbb67ae85;
    c->state[2]=0x3c6ef372; c->state[3]=0xa54ff53a;
    c->state[4]=0x510e527f; c->state[5]=0x9b05688c;
    c->state[6]=0x1f83d9ab; c->state[7]=0x5be0cd19;
}

inline void update(SHA256_CTX* c, const uint8_t* data, size_t len) {
    for (size_t i = 0; i < len; ++i) {
        c->data[c->datalen++] = data[i];
        if (c->datalen == 64) { transform(c, c->data); c->bitlen += 512; c->datalen = 0; }
    }
}

inline void final(SHA256_CTX* c, uint8_t* hash) {
    uint32_t i = c->datalen;
    if (c->datalen < 56) {
        c->data[i++] = 0x80;
        while (i < 56) c->data[i++] = 0x00;
    } else {
        c->data[i++] = 0x80;
        while (i < 64) c->data[i++] = 0x00;
        transform(c, c->data);
        memset(c->data, 0, 56);
    }
    c->bitlen += c->datalen * 8;
    c->data[63] = (uint8_t)(c->bitlen);
    c->data[62] = (uint8_t)(c->bitlen >> 8);
    c->data[61] = (uint8_t)(c->bitlen >> 16);
    c->data[60] = (uint8_t)(c->bitlen >> 24);
    c->data[59] = (uint8_t)(c->bitlen >> 32);
    c->data[58] = (uint8_t)(c->bitlen >> 40);
    c->data[57] = (uint8_t)(c->bitlen >> 48);
    c->data[56] = (uint8_t)(c->bitlen >> 56);
    transform(c, c->data);
    for (i = 0; i < 4; ++i) {
        hash[i]      = (uint8_t)((c->state[0] >> (24 - i * 8)) & 0xff);
        hash[i + 4]  = (uint8_t)((c->state[1] >> (24 - i * 8)) & 0xff);
        hash[i + 8]  = (uint8_t)((c->state[2] >> (24 - i * 8)) & 0xff);
        hash[i + 12] = (uint8_t)((c->state[3] >> (24 - i * 8)) & 0xff);
        hash[i + 16] = (uint8_t)((c->state[4] >> (24 - i * 8)) & 0xff);
        hash[i + 20] = (uint8_t)((c->state[5] >> (24 - i * 8)) & 0xff);
        hash[i + 24] = (uint8_t)((c->state[6] >> (24 - i * 8)) & 0xff);
        hash[i + 28] = (uint8_t)((c->state[7] >> (24 - i * 8)) & 0xff);
    }
}

inline std::string hex(const uint8_t* hash) {
    std::ostringstream oss;
    for (int i = 0; i < 32; i++) oss << std::hex << std::setw(2) << std::setfill('0') << (int)hash[i];
    return oss.str();
}

}

inline std::string computeHwid() {
    std::stringstream src;

    int cpuInfo[4] = {};
    __cpuid(cpuInfo, 0);
    src << std::hex << cpuInfo[1] << cpuInfo[3] << cpuInfo[2];
    __cpuid(cpuInfo, 0x80000002);
    for (int i = 0; i < 4; i++) src << std::hex << cpuInfo[i];
    __cpuid(cpuInfo, 0x80000003);
    for (int i = 0; i < 4; i++) src << std::hex << cpuInfo[i];
    __cpuid(cpuInfo, 0x80000004);
    for (int i = 0; i < 4; i++) src << std::hex << cpuInfo[i];

    DWORD vol = 0;
    GetVolumeInformationA("C:\\", nullptr, 0, &vol, nullptr, nullptr, nullptr, 0);
    src << std::hex << vol;

    PIP_ADAPTER_INFO info = nullptr;
    ULONG size = 0;
    if (GetAdaptersInfo(nullptr, &size) == ERROR_BUFFER_OVERFLOW) {
        info = (PIP_ADAPTER_INFO)malloc(size);
        if (info && GetAdaptersInfo(info, &size) == NO_ERROR) {
            for (auto* p = info; p; p = p->Next) {
                if (p->AddressLength >= 6) {
                    for (UINT i = 0; i < p->AddressLength; i++)
                        src << std::hex << std::setw(2) << std::setfill('0') << (int)p->Address[i];
                    break;
                }
            }
        }
        free(info);
    }

    std::string s = src.str();
    sha256x::SHA256_CTX ctx;
    sha256x::init(&ctx);
    sha256x::update(&ctx, (const uint8_t*)s.data(), s.size());
    uint8_t hash[32];
    sha256x::final(&ctx, hash);
    return sha256x::hex(hash);
}