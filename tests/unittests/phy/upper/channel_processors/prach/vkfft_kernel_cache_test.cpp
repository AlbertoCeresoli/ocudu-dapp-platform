// SPDX-FileCopyrightText: Copyright (C) 2021-2026 DeepSig Inc
// SPDX-FileCopyrightText: Copyright (C) 2021-2026 Software Radio Systems Limited
// SPDX-License-Identifier: BSD-3-Clause-Open-MPI
// Portions of this file may implement 3GPP specifications, which may be subject to additional licensing requirements.

/// \file
/// \brief Tests for the on-disk cache of compiled VkFFT kernel images.
///
/// VkFFT is handed a bare pointer to load a compiled kernel from and reads a length out of the image itself, so a
/// file that is shorter than the image claims to be is read past its end. The cache therefore only hands an image
/// over once it has checked it against a header of its own. These tests cover that check, and the writing side
/// that has to leave a reader with either the whole previous image or the whole new one. No GPU is involved.

#include "vkfft_kernel_cache.h"
#include <cstdio>
#include <gtest/gtest.h>
#include <string>
#include <vector>

using namespace ocudu::vkfft_cache;

namespace {

/// Scratch directory that cleans up after itself.
class cache_dir_fixture : public ::testing::Test
{
protected:
  void SetUp() override
  {
    std::string pattern = testing::TempDir() + "ocudu-vkfft-cache-XXXXXX";
    char*       made    = ::mkdtemp(pattern.data());
    ASSERT_NE(made, nullptr);
    dir  = made;
    path = dir + "/image.bin";
  }

  void TearDown() override
  {
    ::remove(path.c_str());
    ::rmdir(dir.c_str());
  }

  /// Overwrites the image file with exactly these bytes.
  void write_raw(const std::vector<char>& bytes) const
  {
    std::FILE* file = std::fopen(path.c_str(), "wb");
    ASSERT_NE(file, nullptr);
    if (!bytes.empty()) {
      ASSERT_EQ(std::fwrite(bytes.data(), 1, bytes.size(), file), bytes.size());
    }
    std::fclose(file);
  }

  /// Reads the image file back byte for byte.
  std::vector<char> read_raw() const
  {
    std::FILE* file = std::fopen(path.c_str(), "rb");
    EXPECT_NE(file, nullptr);
    std::vector<char> bytes;
    char              chunk[512];
    size_t            got = 0;
    while ((got = std::fread(chunk, 1, sizeof(chunk), file)) != 0) {
      bytes.insert(bytes.end(), chunk, chunk + got);
    }
    std::fclose(file);
    return bytes;
  }

  std::string dir;
  std::string path;
};

const std::vector<char> sample_image = {'c', 'u', 'b', 'i', 'n', '\0', '\x01', '\x02', '\x7f', 'z'};

} // namespace

TEST_F(cache_dir_fixture, an_image_survives_a_write_and_a_read)
{
  ASSERT_TRUE(write_image(path, sample_image.data(), sample_image.size()));

  std::vector<char> loaded;
  ASSERT_TRUE(read_image(path, loaded));
  EXPECT_EQ(loaded, sample_image);
}

TEST_F(cache_dir_fixture, a_truncated_image_is_refused)
{
  // The case that matters: VkFFT would read a length out of the image and copy that many bytes from a buffer that
  // no longer holds them. Every prefix of a good file has to be refused, not handed over.
  ASSERT_TRUE(write_image(path, sample_image.data(), sample_image.size()));
  const std::vector<char> whole = read_raw();
  ASSERT_GT(whole.size(), sample_image.size());

  for (size_t kept = 0; kept != whole.size(); ++kept) {
    write_raw(std::vector<char>(whole.begin(), whole.begin() + kept));
    std::vector<char> loaded;
    EXPECT_FALSE(read_image(path, loaded)) << "a file of " << kept << " bytes must not be loaded";
    EXPECT_TRUE(loaded.empty());
  }

  // The whole file still loads, so the refusals above are not simply a broken reader.
  write_raw(whole);
  std::vector<char> loaded;
  EXPECT_TRUE(read_image(path, loaded));
  EXPECT_EQ(loaded, sample_image);
}

TEST_F(cache_dir_fixture, an_edited_image_is_refused)
{
  ASSERT_TRUE(write_image(path, sample_image.data(), sample_image.size()));
  std::vector<char> whole = read_raw();

  for (size_t index = 0; index != whole.size(); ++index) {
    std::vector<char> edited = whole;
    edited[index] ^= 0x5a;
    write_raw(edited);
    std::vector<char> loaded;
    EXPECT_FALSE(read_image(path, loaded)) << "a file edited at byte " << index << " must not be loaded";
  }
}

