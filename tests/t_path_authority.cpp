#include "test_framework.hpp"

#include "harness.hpp"

using namespace routefabric;
using rftest::Harness;
using rftest::make_id;

namespace {

struct PathHarness {
  std::unique_ptr<Harness> harness;
  PathId path = make_id<PathId>(100);
  PathAuthorityGeneration generation = PathAuthorityGeneration::from_value(1);
};

std::unique_ptr<PathHarness> make_path_harness(PathAuthorization state = PathAuthorization::Usable) {
  auto result = std::make_unique<PathHarness>();
  result->harness = Harness::Create();
  (void)result->harness->path_authority.SetPath(result->path, result->generation, state);
  (void)result->harness->register_default();
  return result;
}

}  // namespace

RF_TEST(path_backed_routes_require_a_current_usable_path) {
  auto fixture = make_path_harness();
  const RouteKey key = fixture->harness->key("10.0.0.0/24");
  const Expected<PublishResult> published =
      fixture->harness->publish(key, fixture->harness->path_binding(fixture->path, fixture->generation));
  RF_REQUIRE(published.has_value());
  RF_CHECK(published.value().currentness == RouteCurrentness::Current);
  const Expected<RouteSnapshot> snapshot = fixture->harness->query(key);
  RF_REQUIRE(snapshot.has_value());
  RF_CHECK(snapshot.value().record.binding.kind == BindingKind::AuthorizedPath);
  RF_CHECK(snapshot.value().record.provenance.path_authority_generation == fixture->generation);
}

RF_TEST(stale_path_authority_generation_is_refused) {
  auto fixture = make_path_harness();
  const Expected<PathAuthorityGeneration> advanced =
      fixture->harness->path_authority.Invalidate(fixture->path, PathAuthorization::Usable);
  RF_REQUIRE(advanced.has_value());
  const Expected<PublishResult> published = fixture->harness->publish(
      fixture->harness->key("10.0.0.0/24"),
      fixture->harness->path_binding(fixture->path, fixture->generation));
  RF_REQUIRE(!published.has_value());
  RF_CHECK(published.error().code() == StatusCode::PathAuthorityStale);
  RF_CHECK_EQ(fixture->harness->runtime->counters().path_authority_rejections, static_cast<std::uint64_t>(1));
}

RF_TEST(a_rejected_or_unknown_path_never_authorizes_a_route) {
  for (const PathAuthorization state : {PathAuthorization::Rejected, PathAuthorization::Revoked,
                                        PathAuthorization::Retired, PathAuthorization::Stale,
                                        PathAuthorization::RevalidationRequired}) {
    auto fixture = make_path_harness(state);
    const Expected<PublishResult> published = fixture->harness->publish(
        fixture->harness->key("10.0.0.0/24"),
        fixture->harness->path_binding(fixture->path, fixture->generation));
    RF_REQUIRE(!published.has_value());
    RF_CHECK(published.error().code() == StatusCode::PathAuthorityRejected);
  }
  auto unknown = make_path_harness();
  const PathId other = make_id<PathId>(999);
  const Expected<PublishResult> published = unknown->harness->publish(
      unknown->harness->key("10.0.0.0/24"),
      unknown->harness->path_binding(other, PathAuthorityGeneration::from_value(1)));
  RF_REQUIRE(!published.has_value());
  RF_CHECK(published.error().code() == StatusCode::PathAuthorityRejected);
}

