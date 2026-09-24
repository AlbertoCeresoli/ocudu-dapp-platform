// SPDX-FileCopyrightText: Copyright (C) 2026 DeepSig Inc.
// SPDX-License-Identifier: BSD-3-Clause-Clear

#include "ocudu/dapp/runtime/abi_validation.h"
#include "ocudu/dapp/runtime/native_preflight.h"
#include "ocudu/dapp/use_cases/v1/channel_estimator.h"
#include "ocudu/dapp/use_cases/v1/equalizer.h"
#include "ocudu/dapp/use_cases/v1/receiver.h"
#include "ocudu/dapp/use_cases/v1/scheduler.h"
#include "ocudu/dapp/use_cases/v1/spectrum.h"
#include "ocudu/dapp/use_cases/v1/srs_channel_estimator.h"
#include "ocudu/dapp/use_cases/v1/srs_isac.h"
#include <algorithm>
#include <cerrno>
#include <charconv>
#include <climits>
#include <csignal>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <dlfcn.h>
#include <exception>
#include <fcntl.h>
#include <linux/audit.h>
#include <linux/filter.h>
#include <linux/seccomp.h>
#include <string>
#include <string_view>
#include <sys/prctl.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>
#include <vector>

namespace {

using ocudu::dapp::native_preflight_stage;

constexpr int exit_code(native_preflight_stage stage) noexcept
{
  return static_cast<int>(stage);
}

bool parse_u64(std::string_view text, uint64_t& value) noexcept
{
  const auto result = std::from_chars(text.data(), text.data() + text.size(), value);
  return result.ec == std::errc{} && result.ptr == text.data() + text.size();
}

bool parse_build_id(std::string_view text, uint64_t (&value)[2]) noexcept
{
  const auto separator = text.find(':');
  return separator != std::string_view::npos && parse_u64(text.substr(0, separator), value[0]) &&
         parse_u64(text.substr(separator + 1), value[1]);
}

constexpr uint32_t native_audit_arch() noexcept
{
#if defined(__aarch64__)
  return AUDIT_ARCH_AARCH64;
#elif defined(__x86_64__)
  return AUDIT_ARCH_X86_64;
#else
#error "The native preflight seccomp profile needs an audit architecture mapping"
#endif
}

void append_instruction(std::vector<sock_filter>& filter,
                        uint16_t                  code,
                        uint8_t                   jump_true,
                        uint8_t                   jump_false,
                        uint32_t                  value)
{
  filter.push_back({code, jump_true, jump_false, value});
}

void deny_syscall(std::vector<sock_filter>& filter, int syscall_number)
{
  append_instruction(filter, BPF_JMP | BPF_JEQ | BPF_K, 0, 1, static_cast<uint32_t>(syscall_number));
  append_instruction(filter, BPF_RET | BPF_K, 0, 0, SECCOMP_RET_ERRNO | static_cast<uint32_t>(EPERM));
}

bool install_seccomp_filter() noexcept
{
  try {
    std::vector<sock_filter> filter;
    filter.reserve(96);
    append_instruction(filter, BPF_LD | BPF_W | BPF_ABS, 0, 0, offsetof(seccomp_data, arch));
    append_instruction(filter, BPF_JMP | BPF_JEQ | BPF_K, 1, 0, native_audit_arch());
    append_instruction(filter, BPF_RET | BPF_K, 0, 0, SECCOMP_RET_KILL_PROCESS);
    append_instruction(filter, BPF_LD | BPF_W | BPF_ABS, 0, 0, offsetof(seccomp_data, nr));
#define OCUDU_PREFLIGHT_DENY(syscall_name)                                                                             \
  do {                                                                                                                 \
    deny_syscall(filter, __NR_##syscall_name);                                                                         \
  } while (false)
#ifdef __NR_execve
    OCUDU_PREFLIGHT_DENY(execve);
#endif
#ifdef __NR_execveat
    OCUDU_PREFLIGHT_DENY(execveat);
#endif
#ifdef __NR_fork
    OCUDU_PREFLIGHT_DENY(fork);
#endif
#ifdef __NR_vfork
    OCUDU_PREFLIGHT_DENY(vfork);
#endif
#ifdef __NR_ptrace
    OCUDU_PREFLIGHT_DENY(ptrace);
#endif
#ifdef __NR_process_vm_readv
    OCUDU_PREFLIGHT_DENY(process_vm_readv);
#endif
#ifdef __NR_process_vm_writev
    OCUDU_PREFLIGHT_DENY(process_vm_writev);
#endif
#ifdef __NR_kill
    OCUDU_PREFLIGHT_DENY(kill);
#endif
#ifdef __NR_tkill
    OCUDU_PREFLIGHT_DENY(tkill);
#endif
#ifdef __NR_pidfd_send_signal
    OCUDU_PREFLIGHT_DENY(pidfd_send_signal);
#endif
#ifdef __NR_setpgid
    OCUDU_PREFLIGHT_DENY(setpgid);
#endif
#ifdef __NR_setsid
    OCUDU_PREFLIGHT_DENY(setsid);
#endif
#ifdef __NR_mount
    OCUDU_PREFLIGHT_DENY(mount);
#endif
#ifdef __NR_umount2
    OCUDU_PREFLIGHT_DENY(umount2);
#endif
#ifdef __NR_pivot_root
    OCUDU_PREFLIGHT_DENY(pivot_root);
#endif
#ifdef __NR_setns
    OCUDU_PREFLIGHT_DENY(setns);
#endif
#ifdef __NR_unshare
    OCUDU_PREFLIGHT_DENY(unshare);
#endif
#ifdef __NR_bpf
    OCUDU_PREFLIGHT_DENY(bpf);
#endif
#ifdef __NR_perf_event_open
    OCUDU_PREFLIGHT_DENY(perf_event_open);
#endif
#ifdef __NR_userfaultfd
    OCUDU_PREFLIGHT_DENY(userfaultfd);
#endif
#ifdef __NR_keyctl
    OCUDU_PREFLIGHT_DENY(keyctl);
#endif
#ifdef __NR_add_key
    OCUDU_PREFLIGHT_DENY(add_key);
#endif
#ifdef __NR_request_key
    OCUDU_PREFLIGHT_DENY(request_key);
#endif
#ifdef __NR_open_by_handle_at
    OCUDU_PREFLIGHT_DENY(open_by_handle_at);
#endif
#ifdef __NR_name_to_handle_at
    OCUDU_PREFLIGHT_DENY(name_to_handle_at);
#endif
#ifdef __NR_init_module
    OCUDU_PREFLIGHT_DENY(init_module);
#endif
#ifdef __NR_finit_module
    OCUDU_PREFLIGHT_DENY(finit_module);
#endif
#ifdef __NR_delete_module
    OCUDU_PREFLIGHT_DENY(delete_module);
#endif
#ifdef __NR_kexec_load
    OCUDU_PREFLIGHT_DENY(kexec_load);
#endif
#ifdef __NR_reboot
    OCUDU_PREFLIGHT_DENY(reboot);
#endif
#undef OCUDU_PREFLIGHT_DENY
    append_instruction(filter, BPF_RET | BPF_K, 0, 0, SECCOMP_RET_ALLOW);
    sock_fprog program{static_cast<unsigned short>(filter.size()), filter.data()};
    return ::syscall(SYS_seccomp, SECCOMP_SET_MODE_FILTER, 0, &program) == 0;
  } catch (...) {
    return false;
  }
}

bool set_bounded_limit(int resource, rlim_t maximum) noexcept
{
  rlimit current{};
  if (::getrlimit(resource, &current) != 0) {
    return false;
  }
  const rlim_t bound = current.rlim_max == RLIM_INFINITY ? maximum : std::min(current.rlim_max, maximum);
  const rlimit limit{bound, bound};
  return ::setrlimit(resource, &limit) == 0;
}

bool harden() noexcept
{
  const pid_t parent = ::getppid();
  ::umask(0077);
  return parent > 1 && ::chdir("/") == 0 && set_bounded_limit(RLIMIT_CORE, 0) && set_bounded_limit(RLIMIT_FSIZE, 0) &&
         set_bounded_limit(RLIMIT_NOFILE, 256) && set_bounded_limit(RLIMIT_RTPRIO, 0) &&
         ::prctl(PR_SET_DUMPABLE, 0, 0, 0, 0) == 0 && ::prctl(PR_SET_PDEATHSIG, SIGKILL, 0, 0, 0) == 0 &&
         ::getppid() == parent && ::prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0) == 0 && install_seccomp_filter();
}

uint64_t monotonic_ns(void*) noexcept
{
  timespec now{};
  if (::clock_gettime(CLOCK_MONOTONIC, &now) != 0) {
    return 0;
  }
  return static_cast<uint64_t>(now.tv_sec) * 1'000'000'000ULL + static_cast<uint64_t>(now.tv_nsec);
}

void ignore_metric(void*, uint32_t, uint64_t) noexcept {}

void ignore_incident(void*, uint32_t, uint64_t) noexcept {}

ocudu_dapp_status_v1 reject_publication(void*, uint32_t, uint32_t, const void*, uint32_t) noexcept
{
  return OCUDU_DAPP_UNSUPPORTED_V1;
}

ocudu_dapp_status_v1 reject_extension(void*, uint32_t, uint16_t, const void** extension) noexcept
{
  if (extension != nullptr) {
    *extension = nullptr;
  }
  return OCUDU_DAPP_UNSUPPORTED_V1;
}

bool known_worker_interface(uint32_t interface_id) noexcept
{
  switch (interface_id) {
    case OCUDU_DAPP_INTERFACE_SPECTRUM_V1:
    case OCUDU_DAPP_INTERFACE_SRS_ISAC_V1:
    case OCUDU_DAPP_INTERFACE_CHANNEL_ESTIMATOR_V1:
    case OCUDU_DAPP_INTERFACE_SRS_CHANNEL_ESTIMATOR_V1:
    case OCUDU_DAPP_INTERFACE_EQUALIZER_V1:
    case OCUDU_DAPP_INTERFACE_RECEIVER_V1:
    case OCUDU_DAPP_INTERFACE_SCHEDULER_V1:
      return true;
    default:
      return false;
  }
}

ocudu_dapp_status_v1 invoke_invalid_vector(const ocudu_dapp_interface_descriptor_v1& descriptor, void* context)
{
  switch (descriptor.interface_id) {
    case OCUDU_DAPP_INTERFACE_SPECTRUM_V1:
      return static_cast<const ocudu_dapp_spectrum_interface_v1*>(descriptor.interface_vtable)
          ->publish(context, nullptr);
    case OCUDU_DAPP_INTERFACE_SRS_ISAC_V1:
      return static_cast<const ocudu_dapp_srs_isac_interface_v1*>(descriptor.interface_vtable)
          ->publish(context, nullptr);
    case OCUDU_DAPP_INTERFACE_CHANNEL_ESTIMATOR_V1:
      return static_cast<const ocudu_dapp_channel_estimator_interface_v1*>(descriptor.interface_vtable)
          ->invoke(context, nullptr, nullptr);
    case OCUDU_DAPP_INTERFACE_SRS_CHANNEL_ESTIMATOR_V1:
      return static_cast<const ocudu_dapp_srs_channel_estimator_interface_v1*>(descriptor.interface_vtable)
          ->invoke(context, nullptr, nullptr);
    case OCUDU_DAPP_INTERFACE_EQUALIZER_V1:
      return static_cast<const ocudu_dapp_equalizer_interface_v1*>(descriptor.interface_vtable)
          ->invoke(context, nullptr, nullptr);
    case OCUDU_DAPP_INTERFACE_RECEIVER_V1:
      return static_cast<const ocudu_dapp_receiver_interface_v1*>(descriptor.interface_vtable)
          ->invoke(context, nullptr, nullptr);
    case OCUDU_DAPP_INTERFACE_SCHEDULER_V1:
      return static_cast<const ocudu_dapp_scheduler_interface_v1*>(descriptor.interface_vtable)
          ->invoke(context, nullptr, nullptr);
    default:
      return OCUDU_DAPP_UNSUPPORTED_V1;
  }
}

void cleanup(const ocudu_dapp_module_v1& module, void* instance, bool started) noexcept
{
  if (instance == nullptr) {
    return;
  }
  if (started) {
    try {
      static_cast<void>(module.stop(instance));
    } catch (...) {
    }
  }
  try {
    static_cast<void>(module.quiesce(instance, monotonic_ns(nullptr) + 100'000'000ULL));
  } catch (...) {
  }
  try {
    module.destroy(instance);
  } catch (...) {
  }
}

native_preflight_stage probe_lifecycle(const ocudu_dapp_module_v1& module, uint64_t capabilities) noexcept
{
  ocudu_dapp_host_api_v1 host{};
  host.struct_size      = sizeof(host);
  host.abi_major        = OCUDU_DAPP_ABI_MAJOR_V1;
  host.abi_minor        = OCUDU_DAPP_ABI_MINOR_V1;
  host.capabilities     = capabilities;
  host.monotonic_ns     = monotonic_ns;
  host.metric_add       = ignore_metric;
  host.incident_token   = ignore_incident;
  host.publish_prepared = reject_publication;
  host.query_extension  = reject_extension;

  void* instance = nullptr;
  try {
    const auto status = module.create(&host, nullptr, &instance);
    if (status != OCUDU_DAPP_OK_V1 || instance == nullptr) {
      cleanup(module, instance, false);
      return native_preflight_stage::create_rejected;
    }
  } catch (...) {
    cleanup(module, instance, false);
    return native_preflight_stage::create_threw;
  }

  try {
    if (module.configure(instance, nullptr) != OCUDU_DAPP_OK_V1) {
      cleanup(module, instance, false);
      return native_preflight_stage::configure_rejected;
    }
  } catch (...) {
    cleanup(module, instance, false);
    return native_preflight_stage::configure_threw;
  }
  try {
    if (module.prepare(instance, nullptr) != OCUDU_DAPP_OK_V1) {
      cleanup(module, instance, false);
      return native_preflight_stage::prepare_rejected;
    }
  } catch (...) {
    cleanup(module, instance, false);
    return native_preflight_stage::prepare_threw;
  }
  if (module.warmup != nullptr) {
    try {
      if (module.warmup(instance, nullptr) != OCUDU_DAPP_OK_V1) {
        cleanup(module, instance, false);
        return native_preflight_stage::warmup_rejected;
      }
    } catch (...) {
      cleanup(module, instance, false);
      return native_preflight_stage::warmup_threw;
    }
  }
  try {
    if (module.start(instance) != OCUDU_DAPP_OK_V1) {
      cleanup(module, instance, false);
      return native_preflight_stage::start_rejected;
    }
  } catch (...) {
    cleanup(module, instance, false);
    return native_preflight_stage::start_threw;
  }

  for (uint32_t index = 0; index != module.nof_interfaces; ++index) {
    const auto& descriptor = module.interfaces[index];
    if (!known_worker_interface(descriptor.interface_id)) {
      continue;
    }
    const auto* worker = static_cast<const ocudu_dapp_worker_api_v1*>(descriptor.interface_vtable);
    for (uint32_t worker_id = 0; worker_id != 2; ++worker_id) {
      void* context = nullptr;
      try {
        if (worker->create_worker_context(instance, worker_id, &context) != OCUDU_DAPP_OK_V1 || context == nullptr) {
          cleanup(module, instance, true);
          return native_preflight_stage::worker_create_rejected;
        }
      } catch (...) {
        cleanup(module, instance, true);
        return native_preflight_stage::worker_create_threw;
      }
      try {
        if (invoke_invalid_vector(descriptor, context) == OCUDU_DAPP_OK_V1) {
          worker->destroy_worker_context(instance, context);
          cleanup(module, instance, true);
          return native_preflight_stage::invalid_vector_accepted;
        }
      } catch (...) {
        try {
          worker->destroy_worker_context(instance, context);
        } catch (...) {
        }
        cleanup(module, instance, true);
        return native_preflight_stage::invalid_vector_threw;
      }
      try {
        worker->destroy_worker_context(instance, context);
      } catch (...) {
        cleanup(module, instance, true);
        return native_preflight_stage::worker_destroy_threw;
      }
    }
  }

  try {
    if (module.stop(instance) != OCUDU_DAPP_OK_V1) {
      cleanup(module, instance, false);
      return native_preflight_stage::stop_rejected;
    }
  } catch (...) {
    cleanup(module, instance, false);
    return native_preflight_stage::stop_threw;
  }
  try {
    if (module.quiesce(instance, monotonic_ns(nullptr) + 100'000'000ULL) != OCUDU_DAPP_OK_V1) {
      cleanup(module, instance, false);
      return native_preflight_stage::quiesce_rejected;
    }
  } catch (...) {
    cleanup(module, instance, false);
    return native_preflight_stage::quiesce_threw;
  }
  try {
    module.destroy(instance);
  } catch (...) {
    return native_preflight_stage::destroy_threw;
  }
  return native_preflight_stage::success;
}

} // namespace

