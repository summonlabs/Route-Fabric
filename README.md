# Route Fabric

Route Fabric is the authoritative route-lifecycle and route-state runtime of the
Distributed Fabric Infrastructure / Fabric OS stack. It answers one question:

> What route is authoritative for this destination under the current
> control-plane epoch, which next-hop or authorized path does it bind to, what
> generation and provenance produced it, what has actually been installed or
> withdrawn, and when must the route be rejected, superseded, fenced, retired or
> revalidated?

Version 1.0.0. C++20, CMake, Windows-first with a portable core.

```text
RouteFabric 1.0.0
  library + coordinator + publisher worker + operator CLI
  exports   SummonSoftwareLabs::RouteFabric
  wire protocol 1      persistence format 1
```

## 1. Systems boundary

Route Fabric **owns**:

* route identity and route keys;
* destination identity and canonicalization;
* the route lifecycle and its transition table;
* route generations, route authority generations and invalidation watermarks;
* route provenance and authority binding;
* route publication, replacement, supersession, withdrawal, retirement and
  revocation;
* route currentness;
* route intent versus applied state;
* next-hop binding and Path Authority binding;
* route installation attempts and backend result classification;
* backend reconciliation evidence;
* conservative recovery;
* stale-route rejection, stale-worker fencing and stale-completion defence;
* deterministic snapshots, diffs, digests and explanations;
* versioned, integrity-checked persistence;
* the route programming backend interface;
* authoritative route queries.

Route Fabric **does not own** and does not implement:

* canonical device identity (Fabric Registry);
* topology construction (Fabric Topology);
* link health (Link State Fabric);
* port configuration (Port Fabric);
* capability truth (Fabric Capability Registry);
* failure-domain semantics (Failure Domain Registry);
* epoch issuance (Fabric Epoch);
* path computation, path legality, ECMP policy, weighted path selection,
  adaptive routing, convergence optimization, traffic engineering, admission
  control, bandwidth reservation, congestion control, queue and buffer
  allocation, packet scheduling.

Route Fabric never computes a path. It never searches topology for alternatives,
never computes a shortest path and never optimizes hop count. It consumes an
exact path identity and the exact Path Authority generation that authorized it.

### 1.1 Relation to Path Authority

Path Authority owns whether an exact supplied path is legally usable. Route
Fabric consumes that answer through ```IPathAuthority::Query``` and refuses to keep a
route installable when the answer is anything other than ```USABLE``` or when the
authorized generation is no longer current. A path-backed route stores both the
```PathId``` and the ```PathAuthorityGeneration``` it was published against. Two
consequences are enforced and tested:

* a stale path authority generation can never produce a ```CURRENT``` route;
* path invalidation invalidates only the routes that depend on that path.

The shipped path authority implementation is an in-memory/file-backed stand-in
labelled **SYNTHETIC**; no REAL Path Authority runtime is integrated or claimed.

### 1.2 Single authoritative coordinator

Route Fabric 1.0.0 has exactly one authoritative coordinator instance per
durable store. It is **not** a consensus system. A stale epoch can be rejected
once a newer epoch exists; nothing in this release prevents two independent
coordinators from being started against separate stores. No split-brain
exclusion, quorum or leader election is implemented or claimed.

### 1.3 Transport and authentication

The distributed protocol runs over plain TCP on the loopback interface by
default. Frames carry a CRC-32C integrity field covering the semantic header
fields and the payload; the persistence format carries a CRC-32C plus a 64-bit
content hash. Both are **non-cryptographic**: they detect corruption, they do
not authenticate a writer. No cryptographic authentication, no TLS, no
authorization token and no signature is implemented or claimed. Distributed
input is still treated as untrusted: every field is bounds checked, every enum
is validated, every frame length is bounded and every payload must be consumed
exactly.

## 2. Route identity

Identity types are strongly typed and are never interchangeable:

