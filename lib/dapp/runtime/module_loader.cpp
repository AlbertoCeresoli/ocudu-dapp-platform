// SPDX-FileCopyrightText: Copyright (C) 2026 DeepSig Inc.
// SPDX-License-Identifier: BSD-3-Clause-Clear

#include "ocudu/dapp/runtime/module_loader.h"
#include "ocudu/dapp/runtime/abi_validation.h"
#include <cstring>
#include <dlfcn.h>
#include <exception>
#include <fcntl.h>
#include <mutex>
#include <sstream>
#include <unistd.h>
#include <vector>

using namespace ocudu::dapp;

namespace {

constexpr std::size_t maximum_loader_identities = 1024;

/// glibc may retain a C++ DSO after dlclose when it contains GNU-unique symbols.
/// Reserve one stable procfs descriptor name per content digest so a retained
/// link-map can never be mistaken for a different, later admitted inode.
class loader_identity_store
{
public:
  ~loader_identity_store()
  {
    for (const auto& item : entries) {
      ::close(item.descriptor);
    }
  }

  int descriptor_for(const approved_artifact& artifact)
  {
    std::lock_guard<std::mutex> lock(mutex);
    for (const auto& item : entries) {
      if (item.digest == artifact.digest()) {
        return item.descriptor;
      }
    }
    if (entries.size() == maximum_loader_identities) {
      return -1;
    }
    const int descriptor = ::fcntl(artifact.native_descriptor(), F_DUPFD_CLOEXEC, 256);
    if (descriptor < 0) {
      return -1;
    }
    try {
      entries.push_back({artifact.digest(), descriptor});
    } catch (...) {
      ::close(descriptor);
      throw;
    }
    return descriptor;
  }

private:
  struct identity_record {
    artifact_digest digest;
    int             descriptor;
  };

  std::mutex                   mutex;
  std::vector<identity_record> entries;
};

loader_identity_store& loader_identities()
{
  static loader_identity_store store;
  return store;
}

} // namespace

native_module_library::native_module_library(void*                handle_,
                                             ocudu_dapp_module_v1 module_,
                                             artifact_digest      digest_,
                                             std::string          path_) noexcept :
  handle(handle_), module(module_), digest(digest_), path(std::move(path_))
{
}

native_module_library::~native_module_library()
{
  if (handle != nullptr) {
    ::dlclose(handle);
  }
}

module_load_result ocudu::dapp::load_native_module(approved_artifact&& artifact, const ocudu_dapp_host_info_v1& host)
{
  int descriptor = -1;
  try {
    descriptor = loader_identities().descriptor_for(artifact);
  } catch (const std::exception& exception) {
    return {module_load_error::dynamic_load_failed, nullptr, exception.what()};
  } catch (...) {
    return {module_load_error::dynamic_load_failed, nullptr, "loader identity reservation failed"};
  }
  if (descriptor < 0) {
    return {module_load_error::dynamic_load_failed, nullptr, "loader identity capacity or descriptor limit reached"};
  }
  std::ostringstream proc_path;
  proc_path << "/proc/self/fd/" << descriptor;
  void* handle = ::dlopen(proc_path.str().c_str(), RTLD_NOW | RTLD_LOCAL);
  if (handle == nullptr) {
    const char* error = ::dlerror();
    return {module_load_error::dynamic_load_failed, nullptr, error != nullptr ? error : "dlopen failed"};
  }
  struct handle_guard {
    void* value;
    ~handle_guard()
    {
      if (value != nullptr)
        ::dlclose(value);
    }
  } guard{handle};

  ::dlerror();
  void* symbol = ::dlsym(handle, "ocudu_dapp_query_v1");
  if (const char* error = ::dlerror(); error != nullptr || symbol == nullptr) {
    return {module_load_error::missing_query_symbol, nullptr, error != nullptr ? error : "query symbol missing"};
  }
  ocudu_dapp_query_v1_fn query = nullptr;
  static_assert(sizeof(query) == sizeof(symbol), "POSIX function and data pointer sizes differ");
  std::memcpy(&query, &symbol, sizeof(query));

  ocudu_dapp_module_v1 module{};
  module.struct_size = sizeof(module);
  module.abi_major   = OCUDU_DAPP_ABI_MAJOR_V1;
  module.abi_minor   = OCUDU_DAPP_ABI_MINOR_V1;
  ocudu_dapp_status_v1 status;
  try {
    status = query(&host, &module);
  } catch (const std::exception& exception) {
    return {module_load_error::query_exception, nullptr, exception.what()};
  } catch (...) {
    return {module_load_error::query_exception, nullptr, "unknown exception escaped module query"};
  }
  if (status != OCUDU_DAPP_OK_V1) {
    return {module_load_error::query_rejected, nullptr, "module query returned status " + std::to_string(status)};
  }
  if (const auto validation = validate_module_descriptor(&module); validation != validation_error::none) {
    return {module_load_error::invalid_descriptor, nullptr, to_string(validation)};
  }

  guard.value  = nullptr;
  auto library = std::shared_ptr<native_module_library>(
      new native_module_library(handle, module, artifact.digest(), artifact.canonical_path()));
  return {module_load_error::none, std::move(library), {}};
}

const char* ocudu::dapp::to_string(module_load_error error) noexcept
{
  static constexpr const char* names[] = {
      "none", "dynamic load failed", "missing query symbol", "query exception", "query rejected", "invalid descriptor"};
  const auto index = static_cast<unsigned>(error);
  return index < sizeof(names) / sizeof(names[0]) ? names[index] : "unknown module load error";
}
