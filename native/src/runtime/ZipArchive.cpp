#include <runtime/ZipArchive.h>

#include <runtime/Digest.h>

#include <miniz_tinfl.h>

#include <array>
#include <stdexcept>
#include <vector>

namespace canton::runtime {
    namespace {
        [[noreturn]] void fail(const std::string& reason, const std::string& name = {}) {
            throw std::runtime_error("UI 包：" + reason + (name.empty() ? "" : "：" + name));
        }

        std::uint16_t u16(const unsigned char* bytes) {
            return static_cast<std::uint16_t>(bytes[0] | (bytes[1] << 8));
        }

        std::uint32_t u32(const unsigned char* bytes) {
            return static_cast<std::uint32_t>(bytes[0]) | (static_cast<std::uint32_t>(bytes[1]) << 8) | (static_cast<std::uint32_t>(bytes[2]) << 16) | (static_cast<std::uint32_t>(bytes[3]) << 24);
        }
    }

    ZipArchive::ZipArchive(const std::filesystem::path& path, std::size_t entryLimit, std::size_t sizeLimit) : file_(path, std::ios::binary) {
        if (!file_) fail("ZIP 文件无法打开");
        file_.seekg(0, std::ios::end);
        const auto length = static_cast<std::int64_t>(file_.tellg());
        if (length < 22 || length >= (std::int64_t{1} << 31)) fail("ZIP 大小或文件无效");
        const auto readAt = [&](std::int64_t offset, void* buffer, std::size_t size) {
            file_.clear();
            file_.seekg(offset);
            if (!file_.read(static_cast<char*>(buffer), static_cast<std::streamsize>(size))) fail("ZIP 索引截断");
        };
        std::array<unsigned char, 22> end{};
        readAt(length - 22, end.data(), end.size());
        const std::size_t count = u16(end.data() + 10);
        const std::int64_t size = u32(end.data() + 12), start = u32(end.data() + 16);
        if (u32(end.data()) != 0x06054b50 || u32(end.data() + 4) != 0 || u16(end.data() + 8) != count || u16(end.data() + 20) != 0 || count < 1 || count > entryLimit || size > 16 * 1024 * 1024 || start + size != length - 22) fail("ZIP 目录无效（不支持 ZIP64、分卷或归档注释）");
        auto position = start;
        for (std::size_t i = 0; i < count; ++i) {
            std::array<unsigned char, 46> header{};
            readAt(position, header.data(), header.size());
            const auto flags = u16(header.data() + 8), method = u16(header.data() + 10), nameLength = u16(header.data() + 28);
            const auto compressed = u32(header.data() + 20), uncompressed = u32(header.data() + 24), localOffset = u32(header.data() + 42);
            // 允许的标志位：数据描述符（bit 3）、UTF-8 文件名（bit 11）与 deflate 的压缩级别提示（bit 1、2）
            if (u32(header.data()) != 0x02014b50 || (flags & ~0x080e) != 0 || (method != 0 && method != 8) || nameLength == 0 || nameLength > 1024 || u16(header.data() + 34) != 0 || uncompressed > sizeLimit || localOffset + std::uint64_t{30} > static_cast<std::uint64_t>(start)) fail("ZIP 包含不支持或损坏的条目");
            std::array<unsigned char, 30> local{};
            readAt(localOffset, local.data(), local.size());
            const auto dataOffset = localOffset + std::uint64_t{30} + nameLength + u16(local.data() + 28);
            if (u32(local.data()) != 0x04034b50 || u16(local.data() + 6) != flags || u16(local.data() + 8) != method || u16(local.data() + 26) != nameLength || dataOffset + compressed > static_cast<std::uint64_t>(start)) fail("ZIP 条目头不匹配");
            std::string name(nameLength, '\0'), localName(nameLength, '\0');
            readAt(position + 46, name.data(), nameLength);
            readAt(localOffset + 30, localName.data(), nameLength);
            if (name != localName) fail("ZIP 条目名不匹配", name);
            if (!entries_.emplace(name, Entry{method, u32(header.data() + 16), compressed, uncompressed, dataOffset}).second) fail("ZIP 条目重名", name);
            position += 46 + nameLength + u16(header.data() + 30) + u16(header.data() + 32);
            if (position > length - 22) fail("ZIP 索引越界");
        }
        if (position != length - 22) fail("ZIP 索引长度不匹配");
    }

    std::string ZipArchive::read(const std::string& name, std::size_t limit) const {
        const auto found = entries_.find(name);
        if (found == entries_.end()) fail("ZIP 缺少条目", name);
        const auto& entry = found->second;
        if (entry.size > limit) fail("ZIP 条目超过读取预算", name);
        std::vector<char> compressed(entry.compressedSize);
        {
            const std::scoped_lock lock(mutex_);
            file_.clear();
            file_.seekg(static_cast<std::streamoff>(entry.dataOffset));
            if (!compressed.empty() && !file_.read(compressed.data(), static_cast<std::streamsize>(compressed.size()))) fail("ZIP 条目读取失败", name);
        }
        std::string result(entry.size, '\0');
        if (entry.method == 0) {
            if (entry.compressedSize != entry.size) fail("ZIP 条目长度与声明不符", name);
            result.assign(compressed.begin(), compressed.end());
        } else if (entry.size != 0 || entry.compressedSize != 0) {
            // 输出缓冲正好是声明的长度：解出来更长或更短都算失败
            auto inputSize = compressed.size(), outputSize = result.size();
            tinfl_decompressor inflator;
            tinfl_init(&inflator);
            const auto status = tinfl_decompress(&inflator, reinterpret_cast<const mz_uint8*>(compressed.data()), &inputSize, reinterpret_cast<mz_uint8*>(result.data()), reinterpret_cast<mz_uint8*>(result.data()), &outputSize, TINFL_FLAG_USING_NON_WRAPPING_OUTPUT_BUF);
            if (status != TINFL_STATUS_DONE || outputSize != result.size()) fail("ZIP 条目解压失败或长度与声明不符", name);
        }
        if (crc32({reinterpret_cast<const std::uint8_t*>(result.data()), result.size()}) != entry.crc) fail("ZIP 条目校验和不符", name);
        return result;
    }
}