| Purpose | Type |
| --- | --- |
| Route identity | ```RouteId``` (128-bit, non-zero, derived from the route key) |
| Route snapshot | ```RouteSnapshotId``` (content addressed) |
| Mutation attempt | ```MutationAttemptId``` |
| Programming attempt | ```ProgrammingAttemptId``` |
| Worker boot | ```WorkerBootId``` |
| Path | ```PathId```, ```PathAuthorityGeneration``` |
| Next hop | ```NextHopId```, ```NextHopGroupId``` |
| Destination | ```DestinationId``` |
| Publisher / fabric / namespace | ```PublisherId```, ```FabricId```, ```RoutingNamespace``` |
| Counters | ```RouteGeneration```, ```RouteAuthorityGeneration```, ```CoordinatorEpoch```, ```PolicyGeneration```, ```ProgrammingGeneration``` |

Every identity rejects malformed encoding, rejects the zero value, renders
deterministically, encodes to a fixed byte form and orders deterministically.
Counter identities use checked arithmetic and never wrap: incrementing the
highest representable value returns an explicit ```StaleGeneration``` error.

### 2.1 Route key and uniqueness

```RouteKey = (fabric, routing namespace, destination, route class)```

Provenance, publisher, timestamps and generations are deliberately *not* part of
the key. Two publishers that target the same key collide through governance
instead of silently creating two identities. The runtime enforces **at most one
authoritative route per route key**; the only way a key changes hands is an
explicit administrative override, and a retired lineage can only be replaced by
an explicitly minted successor lineage.

### 2.2 Destination model

```DestinationKind``` covers IPv4 prefixes, IPv6 prefixes, fabric endpoints,
service endpoints, overlay endpoints and logical destinations. Prefix
destinations always carry an explicit prefix length, host bits below the prefix
are cleared (```10.0.0.1/24``` canonicalizes to ```10.0.0.0/24```), octets and prefix
lengths are strict decimal without leading zeros, IPv6 uses lowercase groups with
RFC 5952 zero compression, zone identifiers are rejected, and everything is
bounded. Endpoint destinations are bounded tokens from ```[A-Za-z0-9._-]```.

Route lookup is **exact route key lookup only**. Longest-prefix-match is
deliberately not implemented, because route selection policy is not Route
Fabric's boundary. Destination prefix containment exists solely for authority
scope coverage.

## 3. Route lifecycle

```text
DECLARED -> VALIDATING -> READY -> INSTALLING -> INSTALLED
                                  |              |
                                  |              +-> REVALIDATION_REQUIRED
                                  +-> WITHDRAWING -> WITHDRAWN
any -> FAILED | SUPERSEDED | RETIRED
```

The transition table is a static, exported table with exactly one destination
per (state, event) pair. It is verified exhaustively: every state crossed with
every event is asserted to be either allowed with the tabled destination or
rejected without mutating the state. A rejected event is never applied.

Backend outcomes map as follows:

| Backend outcome | Applied classification | Lifecycle |
| --- | --- | --- |
| APPLIED | APPLIED | INSTALLED |
| IDEMPOTENT | IDEMPOTENT_ALREADY_APPLIED | INSTALLED |
| REJECTED | REJECTED | FAILED |
| NOT_SUPPORTED | NOT_SUPPORTED | FAILED |
| PERMANENT_FAILURE | PERMANENT_FAILURE | FAILED |
| RETRYABLE_FAILURE | RETRYABLE_FAILURE | REVALIDATION_REQUIRED |
| AMBIGUOUS | AMBIGUOUS | REVALIDATION_REQUIRED |
| BACKEND_UNAVAILABLE | BACKEND_UNAVAILABLE | REVALIDATION_REQUIRED |
| withdrawal APPLIED/IDEMPOTENT | WITHDRAWN | WITHDRAWN |
| withdrawal failure | WITHDRAW_FAILED or AMBIGUOUS | REVALIDATION_REQUIRED |

## 4. Generations, authority generations and watermarks

* ```RouteGeneration``` advances when the *desired authority* of the record
  changes: publication, replacement, withdrawal, retirement, revocation,
  revalidation, path invalidation and epoch-driven invalidation.
* It does **not** advance for an exact replay, a read, an unchanged backend
  observation or a duplicated confirmation. Exact replay is idempotent and
  performs no backend work.
* ```RouteAuthorityGeneration``` advances whenever the authority binding of the
  record changes, independently of route content.