RF_TEST(path_invalidation_marks_dependent_routes_non_current) {
  auto fixture = make_path_harness();
  const RouteKey key = fixture->harness->key("10.0.0.0/24");
  const Expected<PublishResult> published =
      fixture->harness->publish(key, fixture->harness->path_binding(fixture->path, fixture->generation));
  RF_REQUIRE(published.has_value());

  const Expected<PathAuthorityGeneration> invalidated =
      fixture->harness->path_authority.Invalidate(fixture->path, PathAuthorization::RevalidationRequired);
  RF_REQUIRE(invalidated.has_value());
  const Status status = fixture->harness->runtime->OnPathAuthorityChanged(
      fixture->path, invalidated.value(), PathAuthorization::RevalidationRequired);
  RF_REQUIRE(status.has_value());

  const Expected<RouteSnapshot> snapshot = fixture->harness->query(key);
  RF_REQUIRE(snapshot.has_value());
  RF_CHECK(snapshot.value().record.lifecycle == RouteLifecycle::RevalidationRequired);
  RF_CHECK(snapshot.value().record.applied.classification == AppliedClassification::Applied);
  RF_CHECK(snapshot.value().record.generation > published.value().generation);
  RF_CHECK(snapshot.value().record.invalidation_watermark == snapshot.value().record.authority_generation);
  RF_CHECK(snapshot.value().currentness == RouteCurrentness::PathAuthorityRejected);
  RF_CHECK(!is_current(snapshot.value().currentness));

  // Revalidating against a path that is no longer usable is refused.
  RevalidateRequest revalidate;
  revalidate.publisher = fixture->harness->publisher;
  revalidate.worker_boot = fixture->harness->boot;
  revalidate.attempt = fixture->harness->next_attempt();
  revalidate.route = published.value().route;
  revalidate.reason = "attempt under invalidated path";
  const Expected<WithdrawOutcome> refused = fixture->harness->runtime->RevalidateRoute(revalidate);
  RF_REQUIRE(!refused.has_value());
  // The bound generation is no longer current, so the refusal is either a stale
  // generation or an unusable path; both are conservative.
  RF_CHECK(refused.error().code() == StatusCode::PathAuthorityRejected ||
           refused.error().code() == StatusCode::PathAuthorityStale);
}

RF_TEST(path_invalidation_is_precise) {
  auto fixture = make_path_harness();
  const PathId other_path = make_id<PathId>(200);
  const PathAuthorityGeneration other_generation = PathAuthorityGeneration::from_value(3);
  RF_REQUIRE(fixture->harness->path_authority
                 .SetPath(other_path, other_generation, PathAuthorization::Usable)
                 .has_value());
  const RouteKey dependent = fixture->harness->key("10.0.0.0/24");
  const RouteKey independent = fixture->harness->key("10.0.1.0/24");
  const RouteKey plain = fixture->harness->key("10.0.2.0/24");
  RF_REQUIRE(fixture->harness->publish(dependent, fixture->harness->path_binding(fixture->path, fixture->generation))
                 .has_value());
  RF_REQUIRE(fixture->harness->publish(independent, fixture->harness->path_binding(other_path, other_generation))
                 .has_value());
  RF_REQUIRE(fixture->harness->publish(plain, fixture->harness->next_hop_binding(fixture->harness->next_next_hop()))
                 .has_value());

  const Expected<PathAuthorityGeneration> invalidated =
      fixture->harness->path_authority.Invalidate(fixture->path, PathAuthorization::Revoked);
  RF_REQUIRE(invalidated.has_value());
  RF_REQUIRE(fixture->harness->runtime
                 ->OnPathAuthorityChanged(fixture->path, invalidated.value(), PathAuthorization::Revoked)
                 .has_value());

  const Expected<RouteSnapshot> changed = fixture->harness->query(dependent);
  const Expected<RouteSnapshot> untouched = fixture->harness->query(independent);
  const Expected<RouteSnapshot> unaffected = fixture->harness->query(plain);
  RF_REQUIRE(changed.has_value());
  RF_REQUIRE(untouched.has_value());
  RF_REQUIRE(unaffected.has_value());
  RF_CHECK(changed.value().record.lifecycle == RouteLifecycle::RevalidationRequired);
  RF_CHECK(untouched.value().record.lifecycle == RouteLifecycle::Installed);
  RF_CHECK(unaffected.value().record.lifecycle == RouteLifecycle::Installed);
  RF_CHECK_EQ(untouched.value().record.generation.value(), static_cast<std::uint64_t>(1));
  RF_CHECK_EQ(unaffected.value().record.generation.value(), static_cast<std::uint64_t>(1));
}

