// Memory card: raw 128 KB images in the standard PS1 layout (.mcr, emulator compatible),
// plus the directory/file operations the BIOS file API needs.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

class MemoryCard {
public:
    static constexpr int kSize = 128 * 1024;
    static constexpr int kFrame = 128;
    static constexpr int kBlock = 8192;
    static constexpr int kBlocks = 15;  // data blocks 1..15

    struct FileInfo {
        std::string name;
        int first_block;   // 1..15
        uint32_t size;     // bytes
    };

    bool load_or_create(const std::string& path);
    bool present() const { return !path_.empty(); }
    void flush();

    std::vector<FileInfo> list() const;
    bool find(const std::string& name, FileInfo* out) const;
    // Allocates `blocks` data blocks for a new file; fails if it exists or no space.
    bool create(const std::string& name, int blocks, FileInfo* out);
    bool remove(const std::string& name);
    void format();

    // Byte offset within the file -> absolute offset in the image.
    int file_offset(const FileInfo& f, uint32_t pos) const;
    uint8_t* data() { return img_.data(); }
    void mark_dirty() { dirty_ = true; }

    // Raw frame access used by the low-level _card_read/_card_write calls.
    bool read_frame(int frame, uint8_t* out) const;
    bool write_frame(int frame, const uint8_t* in);

private:
    uint8_t* dir(int block) { return &img_[block * kFrame]; }
    const uint8_t* dir(int block) const { return &img_[block * kFrame]; }
    void fix_checksum(int frame);

    std::string path_;
    std::vector<uint8_t> img_;
    bool dirty_ = false;
};

MemoryCard& memcard(int port);