* ```ProgrammingGeneration``` is the watermark for backend dispatch. Attempts
  older than the current watermark can never be applied.
* ```InvalidationWatermark``` records the authority generation at which the
  route was invalidated, so a commit or completion started before an
  invalidation cannot land afterwards as current authority.

Provenance is structured, never a free-form string:
```publisher```, ```worker boot```, ```epoch```, ```source class```,
```source generation```, ```mutation attempt```, ```policy generation```,
```path authority generation```.

## 5. Desired state versus applied state

A committed desire is never reported as an installation.

* The authoritative intent is persisted **before** any backend call.
* The backing programming call is made with the runtime lock released.
* The applied outcome is persisted and only then returned to the caller.
* ```AppliedClassification``` distinguishes UNKNOWN, PENDING, APPLIED,
  IDEMPOTENT_ALREADY_APPLIED, REJECTED, NOT_SUPPORTED, RETRYABLE_FAILURE,
  PERMANENT_FAILURE, AMBIGUOUS, WITHDRAWN, WITHDRAW_FAILED and
  BACKEND_UNAVAILABLE.
* A deferred dispatch leaves the route in INSTALLING with a PENDING applied
  classification and currentness NOT_INSTALLED.

### 5.1 Stale completion defence

Every programming attempt carries its desired generation, programming generation
and epoch. A completion whose attempt is already resolved, whose generations no
longer match, whose epoch is stale, or whose route is retired, superseded or
revoked is classified STALE, its attempt is durably resolved and it never
modifies applied truth. This is tested deterministically for: a late install
after a replacement, a late withdrawal after a replacement, a path invalidation
racing an install, an epoch advance racing an install, retirement racing a
completion, and duplicate completions.

### 5.2 Crash windows and conservative recovery

The commit order is:

```text
validate authority -> validate expected generation -> validate path authority
-> prepare desired state -> persist authoritative intent -> program backend
-> persist applied outcome -> acknowledge
```

Because the intent is durable before programming and the outcome is durable
before acknowledgment, a crash at any boundary leaves either "no record of the
mutation" or "a record whose applied state is honestly unknown". After a restart:

* durable route intent survives;
* live publisher authority does not (registration is never durable);
* any unresolved programming attempt becomes AMBIGUOUS and its route becomes
  REVALIDATION_REQUIRED;
* nothing is inferred to be installed merely because a pre-crash record said so.

## 6. Publisher and epoch authority

Every mutation is bound to the current coordinator epoch, the registered
publisher, the exact worker boot, the publisher's scope, the expected route
generation where supplied, and a mutation attempt identity. A process being
connected is not enough, a publisher name being known is not enough, and a
durable record is not enough.

* Registration is explicit and scoped. The default-constructed scope authorizes
  nothing: each dimension (namespace, destination, route class) permits nothing
  unless entries are listed or the matching wildcard flag is explicitly set.
* A publisher that re-registers with a new worker boot fences the previous boot;
  the previous boot can never mutate again.
* Session end fences the boot that the session registered, but only if that boot
  is still the live registration.
* Administrative fencing is separate and persists until explicitly cleared.
* Advancing the epoch makes every existing registration stale; publishers must
  register again, and stale-epoch traffic is refused.

## 7. Replacement, supersession, withdrawal, retirement, revocation

* **Replacement** keeps the ```RouteId```, advances the generation and records
  the predecessor generation, the successor generation and the reason.
* **Supersession** happens when a retired or superseded lineage is replaced by an
  explicitly minted successor lineage with an administrative scope. The previous
  record becomes SUPERSEDED, loses the key, and remains inspectable by identity.
* **Withdrawal** is first-class: WITHDRAWING is distinct from WITHDRAWN, a failed
  withdrawal keeps the authoritative record and moves the route to
  REVALIDATION_REQUIRED, and repeated withdrawal is idempotent without a
  generation change.
* **Retirement** means the lineage is no longer usable. A retired route cannot
  accept a stale publication, become installed from a delayed completion, regain
  authority through reconnect or be silently recreated. Retirement is
  control-plane only: it does not program the backend. Use withdrawal to program
  removal; reconciliation reports the difference.
