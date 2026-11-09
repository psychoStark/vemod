#include <zlib.h>
#include <mutex>
#include <unordered_map>

#include "nb.h"

namespace nb {

namespace {

#pragma pack(push, 4)
struct z_stream_32 {
    u32 next_in;
    u32 avail_in;
    u32 total_in;

    u32 next_out;
    u32 avail_out;
    u32 total_out;

    u32 msg;
    u32 state;

    u32 zalloc;
    u32 zfree;
    u32 opaque;

    s32 data_type;
    u32 adler;
    u32 reserved;
};
#pragma pack(pop)

static_assert(sizeof(z_stream_32) == 56, "z_stream_32 must be 56 bytes");

std::mutex g_zlib_mu;
std::unordered_map<gaddr, z_stream*> g_zstreams;

z_stream* get_or_create_stream(gaddr strm_ptr) {
    auto it = g_zstreams.find(strm_ptr);
    if (it != g_zstreams.end()) return it->second;
    auto* s = new z_stream();
    memset(s, 0, sizeof(z_stream));
    g_zstreams[strm_ptr] = s;
    return s;
}

void sync_to_host(gaddr strm_ptr, z_stream* host_strm) {
    auto* g = hp<z_stream_32>(strm_ptr);
    host_strm->next_in = g->next_in ? static_cast<Bytef*>(h(g->next_in)) : nullptr;
    host_strm->avail_in = g->avail_in;
    host_strm->total_in = g->total_in;

    host_strm->next_out = g->next_out ? static_cast<Bytef*>(h(g->next_out)) : nullptr;
    host_strm->avail_out = g->avail_out;
    host_strm->total_out = g->total_out;

    host_strm->data_type = g->data_type;
    host_strm->adler = g->adler;
}

void sync_to_guest(z_stream* host_strm, gaddr strm_ptr) {
    auto* g_strm = hp<z_stream_32>(strm_ptr);
    g_strm->next_in = host_strm->next_in ? g(host_strm->next_in) : 0;
    g_strm->avail_in = host_strm->avail_in;
    g_strm->total_in = static_cast<u32>(host_strm->total_in);

    g_strm->next_out = host_strm->next_out ? g(host_strm->next_out) : 0;
    g_strm->avail_out = host_strm->avail_out;
    g_strm->total_out = static_cast<u32>(host_strm->total_out);

    g_strm->data_type = host_strm->data_type;
    g_strm->adler = static_cast<u32>(host_strm->adler);
}

int x_inflateInit_(gaddr strm_ptr, const char* version, int stream_size) {
    std::lock_guard lk(g_zlib_mu);
    z_stream* s = get_or_create_stream(strm_ptr);
    sync_to_host(strm_ptr, s);
    s->zalloc = Z_NULL;
    s->zfree = Z_NULL;
    s->opaque = Z_NULL;
    int ret = inflateInit_(s, version ? version : ZLIB_VERSION, sizeof(z_stream));
    sync_to_guest(s, strm_ptr);
    return ret;
}

int x_inflateInit2_(gaddr strm_ptr, int windowBits, const char* version, int stream_size) {
    std::lock_guard lk(g_zlib_mu);
    z_stream* s = get_or_create_stream(strm_ptr);
    sync_to_host(strm_ptr, s);
    s->zalloc = Z_NULL;
    s->zfree = Z_NULL;
    s->opaque = Z_NULL;
    int ret = inflateInit2_(s, windowBits, version ? version : ZLIB_VERSION, sizeof(z_stream));
    sync_to_guest(s, strm_ptr);
    return ret;
}

int x_inflate(gaddr strm_ptr, int flush) {
    std::lock_guard lk(g_zlib_mu);
    auto it = g_zstreams.find(strm_ptr);
    if (it == g_zstreams.end()) return Z_STREAM_ERROR;
    z_stream* s = it->second;
    sync_to_host(strm_ptr, s);
    int ret = inflate(s, flush);
    sync_to_guest(s, strm_ptr);
    return ret;
}

int x_inflateEnd(gaddr strm_ptr) {
    std::lock_guard lk(g_zlib_mu);
    auto it = g_zstreams.find(strm_ptr);
    if (it == g_zstreams.end()) return Z_STREAM_ERROR;
    z_stream* s = it->second;
    int ret = inflateEnd(s);
    delete s;
    g_zstreams.erase(it);
    return ret;
}

int x_inflateReset(gaddr strm_ptr) {
    std::lock_guard lk(g_zlib_mu);
    auto it = g_zstreams.find(strm_ptr);
    if (it == g_zstreams.end()) return Z_STREAM_ERROR;
    return inflateReset(it->second);
}

int x_deflateInit_(gaddr strm_ptr, int level, const char* version, int stream_size) {
    std::lock_guard lk(g_zlib_mu);
    z_stream* s = get_or_create_stream(strm_ptr);
    sync_to_host(strm_ptr, s);
    s->zalloc = Z_NULL;
    s->zfree = Z_NULL;
    s->opaque = Z_NULL;
    int ret = deflateInit_(s, level, version ? version : ZLIB_VERSION, sizeof(z_stream));
    sync_to_guest(s, strm_ptr);
    return ret;
}

int x_deflateInit2_(gaddr strm_ptr, int level, int method, int windowBits, int memLevel, int strategy, const char* version, int stream_size) {
    std::lock_guard lk(g_zlib_mu);
    z_stream* s = get_or_create_stream(strm_ptr);
    sync_to_host(strm_ptr, s);
    s->zalloc = Z_NULL;
    s->zfree = Z_NULL;
    s->opaque = Z_NULL;
    int ret = deflateInit2_(s, level, method, windowBits, memLevel, strategy, version ? version : ZLIB_VERSION, sizeof(z_stream));
    sync_to_guest(s, strm_ptr);
    return ret;
}

int x_deflate(gaddr strm_ptr, int flush) {
    std::lock_guard lk(g_zlib_mu);
    auto it = g_zstreams.find(strm_ptr);
    if (it == g_zstreams.end()) return Z_STREAM_ERROR;
    z_stream* s = it->second;
    sync_to_host(strm_ptr, s);
    int ret = deflate(s, flush);
    sync_to_guest(s, strm_ptr);
    return ret;
}

int x_deflateEnd(gaddr strm_ptr) {
    std::lock_guard lk(g_zlib_mu);
    auto it = g_zstreams.find(strm_ptr);
    if (it == g_zstreams.end()) return Z_STREAM_ERROR;
    z_stream* s = it->second;
    int ret = deflateEnd(s);
    delete s;
    g_zstreams.erase(it);
    return ret;
}

int x_deflateReset(gaddr strm_ptr) {
    std::lock_guard lk(g_zlib_mu);
    auto it = g_zstreams.find(strm_ptr);
    if (it == g_zstreams.end()) return Z_STREAM_ERROR;
    return deflateReset(it->second);
}

u32 x_crc32(u32 crc, const void* buf, u32 len) {
    return static_cast<u32>(crc32(crc, static_cast<const Bytef*>(buf), len));
}

u32 x_adler32(u32 adler, const void* buf, u32 len) {
    return static_cast<u32>(adler32(adler, static_cast<const Bytef*>(buf), len));
}

gaddr s_zlib_ver_str = 0;
gaddr x_zlibVersion() {
    if (!s_zlib_ver_str) {
        s_zlib_ver_str = gstrdup(ZLIB_VERSION);
    }
    return s_zlib_ver_str;
}

}  // namespace

void register_zlib() {
    NB_REG("inflateInit_", x_inflateInit_);
    NB_REG("inflateInit2_", x_inflateInit2_);
    NB_REG("inflate", x_inflate);
    NB_REG("inflateEnd", x_inflateEnd);
    NB_REG("inflateReset", x_inflateReset);

    NB_REG("deflateInit_", x_deflateInit_);
    NB_REG("deflateInit2_", x_deflateInit2_);
    NB_REG("deflate", x_deflate);
    NB_REG("deflateEnd", x_deflateEnd);
    NB_REG("deflateReset", x_deflateReset);

    NB_REG("crc32", x_crc32);
    NB_REG("adler32", x_adler32);
    NB_REG("zlibVersion", x_zlibVersion);
}

}  // namespace nb