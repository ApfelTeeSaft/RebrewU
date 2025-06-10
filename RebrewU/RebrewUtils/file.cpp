#include "file.h"
#include "wiiu_ppc.h"
#include <fstream>
#include <filesystem>
#include <cstring>

std::vector<uint8_t> LoadFile(const char* filename) {
    std::ifstream file(filename, std::ios::binary | std::ios::ate);
    if (!file.is_open()) {
        return {};
    }

    std::streamsize size = file.tellg();
    file.seekg(0, std::ios::beg);

    std::vector<uint8_t> buffer(static_cast<size_t>(size));
    if (!file.read(reinterpret_cast<char*>(buffer.data()), size)) {
        return {};
    }

    return buffer;
}

std::vector<uint8_t> LoadFile(const std::string& filename) {
    return LoadFile(filename.c_str());
}

bool SaveFile(const char* filename, const void* data, size_t size) {
    std::ofstream file(filename, std::ios::binary);
    if (!file.is_open()) {
        return false;
    }

    return file.write(static_cast<const char*>(data), size).good();
}

bool SaveFile(const std::string& filename, const void* data, size_t size) {
    return SaveFile(filename.c_str(), data, size);
}

bool SaveFile(const std::string& filename, const std::vector<uint8_t>& data) {
    return SaveFile(filename.c_str(), data.data(), data.size());
}

bool FileExists(const char* filename) {
    return std::filesystem::exists(filename);
}

bool FileExists(const std::string& filename) {
    return std::filesystem::exists(filename);
}

size_t GetFileSize(const char* filename) {
    std::error_code ec;
    auto size = std::filesystem::file_size(filename, ec);
    return ec ? 0 : static_cast<size_t>(size);
}

size_t GetFileSize(const std::string& filename) {
    return GetFileSize(filename.c_str());
}

std::string GetFileName(const std::string& path) {
    std::filesystem::path p(path);
    return p.filename().string();
}

std::string GetFileExtension(const std::string& path) {
    std::filesystem::path p(path);
    return p.extension().string();
}

std::string GetDirectoryPath(const std::string& path) {
    std::filesystem::path p(path);
    return p.parent_path().string();
}

std::string JoinPath(const std::string& dir, const std::string& file) {
    std::filesystem::path result(dir);
    result /= file;
    return result.string();
}

bool IsRPXFile(const char* filename) {
    auto ext = GetFileExtension(filename);
    return ext == ".rpx" || ext == ".RPX";
}

bool IsRPXFile(const std::string& filename) {
    return IsRPXFile(filename.c_str());
}

bool ValidateRPXHeader(const void* data, size_t size) {
    if (size < 16) {
        return false;
    }

    const uint8_t* bytes = static_cast<const uint8_t*>(data);
    
    // Check ELF magic number
    if (bytes[0] != 0x7F || bytes[1] != 'E' || bytes[2] != 'L' || bytes[3] != 'F') {
        return false;
    }

    // Check for 32-bit big-endian
    if (bytes[4] != 1 || bytes[5] != 2) {  // 32-bit, big-endian
        return false;
    }

    // Check for PowerPC architecture (EM_PPC = 20)
    if (size >= 18) {
        uint16_t machine = (bytes[18] << 8) | bytes[19];
        if (machine != 20) {
            return false;
        }
    }

    return true;
}