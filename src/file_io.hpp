// Private crash-safe file helpers.
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <cstddef>
#include <filesystem>
#include <span>
#include <vector>

#include "hgm/error.hpp"

namespace hgm {

// Creates the directory (and parents) when missing.
Status ensure_directory(const std::filesystem::path& directory);

bool file_exists(const std::filesystem::path& path);
std::uint64_t file_size(const std::filesystem::path& path);

// Truncating write followed by a durability barrier.
Status write_file_all(const std::filesystem::path& path, std::span<const std::byte> bytes);

// Appends and flushes to the OS, without a durability barrier.
Status append_file_all(const std::filesystem::path& path, std::span<const std::byte> bytes);

// Durability barrier for an existing file.
Status fsync_file(const std::filesystem::path& path);

// Atomically replaces "to" with "from" (same volume).
Status atomic_replace(const std::filesystem::path& from, const std::filesystem::path& to);

// Reads a whole file. Fails when the file exceeds max_bytes.
Result<std::vector<std::byte>> read_file_all(const std::filesystem::path& path, std::uint64_t max_bytes);

// Truncates the file to the given size.
Status truncate_file(const std::filesystem::path& path, std::uint64_t size);

Status remove_file(const std::filesystem::path& path);

}  // namespace hgm