* **Revocation** is durable, generation-bound, reason-coded and absolute.
  Revocation is distinct from withdrawal, path invalidation, publisher fencing
  and backend failure, and a revoked key can never be recreated.

## 8. Route programming backend contract

```IRouteProgrammingBackend``` exposes ```InstallRoute```, ```ReplaceRoute```,
```WithdrawRoute``` and ```QueryRoute```. Results are structured; there is no
boolean anywhere in the contract. A call either resolves inline or is explicitly
deferred, in which case the outcome arrives later through
```ApplyProgrammingCompletion``` and the same staleness rules apply.

Backend calls are always made with the runtime lock released, and an
implementation that calls back into the runtime is detected: the nested call is
refused and the originating operation fails with ```ReentrancyViolation```
instead of deadlocking.

### 8.1 Synthetic programming backend (SYNTHETIC)

```SyntheticProgrammingBackend``` deterministically covers successful install,
idempotent install, delayed install, rejected install, transient failure,
permanent failure, ambiguous completion, stale completion, successful withdraw,
failed withdraw, unavailable backend and divergent observed state. It never
touches the host routing table.

### 8.2 Real host route adapter (REAL, read-only)

```WindowsRouteTableBackend``` enumerates and queries the real host routing table
through ```GetIpForwardTable2``` for IPv4 and IPv6, normalizes entries into
canonical destinations and exposes them as operator evidence. It is physically
read-only: every mutation returns NOT_SUPPORTED and the host routing table is
never modified by any code in this repository, including its tests.

### 8.3 Reconciliation

Reconciliation compares the desired route with the observed backend state and
classifies MATCHED, MISSING, DIVERGED, EXTRA or UNAVAILABLE. Reconciliation is
evidence, never authority: it never mutates desired state to match the backend,
and a missing or divergent observation makes the route non-current rather than
installed.

## 9. Currentness

```RouteCurrentness``` is explicit and never collapsed into a boolean:
CURRENT, NOT_INSTALLED, REVALIDATION_REQUIRED, STALE_EPOCH, STALE_PUBLISHER,
STALE_WORKER_BOOT, STALE_PATH_AUTHORITY, STALE_BACKEND_OBSERVATION,
FENCED_PUBLISHER, PATH_AUTHORITY_REJECTED, WITHDRAWN, RETIRED, SUPERSEDED,
FAILED, REVOKED, DESIRED_APPLIED_MISMATCH.

A route can exist in the forwarding hardware while no longer being current
control-plane authority; that is represented, not hidden.

## 10. Snapshots, diffs, digests, explanations

* Snapshots are immutable and content addressed. Old snapshots remain
  inspectable and never grant authority.
* Diffs between snapshots are deterministic and emitted in a stable field order.
* The semantic digest of a route covers authority-relevant fields only. It
  excludes timestamps, thread identities, socket handles, memory addresses,
  arrival order, process-local counters and diagnostic-only text. Equivalent
  state reached through different publication orders hashes identically.
* Whole-runtime snapshot digests are order independent because routes are visited
  in route identity order.
* ```RouteExplanation``` answers the operator questions: why a route is
  authoritative or not, which publisher and boot produced it, which epoch and
  path authority generation support it, why it is revalidation-required,
  withdrawn or rejected, which backend attempt last touched it, and whether the
  observed backend state matched the desired state.

## 11. Persistence

The store is versioned and integrity checked. A snapshot file holds the complete
durable state; a journal file holds individually flushed mutation entries and is
compacted into a new snapshot on demand. Both use an explicit magic, a format
version, a deterministic payload and an integrity trailer
(```CRC-32C``` plus a 64-bit content hash) over the payload. The snapshot is
written to a temporary file, flushed, and atomically moved into place.

Loading rejects: empty files, wrong magic, unsupported versions, truncated
headers, every truncation point, corrupted trailers, trailing bytes, oversized
payload declarations, duplicate route identities, duplicate route keys,
duplicate programming attempts, zero or impossible generations, applied
generations newer than the desired generation, retired routes without a
retirement record, superseded routes without a supersession record, installed
routes without an applied outcome, malformed destinations and malformed
enumerations. A torn journal tail is discarded and truncated: such a tail can
only come from a mutation that was never acknowledged.

