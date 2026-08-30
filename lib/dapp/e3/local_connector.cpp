// SPDX-FileCopyrightText: Copyright (C) 2026 DeepSig Inc.
// SPDX-License-Identifier: BSD-3-Clause-Clear

#include "ocudu/dapp/e3/local_connector.h"
#include "local_transport_internal.h"
#include "ocudu/dapp/e3/management_service.h"
#include "ocudu/support/io/unique_fd.h"
#include <array>
#include <poll.h>
#include <stdexcept>
#include <sys/eventfd.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <thread>
#include <unistd.h>

using namespace ocudu::dapp::e3;
using namespace ocudu::dapp::e3::detail;

struct local_management_server::implementation {
  implementation(management_service& service_, local_server_config config_) :
    service(service_), config(std::move(config_))
  {
  }

  ~implementation() { remove_owned_socket(); }

  bool initialize(std::string& detail)
  {
    sockaddr_un address{};
    if (!make_address(config.socket_path, address, detail) || config.request_timeout.count() <= 0) {
      if (detail.empty()) {
        detail = "local management request timeout must be positive";
      }
      return false;
    }
    struct stat existing {};
    if (::lstat(config.socket_path.c_str(), &existing) == 0) {
      detail = "local management socket path already exists";
      return false;
    }
    if (errno != ENOENT) {
      detail = error_text("inspect local management socket path");
      return false;
    }

    listener = ocudu::unique_fd(::socket(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC | SOCK_NONBLOCK, 0));
    if (!listener.is_open()) {
      detail = error_text("create local management socket");
      return false;
    }
    if (::bind(listener.value(), reinterpret_cast<const sockaddr*>(&address), sizeof(address)) != 0) {
      detail = error_text("bind local management socket");
      return false;
    }
    bound = true;
    if (::lstat(config.socket_path.c_str(), &owned_path) != 0 || !S_ISSOCK(owned_path.st_mode) ||
        owned_path.st_uid != ::geteuid() || ::chmod(config.socket_path.c_str(), S_IRUSR | S_IWUSR) != 0) {
      detail = "local management socket ownership or permissions could not be secured";
      return false;
    }
    if (::listen(listener.value(), 8) != 0) {
      detail = error_text("listen on local management socket");
      return false;
    }
    stop_event = ocudu::unique_fd(::eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK));
    if (!stop_event.is_open()) {
      detail = error_text("create local management stop event");
      return false;
    }
    allowed_uid = config.allowed_uid.value_or(::geteuid());
    return true;
  }

  void start()
  {
    worker = std::thread([this] { run(); });
  }

  void stop() noexcept
  {
    if (!stopping.exchange(true, std::memory_order_acq_rel)) {
      uint64_t signal = 1;
      ssize_t  written;
      do {
        written = ::write(stop_event.value(), &signal, sizeof(signal));
      } while (written < 0 && errno == EINTR);
      const bool notified = written == static_cast<ssize_t>(sizeof(signal)) || (written < 0 && errno == EAGAIN);
      if (!notified) {
        failures.fetch_add(1, std::memory_order_relaxed);
        // Wake poll through its other descriptor if the eventfd invariant was violated.
        static_cast<void>(::shutdown(listener.value(), SHUT_RDWR));
      }
    }
    if (worker.joinable()) {
      worker.join();
    }
  }

  local_server_metrics snapshot() const noexcept
  {
    return {accepted.load(std::memory_order_relaxed),
            rejected.load(std::memory_order_relaxed),
            malformed.load(std::memory_order_relaxed),
            completed.load(std::memory_order_relaxed),
            failures.load(std::memory_order_relaxed)};
  }

