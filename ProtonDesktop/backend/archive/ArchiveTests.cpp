// SPDX-License-Identifier: GPL-2.0-only
// Exercises the real pinned minizip implementation, including CRC rejection.
#include <zip.h>
#include <unzip.h>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <stdexcept>
#include <string>
#include <vector>

namespace fs = std::filesystem;
static unsigned checks;
static void Check(bool value, const char* text)
{
    ++checks;
    if (!value) throw std::runtime_error(text);
}
static void Add(zipFile archive, const char* name, const std::string& content)
{
    Check(zipOpenNewFileInZip64(archive, name, nullptr, nullptr, 0, nullptr, 0,
        nullptr, Z_DEFLATED, Z_DEFAULT_COMPRESSION, 1) == ZIP_OK, "open ZIP64 member");
    Check(zipWriteInFileInZip(archive, content.data(), static_cast<unsigned>(content.size())) == ZIP_OK,
          "write deflated member");
    Check(zipCloseFileInZip(archive) == ZIP_OK, "finish member");
}
static std::string Read(unzFile archive, const char* name)
{
    Check(unzLocateFile(archive, name, 1) == UNZ_OK, "locate exact member");
    unz_file_info64 information{};
    Check(unzGetCurrentFileInfo64(archive, &information, nullptr, 0, nullptr, 0, nullptr, 0) == UNZ_OK,
          "read 64-bit member metadata");
    Check(information.uncompressed_size < 1024 * 1024, "bounded self-authored member");
    Check(unzOpenCurrentFile(archive) == UNZ_OK, "open actual deflate stream");
    std::string result;
    char part[3];
    for (;;)
    {
        const int count = unzReadCurrentFile(archive, part, sizeof(part));
        Check(count >= 0, "decompress member");
        if (!count) break;
        result.append(part, static_cast<std::size_t>(count));
    }
    Check(result.size() == information.uncompressed_size, "exact uncompressed byte count");
    Check(unzCloseCurrentFile(archive) == UNZ_OK, "validate member CRC");
    return result;
}
int main(int argc, char** argv)
{
    try
    {
        static_assert(sizeof(ZPOS64_T) >= 8, "ZIP64 offsets must be 64 bit");
        Check(argc == 2, "explicit task-owned output directory");
        const fs::path directory = fs::path(argv[1]) /
            std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());
        fs::create_directories(directory);
        const auto original = directory / "runtime.zip";
        const auto truncated = directory / "truncated.zip";
        const auto damaged = directory / "damaged.zip";
        const std::string content = "Real ProtonDesktop archive data\n";
        zipFile writer = zipOpen64(original.string().c_str(), APPEND_STATUS_CREATE);
        Check(writer != nullptr, "create archive using pinned writer");
        Add(writer, "usr/lib/wine/check.txt", content);
        Add(writer, "usr/lib/wine/alias.link", "check.txt");
        Check(zipClose(writer, nullptr) == ZIP_OK, "finish archive");
        unzFile reader = unzOpen64(original.string().c_str());
        Check(reader != nullptr, "open real archive");
        Check(Read(reader, "usr/lib/wine/check.txt") == content, "actual payload bytes");
        unz64_file_pos position{};
        Check(unzGetFilePos64(reader, &position) == UNZ_OK, "save ZIP64 member position");
        Check(Read(reader, "usr/lib/wine/alias.link") == "check.txt", "Boxedwine link-file payload");
        Check(unzGoToFilePos64(reader, &position) == UNZ_OK, "restore ZIP64 member position");
        Check(unzLocateFile(reader, "absent", 1) == UNZ_END_OF_LIST_OF_FILE, "missing member rejected");
        Check(unzClose(reader) == UNZ_OK, "close archive");
        std::ifstream input(original, std::ios::binary);
        std::vector<unsigned char> bytes((std::istreambuf_iterator<char>(input)), {});
        input.close(); // Windows cannot remove the fixture while this stream owns it.
        Check(bytes.size() > 64, "complete self-authored archive");
        {
            std::ofstream output(truncated, std::ios::binary);
            output.write(reinterpret_cast<const char*>(bytes.data()), 20);
        }
        reader = unzOpen64(truncated.string().c_str());
        Check(reader == nullptr, "truncated archive rejected");
        const auto word = [&bytes](std::size_t at) {
            return static_cast<unsigned>(bytes.at(at)) | (static_cast<unsigned>(bytes.at(at + 1)) << 8);
        };
        const auto payload = 30 + word(26) + word(28);
        Check(payload + 4 < bytes.size(), "member data offset in bounds");
        bytes.at(payload + 2) ^= 0x40;
        {
            std::ofstream output(damaged, std::ios::binary);
            output.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
        }
        reader = unzOpen64(damaged.string().c_str());
        Check(reader != nullptr, "damaged payload retains readable directory");
        Check(unzLocateFile(reader, "usr/lib/wine/check.txt", 1) == UNZ_OK, "locate damaged member");
        Check(unzOpenCurrentFile(reader) == UNZ_OK, "open damaged stream");
        int count = 0;
        char buffer[1024];
        std::size_t readCount = 0;
        do
        {
            count = unzReadCurrentFile(reader, buffer, sizeof(buffer));
            if (count > 0) readCount += static_cast<unsigned>(count);
        } while (count > 0 && readCount < 1024 * 1024);
        const int closed = unzCloseCurrentFile(reader);
        Check(count < 0 || closed != UNZ_OK, "corruption rejected by deflate or CRC");
        unzClose(reader);
        fs::remove_all(directory);
        std::cout << checks << " real pinned minizip checks passed\n";
        return 0;
    }
    catch (const std::exception& error)
    {
        std::cerr << "Archive check failed: " << error.what() << '\n';
        return 1;
    }
}