## 12. Distributed process model

```text
rf_coordinator  one authoritative RouteFabricRuntime behind a TCP front end
rf_publisher    a real worker process that registers and publishes/holds
rf_cli          operator CLI (direct mode or wire mode)
```

Wire messages use explicit stable numeric identifiers (HELLO,
REGISTER_PUBLISHER, PUBLISH_ROUTE, WITHDRAW_ROUTE, REVALIDATE_ROUTE,
RETIRE_ROUTE, REVOKE_ROUTE, QUERY_ROUTE, EXPLAIN_ROUTE, SNAPSHOT_REQUEST,
BACKEND_STATE, RECONCILE_REQUEST, EPOCH_REQUEST, STATISTICS_REQUEST,
APPLY_COMPLETION, FENCE_NOTICE, ERROR and their results). No C++ object layout is
ever serialized. Frames are bounded, strictly validated, integrity checked and
must be consumed exactly.

Sessions are bounded, a partially delivered frame fails explicitly on peer
close, and a protocol violation terminates the session. Shutdown joins every
session thread before returning.

## 13. Proof classification

**REAL** (measured or executed for real):

* host route enumeration and querying through the Windows IP Helper API;
* independent operating-system processes with actual process termination;
* loopback TCP between those processes;
* durable stores with flushed writes and atomic replacement;
* coordinator restart from the same durable store;
* worker death, connection loss, fencing and reincarnation;
* epoch advance, stale-epoch refusal and stale-boot refusal across restarts.

**SYNTHETIC** (deterministic stand-ins, labelled as such):

* the programming backend and all of its outcomes;
* the in-memory path authority;
* fabric-scale destinations and high route counts;
* multi-site or leaf/spine style addresses.

**UNSUPPORTED** (not implemented, not claimed):

* physical switch or ASIC programming;
* vendor SDK programming;
* BGP, OSPF, IS-IS, EVPN or any routing protocol;
* multi-host routing convergence;
* consensus, leader election or split-brain exclusion;
* cryptographic authentication, TLS or signatures;
* path computation, ECMP policy, weighted path selection, adaptive routing,
  traffic engineering, bandwidth reservation, congestion control, queue or
  buffer allocation and packet scheduling.

## 14. Building

Requirements: CMake 3.25 or newer, a C++20 compiler (MSVC 19.4x verified) and
Ninja or another generator.

```text
cmake -S . -B build/release -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build/release
```

Options:

| Option | Default | Meaning |
| --- | --- | --- |
| ```ROUTEFABRIC_BUILD_TESTS``` | ON | build and register the test suite |
| ```ROUTEFABRIC_BUILD_EXAMPLES``` | ON | build and register the examples |
| ```ROUTEFABRIC_BUILD_BENCHMARKS``` | ON | build the benchmarks |
| ```ROUTEFABRIC_BUILD_TOOLS``` | ON | build coordinator, publisher and CLI |
| ```ROUTEFABRIC_WARNINGS_AS_ERRORS``` | ON | ```/W4 /WX /permissive-``` on first-party code |
| ```ROUTEFABRIC_ENABLE_ANALYZE``` | OFF | run the MSVC static analyzer on first-party code |
| ```ROUTEFABRIC_ENABLE_ASAN``` | OFF | build with AddressSanitizer where supported |

## 15. Testing

```text
ctest --test-dir build/release --output-on-failure
build/release/tests/rf_tests.exe            # the whole suite
build/release/tests/rf_tests.exe race       # filter by test-name substring
```

The suite contains unit tests, an exhaustive lifecycle transition matrix, a
seeded deterministic property suite, deterministic race tests, adversarial
input tests, persistence corruption and truncation tests, wire protocol
conformance tests, scale tests (1,000 and 10,000 routes) and real process-level
proofs. There are no test framework timeouts: a hanging test is a defect. The
only bounded waits are internal polls whose exhaustion is reported as an explicit
test failure.

## 16. Installing and consuming

```text
cmake --install build/release --prefix <prefix>
```

