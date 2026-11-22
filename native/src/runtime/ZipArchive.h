#pragma once

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <string>
#include <unordered_map>

namespace canton::runtime {
    // 只读 ZIP。只认 UI 包用到的子集：存储或 deflate、无 ZIP64、无分卷、无归档注释；打开时核对整个索引
    class ZipArchive {
       public:
        struct Entry {
            std::uint16_t method = 0;
            std::uint32_t crc = 0;
            std::uint32_t compressedSize = 0;
            std::uint32_t size = 0;
            std::uint64_t dataOffset = 0;
        };

        // entryLimit 是条目数上限，sizeLimit 是单个条目解压后的字节上限
        ZipArchive(const std::filesystem::path& path, std::size_t entryLimit, std::size_t sizeLimit);

        [[nodiscard]] const std::unordered_map<std::string, Entry>& entries() const noexcept {
            return entries_;
        }

        // 读出并解压一个条目，核对长度与 CRC；可从多个线程调用
        [[nodiscard]] std::string read(const std::string& name, std::size_t limit) const;

       private:
        mutable std::mutex mutex_;
        mutable std::ifstream file_;
        std::unordered_map<std::string, Entry> entries_;
    };
}