int main(int argc, char** argv)
{
  if (argc != 5 || !harden()) {
    return exit_code(argc == 5 ? native_preflight_stage::sandbox_failed : native_preflight_stage::invalid_arguments);
  }
  uint64_t                descriptor{};
  ocudu_dapp_host_info_v1 host{};
  host.struct_size = sizeof(host);
  host.abi_major   = OCUDU_DAPP_ABI_MAJOR_V1;
  host.abi_minor   = OCUDU_DAPP_ABI_MINOR_V1;
  if (!parse_u64(argv[1], descriptor) || descriptor > static_cast<uint64_t>(INT32_MAX) ||
      !parse_u64(argv[2], host.host_capabilities) || !parse_u64(argv[3], host.backend_mask) ||
      !parse_build_id(argv[4], host.host_build_id) || ::fcntl(static_cast<int>(descriptor), F_GETFD) < 0) {
    return exit_code(native_preflight_stage::invalid_arguments);
  }

  const std::string path   = "/proc/self/fd/" + std::to_string(descriptor);
  void*             handle = ::dlopen(path.c_str(), RTLD_NOW | RTLD_LOCAL);
  if (handle == nullptr) {
    return exit_code(native_preflight_stage::dynamic_load_failed);
  }
  struct handle_guard {
    void* handle;
    ~handle_guard() { ::dlclose(handle); }
  } guard{handle};

  ::dlerror();
  void* symbol = ::dlsym(handle, "ocudu_dapp_query_v1");
  if (::dlerror() != nullptr || symbol == nullptr) {
    return exit_code(native_preflight_stage::query_symbol_missing);
  }
  ocudu_dapp_query_v1_fn query = nullptr;
  static_assert(sizeof(query) == sizeof(symbol));
  std::memcpy(&query, &symbol, sizeof(query));

  ocudu_dapp_module_v1 module{};
  module.struct_size = sizeof(module);
  module.abi_major   = OCUDU_DAPP_ABI_MAJOR_V1;
  module.abi_minor   = OCUDU_DAPP_ABI_MINOR_V1;
  ocudu_dapp_status_v1 status{};
  try {
    status = query(&host, &module);
  } catch (...) {
    return exit_code(native_preflight_stage::query_threw);
  }
  if (status != OCUDU_DAPP_OK_V1) {
    return exit_code(native_preflight_stage::query_rejected);
  }
  if (ocudu::dapp::validate_module_descriptor(&module) != ocudu::dapp::validation_error::none) {
    return exit_code(native_preflight_stage::descriptor_invalid);
  }
  return exit_code(probe_lifecycle(module, host.host_capabilities));
}