TEST_F(cache_dir_fixture, a_longer_file_is_refused)
{
  // A file that grew, whether by a stray append or an interleaved writer, no longer matches its header.
  ASSERT_TRUE(write_image(path, sample_image.data(), sample_image.size()));
  std::vector<char> grown = read_raw();
  grown.push_back('x');
  write_raw(grown);

  std::vector<char> loaded;
  EXPECT_FALSE(read_image(path, loaded));
}

TEST_F(cache_dir_fixture, a_file_that_is_not_an_image_is_refused)
{
  write_raw(std::vector<char>(256, 'j'));
  std::vector<char> loaded;
  EXPECT_FALSE(read_image(path, loaded));
}

TEST_F(cache_dir_fixture, a_missing_image_is_refused)
{
  std::vector<char> loaded;
  EXPECT_FALSE(read_image(dir + "/never-written.bin", loaded));
}

TEST_F(cache_dir_fixture, an_empty_image_is_not_written)
{
  EXPECT_FALSE(write_image(path, sample_image.data(), 0));
  EXPECT_FALSE(write_image(path, nullptr, 8));
  std::vector<char> loaded;
  EXPECT_FALSE(read_image(path, loaded));
}

TEST_F(cache_dir_fixture, a_rewrite_replaces_the_image_and_leaves_nothing_behind)
{
  // The write goes to a name of its own and is renamed into place, so a reader never sees a half-written file and
  // two writers cannot land on the same temporary name.
  const std::vector<char> replacement(4096, '\x11');
  ASSERT_TRUE(write_image(path, sample_image.data(), sample_image.size()));
  ASSERT_TRUE(write_image(path, replacement.data(), replacement.size()));

  std::vector<char> loaded;
  ASSERT_TRUE(read_image(path, loaded));
  EXPECT_EQ(loaded, replacement);
}

TEST(vkfft_kernel_cache, caching_can_be_turned_off)
{
  for (const char* value : {"0", "off", "false", ""}) {
    ASSERT_EQ(::setenv("OCUDU_PRACH_VKFFT_CACHE_DIR", value, 1), 0);
    EXPECT_TRUE(cache_directory().empty()) << "OCUDU_PRACH_VKFFT_CACHE_DIR=" << value;
  }

  ASSERT_EQ(::setenv("OCUDU_PRACH_VKFFT_CACHE_DIR", "/somewhere/of/my/own", 1), 0);
  EXPECT_EQ(cache_directory(), "/somewhere/of/my/own");
  ASSERT_EQ(::unsetenv("OCUDU_PRACH_VKFFT_CACHE_DIR"), 0);
}

TEST(vkfft_kernel_cache, the_default_directory_belongs_to_the_user)
{
  // A cached image is a binary this process runs on the GPU, so the default location is the user's own cache
  // directory rather than somewhere every account on the machine can write.
  ASSERT_EQ(::unsetenv("OCUDU_PRACH_VKFFT_CACHE_DIR"), 0);
  ASSERT_EQ(::setenv("XDG_CACHE_HOME", "/home/someone/.cache", 1), 0);
  EXPECT_EQ(cache_directory(), "/home/someone/.cache/ocudu/prach-vkfft");

  ASSERT_EQ(::unsetenv("XDG_CACHE_HOME"), 0);
  ASSERT_EQ(::setenv("HOME", "/home/someone", 1), 0);
  EXPECT_EQ(cache_directory(), "/home/someone/.cache/ocudu/prach-vkfft");
}

TEST_F(cache_dir_fixture, the_cache_directory_is_created_for_its_owner_only)
{
  const std::string nested = dir + "/a/b/c";
  ASSERT_TRUE(make_cache_directory(nested));
  ASSERT_TRUE(make_cache_directory(nested)) << "creating it again must succeed";

  struct stat info = {};
  ASSERT_EQ(::stat(nested.c_str(), &info), 0);
  EXPECT_TRUE(S_ISDIR(info.st_mode));
  EXPECT_EQ(info.st_mode & 0777, 0700U);

  ::rmdir((dir + "/a/b/c").c_str());
  ::rmdir((dir + "/a/b").c_str());
  ::rmdir((dir + "/a").c_str());
}
