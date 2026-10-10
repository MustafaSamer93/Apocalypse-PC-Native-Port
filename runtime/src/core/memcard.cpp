#include "core/memcard.h"
#include "core/psx.h"

#include <cstdio>
#include <cstring>

// Directory frame (frames 1..15 of block 0):
//   0x00 u32 state: 0xA0 free, 0x51 first block, 0x52 middle, 0x53 last, 0xA1-0xA3 deleted
//   0x04 u32 file size (first block only)
//   0x08 u16 next block index (0-based data block, 0xFFFF = none)
//   0x0A char name[21]
//   0x7F xor checksum of 0x00..0x7E
namespace {
constexpr uint32_t kFree = 0xA0, kFirst = 0x51, kMiddle = 0x52, kLast = 0x53;

uint32_t rd32(const uint8_t* p) { return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24; }
void wr32(uint8_t* p, uint32_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24); }
}  // namespace

void MemoryCard::fix_checksum(int frame) {
    uint8_t* f = &img_[frame * kFrame];
    uint8_t x = 0;
    for (int i = 0; i < 0x7F; i++) x ^= f[i];
    f[0x7F] = x;
}

void MemoryCard::format() {
    img_.assign(kSize, 0);
    img_[0] = 'M';
    img_[1] = 'C';
    fix_checksum(0);
    for (int b = 1; b <= kBlocks; b++) {
        uint8_t* d = dir(b);
        wr32(d, kFree);
        d[8] = d[9] = 0xFF;
        fix_checksum(b);
    }
    // broken-sector list (frames 16..35): none
    for (int f = 16; f < 36; f++) {
        uint8_t* d = &img_[f * kFrame];
        wr32(d, 0xFFFFFFFF);
        d[8] = d[9] = 0xFF;
        fix_checksum(f);
    }
    // frame 63: write-test frame, copy of frame 0
    std::memcpy(&img_[63 * kFrame], &img_[0], kFrame);
    dirty_ = true;
}

bool MemoryCard::load_or_create(const std::string& path) {
    path_ = path;
    img_.assign(kSize, 0);
    if (FILE* f = std::fopen(path.c_str(), "rb")) {
        size_t n = std::fread(img_.data(), 1, kSize, f);
        std::fclose(f);
        if (n == kSize && img_[0] == 'M' && img_[1] == 'C') return true;
        log_msg(LogCat::Pad, "memory card %s is not a 128 KB .mcr image; reformatting", path.c_str());
    }
    format();
    flush();
    return true;
}

void MemoryCard::flush() {
    if (!dirty_ || path_.empty()) return;
    std::string tmp = path_ + ".tmp";
    FILE* f = std::fopen(tmp.c_str(), "wb");
    if (!f) {
        log_msg(LogCat::Pad, "cannot write memory card %s", path_.c_str());
        return;
    }
    std::fwrite(img_.data(), 1, kSize, f);
    std::fclose(f);
    std::remove(path_.c_str());
    std::rename(tmp.c_str(), path_.c_str());
    dirty_ = false;
}

std::vector<MemoryCard::FileInfo> MemoryCard::list() const {
    std::vector<FileInfo> out;
    for (int b = 1; b <= kBlocks; b++) {
        const uint8_t* d = dir(b);
        if (rd32(d) != kFirst) continue;
        char name[22] = {};
        std::memcpy(name, d + 0x0A, 21);
        out.push_back({name, b, rd32(d + 4)});
    }
    return out;
}

bool MemoryCard::find(const std::string& name, FileInfo* out) const {
    for (auto& f : list()) {
        if (f.name == name) {
            if (out) *out = f;
            return true;
        }
    }
    return false;
}

bool MemoryCard::create(const std::string& name, int blocks, FileInfo* out) {
    if (blocks < 1 || find(name, nullptr)) return false;
    std::vector<int> free;
    for (int b = 1; b <= kBlocks && (int)free.size() < blocks; b++)
        if ((rd32(dir(b)) & 0xF0) == 0xA0) free.push_back(b);
    if ((int)free.size() < blocks) return false;
    for (size_t i = 0; i < free.size(); i++) {
        uint8_t* d = dir(free[i]);
        std::memset(d, 0, kFrame);
        uint32_t state = blocks == 1 ? kFirst : (i == 0 ? kFirst : (i + 1 == free.size() ? kLast : kMiddle));
        wr32(d, state);
        if (i == 0) {
            wr32(d + 4, (uint32_t)blocks * kBlock);
            std::strncpy((char*)d + 0x0A, name.c_str(), 20);
        }
        uint16_t next = i + 1 < free.size() ? (uint16_t)(free[i + 1] - 1) : 0xFFFF;
        d[8] = (uint8_t)next;
        d[9] = (uint8_t)(next >> 8);
        fix_checksum(free[i]);
        std::memset(&img_[free[i] * kBlock], 0, kBlock);
    }
    dirty_ = true;
    if (out) *out = {name, free[0], (uint32_t)blocks * kBlock};
    return true;
}

bool MemoryCard::remove(const std::string& name) {
    FileInfo f;
    if (!find(name, &f)) return false;
    int b = f.first_block;
    for (int guard = 0; b >= 1 && b <= kBlocks && guard < kBlocks; guard++) {
        uint8_t* d = dir(b);
        uint16_t next = d[8] | d[9] << 8;
        wr32(d, (rd32(d) & 0x0F) | 0xA0);
        fix_checksum(b);
        b = next == 0xFFFF ? -1 : next + 1;
    }
    dirty_ = true;
    return true;
}

int MemoryCard::file_offset(const FileInfo& f, uint32_t pos) const {
    int b = f.first_block;
    for (uint32_t skip = pos / kBlock; skip > 0; skip--) {
        const uint8_t* d = dir(b);
        uint16_t next = d[8] | d[9] << 8;
        if (next == 0xFFFF) return -1;
        b = next + 1;
    }
    return b * kBlock + (int)(pos % kBlock);
}

bool MemoryCard::read_frame(int frame, uint8_t* out) const {
    if (frame < 0 || frame >= kSize / kFrame) return false;
    std::memcpy(out, &img_[frame * kFrame], kFrame);
    return true;
}

bool MemoryCard::write_frame(int frame, const uint8_t* in) {
    if (frame < 0 || frame >= kSize / kFrame) return false;
    std::memcpy(&img_[frame * kFrame], in, kFrame);
    dirty_ = true;
    return true;
}

MemoryCard& memcard(int port) {
    static MemoryCard cards[2];
    return cards[port & 1];
}
