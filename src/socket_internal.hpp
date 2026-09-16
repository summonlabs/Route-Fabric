// Internal socket helpers. Not part of the installed public API.
#ifndef ROUTEFABRIC_SOCKET_INTERNAL_HPP
#define ROUTEFABRIC_SOCKET_INTERNAL_HPP

#include <cstdint>
#include <cstring>
#include <mutex>
#include <string>

#include "routefabric/error.hpp"

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
// The Windows SDK's WSAGetIPUserMtu inline wrapper reports a C6101 "returning
// uninitialized memory" finding inside the platform header itself. The
// suppression is scoped to that header alone; no first-party diagnostic is
// disabled.
#pragma warning(push)
#pragma warning(disable : 6101)
#include <ws2tcpip.h>
#pragma warning(pop)
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

namespace routefabric {
namespace detail {

#ifdef _WIN32
using SocketHandle = SOCKET;
constexpr SocketHandle kInvalidSocket = INVALID_SOCKET;
#else
using SocketHandle = int;
constexpr SocketHandle kInvalidSocket = -1;
#endif

// Initializes the platform socket runtime exactly once. The runtime is released
// when the process exits; sockets are always closed by their owning objects.
inline bool socket_runtime_start(std::string& why) {
#ifdef _WIN32
  static std::once_flag once;
  static bool started = false;
  std::call_once(once, []() {
    WSADATA data{};
    started = ::WSAStartup(MAKEWORD(2, 2), &data) == 0;
  });
  if (!started) {
    why = "WSAStartup failed";
    return false;
  }
#else
  (void)why;
#endif
  return true;
}

inline void close_socket(SocketHandle handle) {
  if (handle == kInvalidSocket) {
    return;
  }
#ifdef _WIN32
  ::closesocket(handle);
#else
  ::close(handle);
#endif
}

// Unblocks a blocking recv or accept on another thread.
inline void shutdown_socket(SocketHandle handle) {
  if (handle == kInvalidSocket) {
    return;
  }
#ifdef _WIN32
  ::shutdown(handle, SD_BOTH);
#else
  ::shutdown(handle, SHUT_RDWR);
#endif
}

inline bool send_all(SocketHandle handle, const std::uint8_t* data, std::size_t size, std::string& why) {
  std::size_t sent = 0;
  while (sent < size) {
    const std::size_t chunk = size - sent;
#ifdef _WIN32
    const int request = static_cast<int>(chunk > 1u << 20 ? 1u << 20 : chunk);
    const int result = ::send(handle, reinterpret_cast<const char*>(data + sent), request, 0);
#else
    const int request = static_cast<int>(chunk > 1u << 20 ? 1u << 20 : chunk);
    const int result = static_cast<int>(::send(handle, data + sent, static_cast<std::size_t>(request), MSG_NOSIGNAL));
#endif
    if (result <= 0) {
      why = "peer closed the connection while sending";
      return false;
    }
    sent += static_cast<std::size_t>(result);
  }
  return true;
}

enum class ReceiveStatus {
  Data = 1,
  Closed = 2,
  WouldBlock = 3,
  Failed = 4,
};

// Non-blocking receive of whatever is available. Every outcome is explicit: a
// closed peer, an error, or "no data right now".
inline ReceiveStatus receive_some(SocketHandle handle, std::uint8_t* data, std::size_t capacity,
                                  std::size_t& received, std::string& why) {
  received = 0;
  if (capacity == 0) {
    return ReceiveStatus::Data;
  }
#ifdef _WIN32
  const int request = static_cast<int>(capacity > 1u << 20 ? 1u << 20 : capacity);
  const int result = ::recv(handle, reinterpret_cast<char*>(data), request, 0);
  if (result == 0) {
    why = "peer closed the connection";
    return ReceiveStatus::Closed;
  }
  if (result == SOCKET_ERROR) {
    const int error = ::WSAGetLastError();
    if (error == WSAEWOULDBLOCK) {
      return ReceiveStatus::WouldBlock;
    }
    why = "socket receive failed";
    return ReceiveStatus::Failed;
  }
#else
  const int request = static_cast<int>(capacity > 1u << 20 ? 1u << 20 : capacity);
  const ssize_t result = ::recv(handle, data, static_cast<std::size_t>(request), 0);
  if (result == 0) {
    why = "peer closed the connection";
    return ReceiveStatus::Closed;
  }
  if (result < 0) {
    if (errno == EAGAIN || errno == EWOULDBLOCK) {
      return ReceiveStatus::WouldBlock;
    }
    why = "socket receive failed";
    return ReceiveStatus::Failed;
  }
#endif
  received = static_cast<std::size_t>(result);
  return ReceiveStatus::Data;
}

// Event-driven socket waiter.
//
// A blocking recv() on Windows is not reliably cancelled by shutdown() from
// another thread, so a session that is waiting for a frame would pin its thread
// and block an orderly shutdown. This waiter instead waits on the socket and on
// an explicit shutdown signal, which makes session termination deterministic on
// every path. Waiting on a socket also puts it in non-blocking mode, so the
// receive path always makes bounded progress and can never block indefinitely.
class SocketWaiter {
 public:
  SocketWaiter() = default;
  ~SocketWaiter() { close(); }

