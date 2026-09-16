#include "routefabric/limits.hpp"

namespace routefabric {

Status Limits::validate() const {
  if (max_routes == 0) {
    return make_error(StatusCode::InvalidArgument, "max_routes must be greater than zero");
  }
  if (max_routing_namespaces == 0) {
    return make_error(StatusCode::InvalidArgument, "max_routing_namespaces must be greater than zero");
  }
  if (max_frame_bytes < 64) {
    return make_error(StatusCode::InvalidArgument, "max_frame_bytes must be at least 64");
  }
  if (max_destination_chars == 0) {
    return make_error(StatusCode::InvalidArgument, "max_destination_chars must be greater than zero");
  }
  if (max_history_entries_per_route == 0) {
    return make_error(StatusCode::InvalidArgument, "max_history_entries_per_route must be greater than zero");
  }
  if (max_outstanding_programming == 0) {
    return make_error(StatusCode::InvalidArgument, "max_outstanding_programming must be greater than zero");
  }
  if (max_publishers == 0) {
    return make_error(StatusCode::InvalidArgument, "max_publishers must be greater than zero");
  }
  if (max_sessions == 0) {
    return make_error(StatusCode::InvalidArgument, "max_sessions must be greater than zero");
  }
  if (max_snapshot_routes == 0) {
    return make_error(StatusCode::InvalidArgument, "max_snapshot_routes must be greater than zero");
  }
  if (max_batch_mutations == 0) {
    return make_error(StatusCode::InvalidArgument, "max_batch_mutations must be greater than zero");
  }
  if (max_publisher_scopes == 0) {
    return make_error(StatusCode::InvalidArgument, "max_publisher_scopes must be greater than zero");
  }
  if (max_persist_bytes < 1024) {
    return make_error(StatusCode::InvalidArgument, "max_persist_bytes must be at least 1024");
  }
  return ok_status();
}

}  // namespace routefabric
