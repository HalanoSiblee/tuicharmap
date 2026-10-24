#include "unicode.hpp"
#include <unicode/uchar.h>
#include <algorithm>
#include <atomic>
#include <cctype>
#include <cstring>
#include <cwchar>
#include <mutex>
#include <unordered_map>
#include <vector>
#include <fstream> // For Windows fallback

#ifndef _WIN32
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace uc {

namespace {

constexpr size_t MAX_CP = 0x110000;
constexpr size_t MAX_NAME_BYTES = 16 * 1024 * 1024;
constexpr const char* CACHE_PATH = "/tmp/tuicharmap.cache";
constexpr uint32_t CACHE_VERSION = 3; // Bumped version for cross-platform sync

struct CacheHeader {
    uint32_t magic;
    uint32_t version;
    uint32_t count;
    uint32_t name_bytes;
    uint32_t reserved[4];
};

CacheHeader* hdr = nullptr;
uint32_t*    m_index = nullptr;
uint8_t*     m_widths = nullptr;
uint32_t*    m_offsets = nullptr;
char*        m_names = nullptr;

// Windows memory buffer for cache
std::vector<uint8_t> win_buf;

std::vector<uint32_t>    f_index;
std::vector<uint8_t>     f_widths;
std::vector<uint32_t>    f_offsets;
std::string              f_names;
std::vector<std::string> f_strings;

std::atomic<size_t> g_count{0};
std::atomic<bool>   g_done{false};
std::once_flag      g_build_once;

std::unordered_map<uint32_t, std::string> g_cache;
std::mutex                                g_cache_mtx;

std::string upper_copy(const char* p, size_t n) {
    std::string s(p, n);
    for (auto& c : s) c = (char)std::toupper((unsigned char)c);
    return s;
}

void build_or_load() {
    bool mapped = false;

#ifndef _WIN32
    int fd = open(CACHE_PATH, O_RDWR | O_CREAT, 0600);
    if (fd >= 0) {
        size_t total_size = sizeof(CacheHeader) + MAX_CP * 9 + MAX_NAME_BYTES;
        struct stat st;
        if (fstat(fd, &st) == 0 && (size_t)st.st_size < total_size) {
            if (ftruncate(fd, total_size) != 0) { /* handle error */ }
        }

        void* base = mmap(nullptr, total_size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
        if (base != MAP_FAILED) {
            hdr = (CacheHeader*)base;
            m_index = (uint32_t*)((char*)base + sizeof(CacheHeader));
            m_widths = (uint8_t*)(m_index + MAX_CP);
            m_offsets = (uint32_t*)(m_widths + MAX_CP);
            m_names = (char*)(m_offsets + MAX_CP);
            mapped = true;

            if (hdr->magic == 0x54554943 && hdr->version == CACHE_VERSION) {
                g_count.store(hdr->count, std::memory_order_release);
                g_done.store(true, std::memory_order_release);
                close(fd);
                return;
            }
        }
        close(fd);
    }
#else
    // Windows: read cache into memory if valid
    std::ifstream ifs(CACHE_PATH, std::ios::binary);
    if (ifs) {
        CacheHeader local_hdr;
        ifs.read((char*)&local_hdr, sizeof(local_hdr));
        if (local_hdr.magic == 0x54554943 && local_hdr.version == CACHE_VERSION) {
            ifs.seekg(0, std::ios::beg);
            win_buf.resize(sizeof(CacheHeader) + MAX_CP * 9 + local_hdr.name_bytes);
            ifs.read((char*)win_buf.data(), win_buf.size());
            
            hdr = (CacheHeader*)win_buf.data();
            m_index = (uint32_t*)((char*)win_buf.data() + sizeof(CacheHeader));
            m_widths = (uint8_t*)(m_index + MAX_CP);
            m_offsets = (uint32_t*)(m_widths + MAX_CP);
            m_names = (char*)(m_offsets + MAX_CP);
            
            g_count.store(hdr->count, std::memory_order_release);
            g_done.store(true, std::memory_order_release);
            return;
        }
    }
    // Prepare buffer for building
    win_buf.resize(sizeof(CacheHeader) + MAX_CP * 9 + MAX_NAME_BYTES);
    hdr = (CacheHeader*)win_buf.data();
    m_index = (uint32_t*)((char*)win_buf.data() + sizeof(CacheHeader));
    m_widths = (uint8_t*)(m_index + MAX_CP);
    m_offsets = (uint32_t*)(m_widths + MAX_CP);
    m_names = (char*)(m_offsets + MAX_CP);
#endif

    if (!mapped) {
        if (!hdr) { // Fallback to heap vectors if mmap completely failed
            f_index.resize(MAX_CP);
            f_widths.resize(MAX_CP);
            f_offsets.resize(MAX_CP);
            f_names.reserve(MAX_NAME_BYTES);
            f_strings.resize(MAX_CP);
            
            m_index = f_index.data();
            m_widths = f_widths.data();
            m_offsets = f_offsets.data();
        } else {
            hdr->magic = 0;
        }
    }

    size_t name_bytes = 0;
    char buf[256];
    size_t total_count = 0;
    
    for (uint32_t cp = 0x20; cp <= 0x10FFFF; ++cp) {
        if (cp >= 0xD800 && cp <= 0xDFFF) { cp = 0xDFFF; continue; }
        if (cp == 0xFEFF) continue;
        if (cp >= 0xFFF0 && cp <= 0xFFFF) continue;
        
        int8_t cat = u_charType(cp);
        if (cat == U_UNASSIGNED) continue;
        if (cat != U_PRIVATE_USE_CHAR && !u_isprint(cp)) continue;

        int w = wcwidth((wchar_t)cp);
        if (w <= 0) continue;

        UErrorCode err = U_ZERO_ERROR;
        int32_t n = u_charName(cp, U_UNICODE_CHAR_NAME, buf, sizeof(buf), &err);
        if (U_FAILURE(err) || n <= 0) continue;

        std::string uname = upper_copy(buf, (size_t)n);
        if (name_bytes + uname.size() + 1 > MAX_NAME_BYTES) continue;

        m_index[total_count] = cp;
        m_widths[total_count] = (uint8_t)w;
        m_offsets[total_count] = (uint32_t)name_bytes;
        
        if (hdr) {
            std::memcpy(m_names + name_bytes, uname.c_str(), uname.size() + 1);
        } else {
            f_names.append(uname.c_str(), uname.size() + 1);
            f_strings[total_count] = uname;
        }
        
        name_bytes += uname.size() + 1;
        total_count++;

        if (total_count % 1000 == 0) {
            g_count.store(total_count, std::memory_order_release);
        }
    }

    g_count.store(total_count, std::memory_order_release);

    if (hdr) {
        hdr->count = total_count;
        hdr->name_bytes = (uint32_t)name_bytes;
        hdr->version = CACHE_VERSION;
        hdr->magic = 0x54554943;

#ifndef _WIN32
        msync(hdr, sizeof(CacheHeader), MS_SYNC);
#else
        std::ofstream ofs(CACHE_PATH, std::ios::binary);
        if (ofs) {
            ofs.write((char*)win_buf.data(), sizeof(CacheHeader) + MAX_CP * 9 + name_bytes);
        }
#endif
    }
    g_done.store(true, std::memory_order_release);
}

} // namespace

std::string name(uint32_t cp) {
    {
        std::lock_guard<std::mutex> lk(g_cache_mtx);
        auto it = g_cache.find(cp);
        if (it != g_cache.end()) return it->second;
    }
    char buf[256];
    UErrorCode err = U_ZERO_ERROR;
    int32_t n = u_charName(cp, U_UNICODE_CHAR_NAME, buf, sizeof(buf), &err);
    std::string out = (U_SUCCESS(err) && n > 0) ? std::string(buf, n) : "<unnamed>";
    {
        std::lock_guard<std::mutex> lk(g_cache_mtx);
        g_cache.emplace(cp, out);
    }
    return out;
}

bool is_printable(uint32_t cp) { return u_isprint(cp); }

void warmup() { std::call_once(g_build_once, build_or_load); }

bool ready() { return g_done.load(std::memory_order_acquire); }
size_t count() { return g_count.load(std::memory_order_acquire); }

uint32_t cp_at(size_t i) {
    return (i < count()) ? m_index[i] : 0;
}

uint8_t width_at(size_t i) {
    return (i < count()) ? m_widths[i] : 1;
}

std::string_view upper_name(size_t i) {
    if (i >= count()) return {};
    if (hdr || m_names) {
        return std::string_view(m_names + m_offsets[i]);
    } else {
        return std::string_view(f_strings[i]);
    }
}

size_t find(uint32_t cp) {
    size_t n = count();
    auto it = std::lower_bound(m_index, m_index + n, cp);
    if (it != m_index + n && *it == cp) return (size_t)(it - m_index);
    return SIZE_MAX;
}

} // namespace uc