  SocketWaiter(const SocketWaiter&) = delete;
  SocketWaiter& operator=(const SocketWaiter&) = delete;

  bool create(SocketHandle socket, std::string& why) {
    socket_ = socket;
#ifdef _WIN32
    socket_event_ = ::WSACreateEvent();
    shutdown_event_ = ::WSACreateEvent();
    if (socket_event_ == WSA_INVALID_EVENT || shutdown_event_ == WSA_INVALID_EVENT) {
      why = "cannot create socket wait events";
      close();
      return false;
    }
    if (::WSAEventSelect(socket_, socket_event_, FD_READ | FD_CLOSE) == SOCKET_ERROR) {
      why = "cannot associate the socket with its wait event";
      close();
      return false;
    }
    return true;
#else
    if (::pipe(pipe_) != 0) {
      why = "cannot create the shutdown pipe";
      return false;
    }
    return true;
#endif
  }

  void request_shutdown() {
#ifdef _WIN32
    if (shutdown_event_ != WSA_INVALID_EVENT) {
      ::WSASetEvent(shutdown_event_);
    }
#else
    if (pipe_[1] >= 0) {
      const char byte = 1;
      const ssize_t ignored = ::write(pipe_[1], &byte, 1);
      (void)ignored;
    }
#endif
  }

  enum class WaitStatus {
    Readable = 1,
    ShutdownRequested = 2,
    Failed = 3,
  };

  WaitStatus wait(std::string& why) {
#ifdef _WIN32
    if (socket_event_ == WSA_INVALID_EVENT || shutdown_event_ == WSA_INVALID_EVENT) {
      why = "the socket waiter is not initialized";
      return WaitStatus::Failed;
    }
    const HANDLE events[2] = {shutdown_event_, socket_event_};
    const DWORD index = ::WSAWaitForMultipleEvents(2, events, FALSE, WSA_INFINITE, FALSE);
    if (index == WSA_WAIT_EVENT_0) {
      return WaitStatus::ShutdownRequested;
    }
    if (index == WSA_WAIT_EVENT_0 + 1) {
      WSANETWORKEVENTS network_events{};
      if (::WSAEnumNetworkEvents(socket_, socket_event_, &network_events) == SOCKET_ERROR) {
        why = "cannot read socket network events";
        return WaitStatus::Failed;
      }
      if ((network_events.lNetworkEvents & (FD_READ | FD_CLOSE)) != 0) {
        return WaitStatus::Readable;
      }
      return WaitStatus::Readable;  // re-evaluate the socket state
    }
    why = "socket wait failed";
    return WaitStatus::Failed;
#else
    fd_set read_set;
    FD_ZERO(&read_set);
    FD_SET(socket_, &read_set);
    FD_SET(pipe_[0], &read_set);
    const int highest = socket_ > pipe_[0] ? socket_ : pipe_[0];
    if (::select(highest + 1, &read_set, nullptr, nullptr, nullptr) < 0) {
      why = "socket select failed";
      return WaitStatus::Failed;
    }
    if (FD_ISSET(pipe_[0], &read_set)) {
      char byte = 0;
      const ssize_t ignored = ::read(pipe_[0], &byte, 1);
      (void)ignored;
      return WaitStatus::ShutdownRequested;
    }
    return WaitStatus::Readable;
#endif
  }

  void close() {
#ifdef _WIN32
    if (socket_ != kInvalidSocket && socket_event_ != WSA_INVALID_EVENT) {
      (void)::WSAEventSelect(socket_, nullptr, 0);
    }
    if (socket_event_ != WSA_INVALID_EVENT) {
      ::WSACloseEvent(socket_event_);
      socket_event_ = WSA_INVALID_EVENT;
    }
    if (shutdown_event_ != WSA_INVALID_EVENT) {
      ::WSACloseEvent(shutdown_event_);
      shutdown_event_ = WSA_INVALID_EVENT;
    }
#else
    if (pipe_[0] >= 0) {
      ::close(pipe_[0]);
      ::close(pipe_[1]);
      pipe_[0] = -1;
      pipe_[1] = -1;
    }
#endif
    socket_ = kInvalidSocket;
  }