```text
find_package(RouteFabric CONFIG REQUIRED)
target_link_libraries(app PRIVATE SummonSoftwareLabs::RouteFabric)
```

The installed package provides ```RouteFabricConfig.cmake```,
```RouteFabricConfigVersion.cmake```, ```RouteFabricTargets.cmake```, the public
headers including the generated ```routefabric/version.hpp```, the static
library and the three executables. The repository's own test suite configures,
builds and runs an independent consumer that uses nothing but the installed
artifacts.

## 17. Examples

Ten examples under ```examples/``` use only the public API and are executed by the
test suite:

| Example | Shows |
| --- | --- |
| ```ex_basic_publication``` | publication, lifecycle, explanation |
| ```ex_path_backed_route``` | path binding and path invalidation |
| ```ex_route_replacement``` | identity stability and lineage |
| ```ex_withdrawal``` | withdrawal request versus applied outcome |
| ```ex_stale_publisher``` | worker reincarnation fencing a stale boot |
| ```ex_stale_epoch``` | epoch advance invalidating live authority |
| ```ex_backend_failure``` | every structured refusal and its lifecycle mapping |
| ```ex_ambiguous_completion``` | deferred dispatch and late acknowledgment |
| ```ex_restart_reconciliation``` | durable intent, reconciliation, revalidation |
| ```ex_snapshot_and_diff``` | snapshots, digests and deterministic diffs |

## 18. Command line

```text
rf_coordinator [--store <path> --durability journal|snapshot] [--bind <addr>]
               [--port <n>] [--fabric <id>] [--backend synthetic|real]
               [--paths <file>] [--epoch-policy resume|advance]
               [--exit-file <path>] [--shutdown-on-stdin]
rf_publisher   --port <n> --publisher <id> [--destination <prefix>]
               [--path <hex> --path-generation <n>] [--status-file <path>]
               [--hold] [--withdraw]
rf_cli         [--store <path>] [--port <n>] <group> <command> [args]
```

CLI commands: ```version```, ```route publish|list|show|explain|withdraw|revalidate|retire|revoke|reconcile|snapshot```,
```store inspect|diff```, ```backend show```, ```path add|invalidate|list```. Output is line
oriented, deterministic and script friendly; exit codes are 0 for success, 1 for
a command failure and 2 for a usage error.

## 19. Benchmarks

```text
build/release/benchmarks/rf_bench.exe [--quick]
```

Benchmarks report the number of operations that actually completed alongside
elapsed milliseconds, so a partial run cannot be mistaken for a fast one. They
measure publication at 1k/10k/100k routes, exact lookup, replacement,
withdrawal, path invalidation, snapshot, digest, store save/load and
reconciliation. The numbers describe this machine and this build; they are not
portable performance guarantees.

## 20. Concurrency

* All public runtime methods are safe to call concurrently.
* Mutations are serialized, so conflicting operations on one route key resolve
  deterministically.
* Queries take a shared lock; snapshots and digests are computed under it.
* Backend calls never happen under the state lock.
* Path Authority queries happen under the state lock and must therefore be
  synchronous, non-blocking and free of callbacks into the runtime; a violation
  is detected and reported rather than allowed to deadlock.
* Durable persistence happens inside the critical section, because a mutation is
  never acknowledged before its durable record is flushed.
* No mutable internal reference is ever handed out.

## 21. Genuine limitations

* One authoritative coordinator per store; no consensus and no split-brain
  exclusion.
* Plain trusted TCP with non-cryptographic integrity; no authentication.
* Route lookup is exact-key only; no longest-prefix-match and no route selection
  policy.
* The shipped programming backend is synthetic; the real adapter is read-only,
  so no real forwarding-plane mutation is exercised.
* Path Authority is consumed but not implemented; the shipped implementation is
  an in-memory stand-in.
* Retirement does not program the backend: withdrawal is the operation that
  requests physical removal, and reconciliation reports the difference.
* History per route lineage is bounded; current authority never depends on
  scanning history.
* Snapshot durability rewrites the whole state and is therefore O(routes) per
  acknowledged mutation; the journal durability mode exists for that reason.

## License

Apache License 2.0. Copyright 2026 Summon Software Labs. No telemetry transmission.