RF_TEST(path_generation_refresh_never_keeps_a_route_current) {
  auto fixture = make_path_harness();
  const RouteKey key = fixture->harness->key("10.0.0.0/24");
  RF_REQUIRE(fixture->harness->publish(key, fixture->harness->path_binding(fixture->path, fixture->generation))
                 .has_value());
  const Expected<PathAuthorityGeneration> advanced =
      fixture->harness->path_authority.Invalidate(fixture->path, PathAuthorization::Usable);
  RF_REQUIRE(advanced.has_value());
  RF_REQUIRE(fixture->harness->runtime
                 ->OnPathAuthorityChanged(fixture->path, advanced.value(), PathAuthorization::Usable)
                 .has_value());
  const Expected<RouteSnapshot> snapshot = fixture->harness->query(key);
  RF_REQUIRE(snapshot.has_value());
  RF_CHECK(!is_current(snapshot.value().currentness));
  RF_CHECK(snapshot.value().currentness == RouteCurrentness::StalePathAuthority);
}

RF_TEST(revalidate_path_authorities_repairs_a_stale_view) {
  auto fixture = make_path_harness();
  const RouteKey key = fixture->harness->key("10.0.0.0/24");
  RF_REQUIRE(fixture->harness->publish(key, fixture->harness->path_binding(fixture->path, fixture->generation))
                 .has_value());
  // Path Authority changes its answer without an explicit notification: the
  // runtime is stale until it re-queries.
  RF_REQUIRE(fixture->harness->path_authority.Invalidate(fixture->path, PathAuthorization::Usable).has_value());
  const Expected<RouteSnapshot> before = fixture->harness->query(key);
  RF_REQUIRE(before.has_value());
  RF_CHECK(before.value().currentness == RouteCurrentness::StalePathAuthority);
  RF_REQUIRE(fixture->harness->runtime->RevalidatePathAuthorities().has_value());
  const Expected<RouteSnapshot> after = fixture->harness->query(key);
  RF_REQUIRE(after.has_value());
  RF_CHECK(after.value().record.lifecycle == RouteLifecycle::RevalidationRequired);
  RF_CHECK(after.value().record.provenance.path_authority_generation ==
           PathAuthorityGeneration::from_value(2));
}

RF_TEST(path_invalidation_does_not_resurrect_withdrawn_or_retired_routes) {
  auto fixture = make_path_harness();
  const RouteKey withdrawn_key = fixture->harness->key("10.0.0.0/24");
  const RouteKey retired_key = fixture->harness->key("10.0.1.0/24");
  const Expected<PublishResult> withdrawn = fixture->harness->publish(
      withdrawn_key, fixture->harness->path_binding(fixture->path, fixture->generation));
  const Expected<PublishResult> retired = fixture->harness->publish(
      retired_key, fixture->harness->path_binding(fixture->path, fixture->generation));
  RF_REQUIRE(withdrawn.has_value());
  RF_REQUIRE(retired.has_value());
  WithdrawRequest withdraw;
  withdraw.publisher = fixture->harness->publisher;
  withdraw.worker_boot = fixture->harness->boot;
  withdraw.attempt = fixture->harness->next_attempt();
  withdraw.route = withdrawn.value().route;
  RF_REQUIRE(fixture->harness->runtime->WithdrawRoute(withdraw).has_value());
  RetireRequest retire;
  retire.publisher = fixture->harness->publisher;
  retire.worker_boot = fixture->harness->boot;
  retire.attempt = fixture->harness->next_attempt();
  retire.route = retired.value().route;
  RF_REQUIRE(fixture->harness->runtime->RetireRoute(retire).has_value());

  const Expected<PathAuthorityGeneration> invalidated =
      fixture->harness->path_authority.Invalidate(fixture->path, PathAuthorization::Revoked);
  RF_REQUIRE(invalidated.has_value());
  RF_REQUIRE(fixture->harness->runtime
                 ->OnPathAuthorityChanged(fixture->path, invalidated.value(), PathAuthorization::Revoked)
                 .has_value());
  const Expected<RouteSnapshot> withdrawn_snapshot = fixture->harness->query(withdrawn_key);
  const Expected<RouteSnapshot> retired_snapshot = fixture->harness->query(retired_key);
  RF_REQUIRE(withdrawn_snapshot.has_value());
  RF_REQUIRE(retired_snapshot.has_value());
  RF_CHECK(withdrawn_snapshot.value().record.lifecycle == RouteLifecycle::Withdrawn);
  RF_CHECK(retired_snapshot.value().record.lifecycle == RouteLifecycle::Retired);
}
