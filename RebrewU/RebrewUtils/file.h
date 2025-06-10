#pragma once

#include <vector>
#include <cstdint>
#include <string>

// File loading utilities for RebrewU
std::vector<uint8_t> LoadFile(const char* filename);
std::vector<uint8_t> LoadFile(const std::string& filename);

bool SaveFile(const char* filename, const void* data, size_t size);
bool SaveFile(const std::string& filename, const void* data, size_t size);
bool SaveFile(const std::string& filename, const std::vector<uint8_t>& data);

// File utilities
bool FileExists(const char* filename);
bool FileExists(const std::string& filename);
size_t GetFileSize(const char* filename);
size_t GetFileSize(const std::string& filename);

// Path utilities
std::string GetFileName(const std::string& path);
std::string GetFileExtension(const std::string& path);
std::string GetDirectoryPath(const std::string& path);
std::string JoinPath(const std::string& dir, const std::string& file);

// RPX specific utilities
bool IsRPXFile(const char* filename);
bool IsRPXFile(const std::string& filename);
bool ValidateRPXHeader(const void* data, size_t size);