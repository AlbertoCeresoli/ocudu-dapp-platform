// SPDX-FileCopyrightText: Copyright (C) 2021-2026 DeepSig Inc
// SPDX-FileCopyrightText: Copyright (C) 2021-2026 Software Radio Systems Limited
// SPDX-License-Identifier: BSD-3-Clause-Open-MPI
// Portions of this file may implement 3GPP specifications, which may be subject to additional licensing requirements.

/// \file
/// \brief On-disk cache for compiled VkFFT kernel images.
///
/// Building a VkFFT plan compiles a kernel with NVRTC, which costs tens of milliseconds and produces the same image
/// every time for the same inputs. Keeping that image on disk turns the compile into a file read from the second
/// process start onwards, and lets the pooled detector instances of a single start share one compile.
///
/// \section Why the image is wrapped
///
/// VkFFT is handed a bare pointer to load from and is never told how long the buffer is: it reads a size out of the
/// image and copies that many bytes (see \c loadApplicationString in the VkFFT sources). A truncated file therefore
/// reads past the end of the buffer rather than failing, so an image is only ever handed over after its length and
/// contents have been checked against a header written alongside it. A file that does not check out is ignored and
/// the caller compiles as if the cache were empty.

#pragma once

#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <string>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>

namespace ocudu::vkfft_cache {

/// Header written in front of every cached image.
struct image_header {
  /// Identifies the container and its layout. Bumped whenever the layout changes.
  char magic[8];
  /// Length of the image that follows, in bytes.
  uint64_t payload_size;
  /// Hash of the image that follows.
  uint64_t payload_hash;
};

/// Value of \ref image_header::magic written by this version.
inline const char* image_magic()
{
  return "OCUVKF01";
}

/// FNV-1a over \c size bytes. Guards against a torn or edited file, not against a forged one.
inline uint64_t hash_image(const void* data, size_t size)
{
  uint64_t       hash  = 0xcbf29ce484222325ULL;
  const uint8_t* bytes = static_cast<const uint8_t*>(data);
  for (size_t i = 0; i != size; ++i) {
    hash ^= bytes[i];
    hash *= 0x100000001b3ULL;
  }
  return hash;
}

/// \brief Directory where the images live in, or empty when caching is off.
///
/// \c OCUDU_PRACH_VKFFT_CACHE_DIR names it "0", "off", "false", and the empty string turn caching off. Without it
/// the images go under the user's own cache directory: an image is a binary this process then runs on the GPU, so
/// it is not read from a directory where other users of the machine can write.
inline std::string cache_directory()
{
  if (const char* env = std::getenv("OCUDU_PRACH_VKFFT_CACHE_DIR"); env != nullptr) {
    std::string dir(env);
    if (dir.empty() || (dir == "0") || (dir == "off") || (dir == "false")) {
      return {};
    }
    return dir;
  }

  const char* base = std::getenv("XDG_CACHE_HOME");
  if ((base == nullptr) || (base[0] == '\0')) {
    base = std::getenv("HOME");
    if ((base == nullptr) || (base[0] == '\0')) {
      return {};
    }
    return std::string(base) + "/.cache/ocudu/prach-vkfft";
  }
  return std::string(base) + "/ocudu/prach-vkfft";
}

/// Creates directory and its parents with owner-only access.
/// Returns false if it cannot be used.
inline bool make_cache_directory(const std::string& directory)
{
  if (directory.empty()) {
    return false;
  }
  for (size_t pos = directory.find('/', 1); pos != std::string::npos; pos = directory.find('/', pos + 1)) {
    std::string parent = directory.substr(0, pos);
    if ((::mkdir(parent.c_str(), 0700) != 0) && (errno != EEXIST)) {
      return false;
    }
  }
  if ((::mkdir(directory.c_str(), 0700) != 0) && (errno != EEXIST)) {
    return false;
  }
  struct stat info = {};
  return (::stat(directory.c_str(), &info) == 0) && S_ISDIR(info.st_mode);
}

/// \brief Reads the image at path into payload
///
/// \return True only if the file is present, complete and unchanged since it was written. \c payload is left empty
/// otherwise, and the caller compiles instead.
inline bool read_image(const std::string& path, std::vector<char>& payload)
{
  payload.clear();
  if (path.empty()) {
    return false;
  }

  std::ifstream in(path, std::ios::binary);
  if (!in) {
    return false;
  }
  std::vector<char> file((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
  if (file.size() < sizeof(image_header)) {
    return false;
  }

  image_header header = {};
  std::memcpy(&header, file.data(), sizeof(header));
  if (std::memcmp(header.magic, image_magic(), sizeof(header.magic)) != 0) {
    return false;
  }
  // The recorded length has to account for exactly what is in the file: this is the check
  // that keeps VkFFT, which takes the buffer without a length, from reading
  // past the end of a truncated buffer.
  if (header.payload_size != (file.size() - sizeof(image_header))) {
    return false;
  }
  if (header.payload_size == 0) {
    return false;
  }
  const char* body = file.data() + sizeof(image_header);
  if (hash_image(body, header.payload_size) != header.payload_hash) {
    return false;
  }

  payload.assign(body, body + header.payload_size);
  return true;
}

/// Writes the whole buffer to \c fd retrying short writes
inline bool write_all(int fd, const void* data, size_t size)
{
  const char* bytes = static_cast<const char*>(data);
  size_t      left  = size;
  while (left != 0) {
    ssize_t written = ::write(fd, bytes, left);
    if (written <= 0) {
      if ((written < 0) && (errno == EINTR)) {
        continue;
      }
      return false;
    }
    bytes += written;
    left -= static_cast<size_t>(written);
  }
  return true;
}

/// \brief Writes size bytes at data as the image at path
///
/// Writes to a name of its own and renames over \c path, so a reader sees either the previous image or this one and
/// concurrent writers cannot interleave. Failures are silent: a cache that cannot be written only costs a compile.
inline bool write_image(const std::string& path, const void* data, size_t size)
{
  if (path.empty() || (data == nullptr) || (size == 0)) {
    return false;
  }

  std::string tmp_path = path + ".XXXXXX";
  int         fd       = ::mkstemp(tmp_path.data());
  if (fd < 0) {
    return false;
  }

  image_header header = {};
  std::memcpy(header.magic, image_magic(), sizeof(header.magic));
  header.payload_size = size;
  header.payload_hash = hash_image(data, size);

  // Written through the descriptor mkstemp handed over, so nothing can be substituted
  // between creating the file and filling it.
  bool written = write_all(fd, &header, sizeof(header)) && write_all(fd, data, size);
  written      = written && (::fsync(fd) == 0);
  if (::close(fd) != 0) {
    written = false;
  }

  if (!written || (::rename(tmp_path.c_str(), path.c_str()) != 0)) {
    ::remove(tmp_path.c_str());
    return false;
  }
  return true;
}

} // namespace ocudu::vkfft_cache
