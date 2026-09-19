// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0
#include "file_io.hpp"

#include <cstdio>
#include <cstring>
#include <string>
#include <system_error>

#if defined(_WIN32)
  #ifndef WIN32_LEAN_AND_MEAN
    #define WIN32_LEAN_AND_MEAN
  #endif
  #ifndef NOMINMAX
    #define NOMINMAX
  #endif
  #include <windows.h>
  #include <io.h>
#else
  #include <fcntl.h>
  #include <unistd.h>
#endif

namespace hgm {
namespace {

#if defined(_WIN32)
std::FILE* open_binary(const std::filesystem::path& path, const wchar_t* mode) {
  return ::_wfopen(path.c_str(), mode);
}
#else
std::FILE* open_binary(const std::filesystem::path& path, const char* mode) {
  return std::fopen(path.c_str(), mode);
}
#endif

}  // namespace

Status ensure_directory(const std::filesystem::path& directory) {
  std::error_code error;
  if (std::filesystem::exists(directory, error)) {
    if (std::filesystem::is_directory(directory, error)) return Status::success();
    return Status::failure(ErrorCode::PersistenceIoError, "path exists and is not a directory");
  }
  if (!std::filesystem::create_directories(directory, error) && error) {
    return Status::failure(ErrorCode::PersistenceIoError,
                           "could not create directory: " + error.message());
  }
  return Status::success();
}

bool file_exists(const std::filesystem::path& path) {
  std::error_code error;
  return std::filesystem::exists(path, error) && !error;
}

std::uint64_t file_size(const std::filesystem::path& path) {
  std::error_code error;
  const auto size = std::filesystem::file_size(path, error);
  if (error) return 0;
  return static_cast<std::uint64_t>(size);
}

Status write_file_all(const std::filesystem::path& path, std::span<const std::byte> bytes) {
#if defined(_WIN32)
  std::FILE* file = open_binary(path, L"wb");
#else
  std::FILE* file = open_binary(path, "wb");
#endif
  if (file == nullptr) {
    return Status::failure(ErrorCode::PersistenceIoError, "could not open file for writing");
  }
  if (!bytes.empty()) {
    const std::size_t written = std::fwrite(bytes.data(), 1, bytes.size(), file);
    if (written != bytes.size()) {
      std::fclose(file);
      return Status::failure(ErrorCode::PersistenceIoError, "short write");
    }
  }
  if (std::fflush(file) != 0) {
    std::fclose(file);
    return Status::failure(ErrorCode::PersistenceIoError, "flush failed");
  }
#if defined(_WIN32)
  const int descriptor = ::_fileno(file);
  if (descriptor >= 0) static_cast<void>(::_commit(descriptor));
#else
  const int descriptor = ::fileno(file);
  if (descriptor >= 0) static_cast<void>(::fsync(descriptor));
#endif
  if (std::fclose(file) != 0) {
    return Status::failure(ErrorCode::PersistenceIoError, "close failed");
  }
  return Status::success();
}

Status append_file_all(const std::filesystem::path& path, std::span<const std::byte> bytes) {
#if defined(_WIN32)
  std::FILE* file = open_binary(path, L"ab");
#else
  std::FILE* file = open_binary(path, "ab");
#endif
  if (file == nullptr) {
    return Status::failure(ErrorCode::PersistenceIoError, "could not open file for appending");
  }
  if (!bytes.empty()) {
    const std::size_t written = std::fwrite(bytes.data(), 1, bytes.size(), file);
    if (written != bytes.size()) {
      std::fclose(file);
      return Status::failure(ErrorCode::PersistenceIoError, "short append");
    }
  }
  if (std::fflush(file) != 0) {
    std::fclose(file);
    return Status::failure(ErrorCode::PersistenceIoError, "flush failed");
  }
  if (std::fclose(file) != 0) {
    return Status::failure(ErrorCode::PersistenceIoError, "close failed");
  }
  return Status::success();
}

Status fsync_file(const std::filesystem::path& path) {
#if defined(_WIN32)
  std::FILE* file = open_binary(path, L"rb+");
#else
  std::FILE* file = open_binary(path, "rb+");
#endif
  if (file == nullptr) {
    return Status::failure(ErrorCode::PersistenceIoError, "could not open file for the durability barrier");
  }
#if defined(_WIN32)
  const int descriptor = ::_fileno(file);
  const int result = descriptor >= 0 ? ::_commit(descriptor) : -1;
#else
  const int descriptor = ::fileno(file);
  const int result = descriptor >= 0 ? ::fsync(descriptor) : -1;
#endif
  std::fclose(file);
  if (result != 0) {
    return Status::failure(ErrorCode::PersistenceIoError, "durability barrier failed");
  }
  return Status::success();
}

Status atomic_replace(const std::filesystem::path& from, const std::filesystem::path& to) {
#if defined(_WIN32)
  if (::MoveFileExW(from.c_str(), to.c_str(),
                    MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) == 0) {
    return Status::failure(ErrorCode::PersistenceIoError, "atomic replace failed");
  }
#else
  if (::rename(from.c_str(), to.c_str()) != 0) {
    return Status::failure(ErrorCode::PersistenceIoError, "atomic replace failed");
  }
  // Durability of the rename itself.
  const int dir = ::open(to.parent_path().c_str(), O_RDONLY);
  if (dir >= 0) {
    static_cast<void>(::fsync(dir));
    static_cast<void>(::close(dir));
  }
#endif
  return Status::success();
}

Result<std::vector<std::byte>> read_file_all(const std::filesystem::path& path, std::uint64_t max_bytes) {
  const std::uint64_t size = hgm::file_size(path);
  if (size > max_bytes) {
    return Status::failure(ErrorCode::DurableGrowthExceeded, "durable file exceeds the configured bound");
  }
#if defined(_WIN32)
  std::FILE* file = open_binary(path, L"rb");
#else
  std::FILE* file = open_binary(path, "rb");
#endif
  if (file == nullptr) {
    return Status::failure(ErrorCode::PersistenceIoError, "could not open file for reading");
  }
  std::vector<std::byte> bytes(static_cast<std::size_t>(size));
  if (size > 0) {
    const std::size_t read = std::fread(bytes.data(), 1, bytes.size(), file);
    if (read != bytes.size()) {
      std::fclose(file);
      return Status::failure(ErrorCode::PersistenceTruncated, "short read");
    }
  }
  std::fclose(file);
  return bytes;
}

Status truncate_file(const std::filesystem::path& path, std::uint64_t size) {
  std::error_code error;
  std::filesystem::resize_file(path, static_cast<std::uintmax_t>(size), error);
  if (error) {
    return Status::failure(ErrorCode::PersistenceIoError, "could not truncate file");
  }
  return Status::success();
}

Status remove_file(const std::filesystem::path& path) {
  std::error_code error;
  static_cast<void>(std::filesystem::remove(path, error));
  if (error) {
    return Status::failure(ErrorCode::PersistenceIoError, "could not remove file");
  }
  return Status::success();
}

}  // namespace hgm