 private:
  SocketHandle socket_ = kInvalidSocket;
#ifdef _WIN32
  WSAEVENT socket_event_ = WSA_INVALID_EVENT;
  WSAEVENT shutdown_event_ = WSA_INVALID_EVENT;
#else
  int pipe_[2] = {-1, -1};
#endif
};

// Blocking receive of exactly the requested bytes, used by the client where the
// caller owns the socket for the whole exchange.
inline bool recv_exact(SocketHandle handle, std::uint8_t* data, std::size_t size, std::string& why) {
  std::size_t received = 0;
  while (received < size) {
    const std::size_t chunk = size - received;
#ifdef _WIN32
    const int request = static_cast<int>(chunk > 1u << 20 ? 1u << 20 : chunk);
    const int result = ::recv(handle, reinterpret_cast<char*>(data + received), request, 0);
#else
    const int request = static_cast<int>(chunk > 1u << 20 ? 1u << 20 : chunk);
    const int result = static_cast<int>(::recv(handle, data + received, static_cast<std::size_t>(request), 0));
#endif
    if (result == 0) {
      why = "peer closed the connection";
      return false;
    }
    if (result < 0) {
      why = "socket receive failed";
      return false;
    }
    received += static_cast<std::size_t>(result);
  }
  return true;
}

inline bool create_tcp_socket(const std::string& host, SocketHandle& out, bool& ipv6, std::string& why) {
  std::string start_why;
  if (!socket_runtime_start(start_why)) {
    why = start_why;
    return false;
  }
  in6_addr address6{};
  in_addr address4{};
  const bool is_v6 = ::inet_pton(AF_INET6, host.c_str(), &address6) == 1;
  if (!is_v6 && ::inet_pton(AF_INET, host.c_str(), &address4) != 1) {
    why = "bind address must be a literal IPv4 or IPv6 address";
    return false;
  }
  ipv6 = is_v6;
  const int family = is_v6 ? AF_INET6 : AF_INET;
  const SocketHandle handle = ::socket(family, SOCK_STREAM, IPPROTO_TCP);
  if (handle == kInvalidSocket) {
    why = "cannot create a TCP socket";
    return false;
  }
  int reuse = 1;
  ::setsockopt(handle, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&reuse), sizeof(reuse));
  out = handle;
  return true;
}

inline bool bind_and_listen(SocketHandle handle, const std::string& host, std::uint16_t port, bool ipv6,
                            std::uint16_t& bound_port, std::string& why) {
  if (ipv6) {
    sockaddr_in6 address{};
    address.sin6_family = AF_INET6;
    address.sin6_port = htons(port);
    ::inet_pton(AF_INET6, host.c_str(), &address.sin6_addr);
    if (::bind(handle, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0) {
      why = "cannot bind the listener";
      return false;
    }
  } else {
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = htons(port);
    ::inet_pton(AF_INET, host.c_str(), &address.sin_addr);
    if (::bind(handle, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0) {
      why = "cannot bind the listener";
      return false;
    }
  }
  if (::listen(handle, SOMAXCONN) != 0) {
    why = "cannot listen on the socket";
    return false;
  }
  sockaddr_storage local{};
  int local_size = static_cast<int>(sizeof(local));
  if (::getsockname(handle, reinterpret_cast<sockaddr*>(&local), &local_size) != 0) {
    why = "cannot determine the bound port";
    return false;
  }
  if (local.ss_family == AF_INET6) {
    bound_port = ntohs(reinterpret_cast<sockaddr_in6*>(&local)->sin6_port);
  } else {
    bound_port = ntohs(reinterpret_cast<sockaddr_in*>(&local)->sin_port);
  }
  return true;
}

inline bool connect_socket(SocketHandle handle, const std::string& host, std::uint16_t port, bool ipv6,
                           std::string& why) {
  if (ipv6) {
    sockaddr_in6 address{};
    address.sin6_family = AF_INET6;
    address.sin6_port = htons(port);
    ::inet_pton(AF_INET6, host.c_str(), &address.sin6_addr);
    if (::connect(handle, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0) {
      why = "cannot connect to the coordinator";
      return false;
    }
    return true;
  }
  sockaddr_in address{};
  address.sin_family = AF_INET;
  address.sin_port = htons(port);
  ::inet_pton(AF_INET, host.c_str(), &address.sin_addr);
  if (::connect(handle, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0) {
    why = "cannot connect to the coordinator";
    return false;
  }
  return true;
}

}  // namespace detail
}  // namespace routefabric

#endif  // ROUTEFABRIC_SOCKET_INTERNAL_HPP
