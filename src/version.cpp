#include "routefabric/version.hpp"

#include "routefabric/hash.hpp"
#include "routefabric/ids.hpp"
#include "routefabric/limits.hpp"

// This translation unit anchors the generated version header into the library
// so that a downstream consumer can always read the exact version it linked
// against.

namespace routefabric {

static_assert(kVersionMajor == 1, "Route Fabric 1.x version contract");

}  // namespace routefabric