private:
  void run() noexcept
  {
    std::array<pollfd, 2> descriptors{{{listener.value(), POLLIN, 0}, {stop_event.value(), POLLIN, 0}}};
    while (true) {
      const int ready = ::poll(descriptors.data(), descriptors.size(), -1);
      if (ready < 0) {
        if (errno == EINTR) {
          continue;
        }
        failures.fetch_add(1, std::memory_order_relaxed);
        return;
      }
      if ((descriptors[1].revents & POLLIN) != 0) {
        return;
      }
      if ((descriptors[0].revents & POLLIN) == 0) {
        if ((descriptors[0].revents & (POLLERR | POLLHUP | POLLNVAL)) != 0) {
          failures.fetch_add(1, std::memory_order_relaxed);
          return;
        }
        continue;
      }
      while (true) {
        ocudu::unique_fd peer(::accept4(listener.value(), nullptr, nullptr, SOCK_CLOEXEC));
        if (!peer.is_open()) {
          if (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR) {
            failures.fetch_add(1, std::memory_order_relaxed);
          }
          break;
        }
        accepted.fetch_add(1, std::memory_order_relaxed);
        serve(peer.value());
      }
    }
  }

  void serve(int descriptor) noexcept
  {
    ucred     credentials{};
    socklen_t credential_size = sizeof(credentials);
    if (::getsockopt(descriptor, SOL_SOCKET, SO_PEERCRED, &credentials, &credential_size) != 0 ||
        credential_size != sizeof(credentials) || credentials.uid != allowed_uid) {
      rejected.fetch_add(1, std::memory_order_relaxed);
      return;
    }
    if (!set_timeout(descriptor, config.request_timeout)) {
      failures.fetch_add(1, std::memory_order_relaxed);
      return;
    }
    try {
      auto received = receive_message(descriptor);
      if (!received) {
        malformed.fetch_add(1, std::memory_order_relaxed);
        return;
      }
      auto request = aper_codec::decode_request(*received.value);
      if (!request) {
        malformed.fetch_add(1, std::memory_order_relaxed);
        return;
      }
      auto response = aper_codec::encode(service.handle(*request.value));
      if (!response || !send_message(descriptor, *response.value)) {
        failures.fetch_add(1, std::memory_order_relaxed);
        return;
      }
      completed.fetch_add(1, std::memory_order_relaxed);
    } catch (...) {
      failures.fetch_add(1, std::memory_order_relaxed);
    }
  }

  void remove_owned_socket() noexcept
  {
    if (!bound) {
      return;
    }
    struct stat current {};
    if (::lstat(config.socket_path.c_str(), &current) == 0 && current.st_dev == owned_path.st_dev &&
        current.st_ino == owned_path.st_ino) {
      static_cast<void>(::unlink(config.socket_path.c_str()));
    }
    bound = false;
  }

  management_service&   service;
  local_server_config   config;
  ocudu::unique_fd      listener;
  ocudu::unique_fd      stop_event;
  uid_t                 allowed_uid{};
  struct stat           owned_path {};
  bool                  bound{};
  std::atomic<bool>     stopping{false};
  std::thread           worker;
  std::atomic<uint64_t> accepted{0};
  std::atomic<uint64_t> rejected{0};
  std::atomic<uint64_t> malformed{0};
  std::atomic<uint64_t> completed{0};
  std::atomic<uint64_t> failures{0};
};

local_management_server::local_management_server(std::unique_ptr<implementation> state_) : state(std::move(state_)) {}

local_server_create_result local_management_server::create(management_service& service,
                                                           local_server_config config) noexcept
{
  try {
    auto        state = std::make_unique<implementation>(service, std::move(config));
    std::string detail;
    if (!state->initialize(detail)) {
      return {nullptr, std::move(detail)};
    }
    auto result = std::unique_ptr<local_management_server>(new local_management_server(std::move(state)));
    result->state->start();
    return {std::move(result), {}};
  } catch (const std::exception& exception) {
    return {nullptr, exception.what()};
  } catch (...) {
    return {nullptr, "unknown local management server construction failure"};
  }
}

local_management_server::~local_management_server()
{
  stop();
}

void local_management_server::stop() noexcept
{
  if (state != nullptr) {
    state->stop();
  }
}

local_server_metrics local_management_server::metrics() const noexcept
{
  return state != nullptr ? state->snapshot() : local_server_metrics{};
}
