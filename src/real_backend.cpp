#include "routefabric/real_backend.hpp"

#include "routefabric/hash.hpp"

#ifdef _WIN32
#include <winsock2.h>
// See src/socket_internal.hpp: the platform header itself triggers the analyzer;
// the suppression is scoped to that header only.
#pragma warning(push)
#pragma warning(disable : 6101)
#include <ws2tcpip.h>
#include <iphlpapi.h>
#pragma warning(pop)
#endif

namespace routefabric {
namespace {

std::string format_address(const void* address, int family) {
  char buffer[INET6_ADDRSTRLEN] = {};
  if (family == AF_INET) {
    if (::inet_ntop(AF_INET, address, buffer, static_cast<socklen_t>(sizeof(buffer))) == nullptr) {
      return std::string();
    }
    return std::string(buffer);
  }
  if (family == AF_INET6) {
    if (::inet_ntop(AF_INET6, address, buffer, static_cast<socklen_t>(sizeof(buffer))) == nullptr) {
      return std::string();
    }
    return std::string(buffer);
  }
  return std::string();
}

NextHopId derive_next_hop_id(std::string_view gateway) {
  const Digest128 digest = digest128("routefabric.host-route.next-hop", gateway);
  return NextHopId::from_digest(digest);
}

ProgrammingDispatch not_supported(const RouteProgrammingRequest& request, const BackendId& backend) {
  ProgrammingDispatch dispatch;
  dispatch.deferred = false;
  dispatch.immediate.attempt = request.attempt;
  dispatch.immediate.backend = backend;
  dispatch.immediate.outcome = ProgrammingOutcome::NotSupported;
  dispatch.immediate.detail = "the host routing table adapter is read-only: route programming is not supported";
  return dispatch;
}

}  // namespace

std::string HostRouteEntry::render() const {
  std::string out = destination.render();
  out += " -> ";
  out += gateway.empty() ? "on-link" : gateway;
  out += " via ";
  out += interface_alias;
  out += " metric=";
  out += to_decimal(metric);
  return out;
}

struct WindowsRouteTableBackend::Impl {
  BackendId id{"windows-host-routing-table"};
  mutable std::string last_error;
};

WindowsRouteTableBackend::WindowsRouteTableBackend() : impl_(std::make_unique<Impl>()) {}

WindowsRouteTableBackend::~WindowsRouteTableBackend() = default;

BackendId WindowsRouteTableBackend::id() const { return impl_->id; }

BackendCapabilities WindowsRouteTableBackend::capabilities() const {
  BackendCapabilities capabilities;
  capabilities.query = true;
  capabilities.read_only = true;
  capabilities.install = false;
  capabilities.replace = false;
  capabilities.withdraw = false;
  return capabilities;
}

ProgrammingDispatch WindowsRouteTableBackend::InstallRoute(const RouteProgrammingRequest& request) {
  return not_supported(request, impl_->id);
}

ProgrammingDispatch WindowsRouteTableBackend::ReplaceRoute(const RouteProgrammingRequest& request) {
  return not_supported(request, impl_->id);
}

ProgrammingDispatch WindowsRouteTableBackend::WithdrawRoute(const RouteProgrammingRequest& request) {
  return not_supported(request, impl_->id);
}

std::vector<HostRouteEntry> WindowsRouteTableBackend::EnumerateHostRoutes() const {
  std::vector<HostRouteEntry> entries;
#ifdef _WIN32
  const ADDRESS_FAMILY families[2] = {AF_INET, AF_INET6};
  for (const ADDRESS_FAMILY family : families) {
    PMIB_IPFORWARD_TABLE2 table = nullptr;
    const NETIO_STATUS status = ::GetIpForwardTable2(family, &table);
    if (status != NO_ERROR || table == nullptr) {
      impl_->last_error = "GetIpForwardTable2 failed with status " + to_decimal(static_cast<std::uint64_t>(status));
      continue;
    }
    for (ULONG index = 0; index < table->NumEntries; ++index) {
      const MIB_IPFORWARD_ROW2& row = table->Table[index];
      HostRouteEntry entry;
      std::string address_text;
      if (family == AF_INET) {
        address_text = format_address(&row.DestinationPrefix.Prefix.Ipv4.sin_addr, AF_INET);
      } else {
        address_text = format_address(&row.DestinationPrefix.Prefix.Ipv6.sin6_addr, AF_INET6);
      }
      if (address_text.empty()) {
        continue;
      }
      const std::string text = address_text + "/" + to_decimal(row.DestinationPrefix.PrefixLength);
      const Expected<Destination> parsed =
          family == AF_INET ? Destination::parse_ipv4_prefix(text) : Destination::parse_ipv6_prefix(text);
      if (!parsed) {
        continue;
      }
      entry.destination = parsed.value();
      if (family == AF_INET) {
        entry.gateway = format_address(&row.NextHop.Ipv4.sin_addr, AF_INET);
      } else {
        entry.gateway = format_address(&row.NextHop.Ipv6.sin6_addr, AF_INET6);
      }
      entry.on_link = entry.gateway.empty() || entry.gateway == "0.0.0.0" || entry.gateway == "::";
      entry.next_hop = derive_next_hop_id(entry.gateway);
      entry.metric = row.Metric;
      char alias[IF_MAX_STRING_SIZE + 1] = {};
      if (::ConvertInterfaceLuidToNameA(&row.InterfaceLuid, alias, sizeof(alias)) == NO_ERROR) {
        entry.interface_alias = std::string(alias);
      }
      entries.push_back(std::move(entry));
    }
    ::FreeMibTable(table);
  }
  if (!entries.empty()) {
    impl_->last_error.clear();
  }
#else
  impl_->last_error = "host routing table enumeration is only implemented on Windows";
#endif
  return entries;
}

BackendQueryResult WindowsRouteTableBackend::QueryRoute(const RouteKey& key) {
  BackendQueryResult result;
  result.backend = impl_->id;
  if (!key.destination.is_prefix()) {
    result.presence = BackendPresence::Unknown;
    result.detail = "the host routing table has no representation for endpoint destinations";
    return result;
  }
  bool enumerated = false;
  for (const HostRouteEntry& entry : EnumerateHostRoutes()) {
    enumerated = true;
    if (entry.destination == key.destination) {
      result.presence = BackendPresence::Present;
      result.detail = entry.render();
      return result;
    }
  }
  if (!enumerated && !impl_->last_error.empty()) {
    result.presence = BackendPresence::Unavailable;
    result.detail = impl_->last_error;
    return result;
  }
  result.presence = BackendPresence::Absent;
  result.detail = "no matching entry in the host routing table";
  return result;
}

bool WindowsRouteTableBackend::is_supported() noexcept {
#ifdef _WIN32
  return true;
#else
  return false;
#endif
}

const std::string& WindowsRouteTableBackend::last_error() const { return impl_->last_error; }

}  // namespace routefabric
