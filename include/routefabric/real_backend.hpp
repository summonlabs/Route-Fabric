#ifndef ROUTEFABRIC_REAL_BACKEND_HPP
#define ROUTEFABRIC_REAL_BACKEND_HPP

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "routefabric/backend.hpp"
#include "routefabric/destination.hpp"

namespace routefabric {

// One normalized entry of the real host routing table.
struct HostRouteEntry {
  Destination destination;
  std::string gateway;
  NextHopId next_hop;
  std::string interface_alias;
  std::uint32_t metric = 0;
  bool on_link = false;

  std::string render() const;
};

// REAL, read-only adapter over the host routing table (Windows IP Helper
// GetIpForwardTable2).
//
// This backend enumerates and queries real host routes. It is physically
// read-only: InstallRoute, ReplaceRoute and WithdrawRoute return NotSupported,
// and Route Fabric never mutates the host routing table. Programming semantics
// are proven against the SYNTHETIC backend instead.
class WindowsRouteTableBackend final : public IRouteProgrammingBackend {
 public:
  WindowsRouteTableBackend();
  ~WindowsRouteTableBackend() override;

  WindowsRouteTableBackend(const WindowsRouteTableBackend&) = delete;
  WindowsRouteTableBackend& operator=(const WindowsRouteTableBackend&) = delete;

  BackendId id() const override;
  BackendCapabilities capabilities() const override;

  ProgrammingDispatch InstallRoute(const RouteProgrammingRequest& request) override;
  ProgrammingDispatch ReplaceRoute(const RouteProgrammingRequest& request) override;
  ProgrammingDispatch WithdrawRoute(const RouteProgrammingRequest& request) override;
  BackendQueryResult QueryRoute(const RouteKey& key) override;

  // Real host route enumeration (IPv4 and IPv6) used as operator evidence.
  std::vector<HostRouteEntry> EnumerateHostRoutes() const;

  // True when the host routing table can be read on this platform.
  static bool is_supported() noexcept;

  // Set when the last enumeration failed; empty on success.
  const std::string& last_error() const;

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace routefabric

#endif  // ROUTEFABRIC_REAL_BACKEND_HPP
