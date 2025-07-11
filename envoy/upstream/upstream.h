#pragma once

#include <chrono>
#include <cstdint>
#include <functional>
#include <list>
#include <memory>
#include <string>
#include <vector>

#include "envoy/common/callback.h"
#include "envoy/common/optref.h"
#include "envoy/config/cluster/v3/cluster.pb.h"
#include "envoy/config/core/v3/base.pb.h"
#include "envoy/config/core/v3/protocol.pb.h"
#include "envoy/config/typed_metadata.h"
#include "envoy/http/codec.h"
#include "envoy/http/filter_factory.h"
#include "envoy/http/header_validator.h"
#include "envoy/network/connection.h"
#include "envoy/network/transport_socket.h"
#include "envoy/ssl/context.h"
#include "envoy/stats/scope.h"
#include "envoy/stats/stats.h"
#include "envoy/upstream/health_check_host_monitor.h"
#include "envoy/upstream/locality.h"
#include "envoy/upstream/outlier_detection.h"
#include "envoy/upstream/resource_manager.h"
#include "envoy/upstream/types.h"

#include "absl/strings/string_view.h"
#include "absl/types/optional.h"
#include "fmt/format.h"

namespace Envoy {
namespace Http {
class FilterChainManager;
}

namespace Upstream {

/**
 * A bundle struct for address and socket options.
 */
struct UpstreamLocalAddress {
public:
  Network::Address::InstanceConstSharedPtr address_;
  Network::ConnectionSocket::OptionsSharedPtr socket_options_;
};

/**
 * Interface to select upstream local address based on the endpoint address.
 */
class UpstreamLocalAddressSelector {
public:
  virtual ~UpstreamLocalAddressSelector() = default;

  /**
   * Return UpstreamLocalAddress based on the endpoint address.
   * @param endpoint_address is the address used to select upstream local address.
   * @param socket_options applied to the selected address.
   * @return UpstreamLocalAddress which includes the selected upstream local address and socket
   * options.
   */
  UpstreamLocalAddress
  getUpstreamLocalAddress(const Network::Address::InstanceConstSharedPtr& endpoint_address,
                          const Network::ConnectionSocket::OptionsSharedPtr& socket_options) const {
    UpstreamLocalAddress local_address = getUpstreamLocalAddressImpl(endpoint_address);
    Network::ConnectionSocket::OptionsSharedPtr connection_options =
        std::make_shared<Network::ConnectionSocket::Options>(
            socket_options ? *socket_options
                           : std::vector<Network::ConnectionSocket::OptionConstSharedPtr>{});
    return {local_address.address_,
            local_address.socket_options_ != nullptr
                ? Network::Socket::appendOptions(connection_options, local_address.socket_options_)
                : connection_options};
  }

private:
  /*
   * The implementation is responsible for picking the ``UpstreamLocalAddress``
   * based on the ``endpoint_address``. However adding the connection socket
   * options is the responsibility of the base class.
   */
  virtual UpstreamLocalAddress getUpstreamLocalAddressImpl(
      const Network::Address::InstanceConstSharedPtr& endpoint_address) const PURE;
};

using UpstreamLocalAddressSelectorConstSharedPtr =
    std::shared_ptr<const UpstreamLocalAddressSelector>;

class UpstreamLocalAddressSelectorFactory : public Config::TypedFactory {
public:
  ~UpstreamLocalAddressSelectorFactory() override = default;

  /**
   * @param cluster_name is set to the name of the cluster if ``bind_config`` is
   *   from cluster config. If the bind config from the cluster manager, the param
   *   is empty.
   */
  virtual absl::StatusOr<UpstreamLocalAddressSelectorConstSharedPtr>
  createLocalAddressSelector(std::vector<UpstreamLocalAddress> upstream_local_addresses,
                             absl::optional<std::string> cluster_name) const PURE;

  std::string category() const override { return "envoy.upstream.local_address_selector"; }
};

/**
 * RAII handle for tracking the host usage by the connection pools.
 **/
class HostHandle {
public:
  virtual ~HostHandle() = default;
};

using HostHandlePtr = std::unique_ptr<HostHandle>;

/**
 * An upstream host.
 */
class Host : virtual public HostDescription {
public:
  struct CreateConnectionData {
    Network::ClientConnectionPtr connection_;
    HostDescriptionConstSharedPtr host_description_;
  };

  // We use an X-macro here to make it easier to verify that all the enum values are accounted for.
  // clang-format off
#define HEALTH_FLAG_ENUM_VALUES(m)                                               \
  /* The host is currently failing active health checks. */                      \
  m(FAILED_ACTIVE_HC, 0x1)                                                       \
  /* The host is currently considered an outlier and has been ejected. */        \
  m(FAILED_OUTLIER_CHECK, 0x02)                                                  \
  /* The host is currently marked as unhealthy by EDS. */                        \
  m(FAILED_EDS_HEALTH, 0x04)                                                     \
  /* The host is currently marked as degraded through active health checking. */ \
  m(DEGRADED_ACTIVE_HC, 0x08)                                                    \
  /* The host is currently marked as degraded by EDS. */                         \
  m(DEGRADED_EDS_HEALTH, 0x10)                                                   \
  /* The host is pending removal from discovery but is stabilized due to */      \
  /* active HC. */                                                               \
  m(PENDING_DYNAMIC_REMOVAL, 0x20)                                               \
  /* The host is pending its initial active health check. */                     \
  m(PENDING_ACTIVE_HC, 0x40)                                                     \
  /* The host should be excluded from panic, spillover, etc. calculations */     \
  /* because it was explicitly taken out of rotation via protocol signal and */  \
  /* is not meant to be routed to. */                                            \
  m(EXCLUDED_VIA_IMMEDIATE_HC_FAIL, 0x80)                                        \
  /* The host failed active HC due to timeout. */                                \
  m(ACTIVE_HC_TIMEOUT, 0x100)                                                    \
  /* The host is currently marked as draining by EDS */                          \
  m(EDS_STATUS_DRAINING, 0x200)
  // clang-format on

#define DECLARE_ENUM(name, value) name = value,

  enum class HealthFlag { HEALTH_FLAG_ENUM_VALUES(DECLARE_ENUM) };

#undef DECLARE_ENUM

  /**
   * @return host specific counters.
   */
  virtual std::vector<std::pair<absl::string_view, Stats::PrimitiveCounterReference>>
  counters() const PURE;

  /**
   * Create a connection for this host.
   * @param dispatcher supplies the owning dispatcher.
   * @param options supplies the socket options that will be set on the new connection.
   * @param transport_socket_options supplies the transport options that will be set on the new
   * connection.
   * @return the connection data which includes the raw network connection as well as the *real*
   *         host that backs it. The reason why a 2nd host is returned is that some hosts are
   *         logical and wrap multiple real network destinations. In this case, a different host
   *         will be returned along with the connection vs. the host the method was called on.
   *         If it matters, callers should not assume that the returned host will be the same.
   */
  virtual CreateConnectionData createConnection(
      Event::Dispatcher& dispatcher, const Network::ConnectionSocket::OptionsSharedPtr& options,
      Network::TransportSocketOptionsConstSharedPtr transport_socket_options) const PURE;

  /**
   * Create a health check connection for this host.
   * @param dispatcher supplies the owning dispatcher.
   * @param transport_socket_options supplies the transport options that will be set on the new
   * connection.
   * @return the connection data.
   */
  virtual CreateConnectionData createHealthCheckConnection(
      Event::Dispatcher& dispatcher,
      Network::TransportSocketOptionsConstSharedPtr transport_socket_options,
      const envoy::config::core::v3::Metadata* metadata) const PURE;

  /**
   * @return host specific gauges.
   */
  virtual std::vector<std::pair<absl::string_view, Stats::PrimitiveGaugeReference>>
  gauges() const PURE;

  /**
   * Atomically clear a health flag for a host. Flags are specified in HealthFlags.
   */
  virtual void healthFlagClear(HealthFlag flag) PURE;

  /**
   * Atomically get whether a health flag is set for a host. Flags are specified in HealthFlags.
   */
  virtual bool healthFlagGet(HealthFlag flag) const PURE;

  /**
   * Atomically set a health flag for a host. Flags are specified in HealthFlags.
   */
  virtual void healthFlagSet(HealthFlag flag) PURE;

  /**
   * Atomically get multiple health flags that are set for a host. Flags are specified
   * as a bitset of HealthFlags.
   */
  virtual uint32_t healthFlagsGetAll() const PURE;

  /**
   * Atomically set the health flag for a host. Flags are specified as a bitset
   * of HealthFlags.
   */
  virtual void healthFlagsSetAll(uint32_t bits) PURE;

  enum class Health {
    /**
     * Host is unhealthy and is not able to serve traffic. A host may be marked as unhealthy either
     * through EDS or through active health checking.
     */
    Unhealthy,
    /**
     * Host is healthy, but degraded. It is able to serve traffic, but hosts that aren't degraded
     * should be preferred. A host may be marked as degraded either through EDS or through active
     * health checking.
     */
    Degraded,
    /**
     * Host is healthy and is able to serve traffic.
     */
    Healthy,
  };

  /**
   * @return the coarse health status of the host.
   */
  virtual Health coarseHealth() const PURE;

  using HealthStatus = envoy::config::core::v3::HealthStatus;

  /**
   * @return more specific health status of host. This status is hybrid of EDS status and runtime
   * active status (from active health checker or outlier detection). Active status will be taken as
   * a priority.
   */
  virtual HealthStatus healthStatus() const PURE;

  /**
   * Set the EDS health status of the host. This is used when the host status is updated via EDS.
   */
  virtual void setEdsHealthStatus(HealthStatus health_status) PURE;

  /**
   * @return the EDS health status of the host.
   */
  virtual HealthStatus edsHealthStatus() const PURE;

  /**
   * @return the current load balancing weight of the host, in the range 1-128 (see
   * envoy.api.v2.endpoint.Endpoint.load_balancing_weight).
   */
  virtual uint32_t weight() const PURE;

  /**
   * Set the current load balancing weight of the host, in the range 1-128 (see
   * envoy.api.v2.endpoint.Endpoint.load_balancing_weight).
   */
  virtual void weight(uint32_t new_weight) PURE;

  /**
   * @return the current boolean value of host being in use by any connection pool.
   */
  virtual bool used() const PURE;

  /**
   * Creates a handle for a host. Deletion of the handle signals that the
   * connection pools no longer need this host.
   */
  virtual HostHandlePtr acquireHandle() const PURE;

  /**
   * @return true if active health check is disabled.
   */
  virtual bool disableActiveHealthCheck() const PURE;

  /**
   * Set true to disable active health check for the host.
   */
  virtual void setDisableActiveHealthCheck(bool disable_active_health_check) PURE;
};

using HostConstSharedPtr = std::shared_ptr<const Host>;

using HostVector = std::vector<HostSharedPtr>;
using HealthyHostVector = Phantom<HostVector, Healthy>;
using DegradedHostVector = Phantom<HostVector, Degraded>;
using ExcludedHostVector = Phantom<HostVector, Excluded>;
using HostMap = absl::flat_hash_map<std::string, Upstream::HostSharedPtr>;
using HostMapSharedPtr = std::shared_ptr<HostMap>;
using HostMapConstSharedPtr = std::shared_ptr<const HostMap>;
using HostVectorSharedPtr = std::shared_ptr<HostVector>;
using HostVectorConstSharedPtr = std::shared_ptr<const HostVector>;

using HealthyHostVectorConstSharedPtr = std::shared_ptr<const HealthyHostVector>;
using DegradedHostVectorConstSharedPtr = std::shared_ptr<const DegradedHostVector>;
using ExcludedHostVectorConstSharedPtr = std::shared_ptr<const ExcludedHostVector>;

using HostListPtr = std::unique_ptr<HostVector>;
using LocalityWeightsMap =
    absl::node_hash_map<envoy::config::core::v3::Locality, uint32_t, LocalityHash, LocalityEqualTo>;
using PriorityState = std::vector<std::pair<HostListPtr, LocalityWeightsMap>>;

/**
 * Bucket hosts by locality.
 */
class HostsPerLocality {
public:
  virtual ~HostsPerLocality() = default;

  /**
   * @return bool is local locality one of the locality buckets? If so, the
   *         local locality will be the first in the get() vector.
   */
  virtual bool hasLocalLocality() const PURE;

  /**
   * @return const std::vector<HostVector>& list of hosts organized per
   *         locality. The local locality is the first entry if
   *         hasLocalLocality() is true. All hosts within the same entry have the same locality
   *         and all hosts with a given locality are in the same entry. With the exception of
   *         the local locality entry (if present), all entries are sorted by locality with
   *         those considered less by the LocalityLess comparator ordered earlier in the list.
   */
  virtual const std::vector<HostVector>& get() const PURE;

  /**
   * Clone object with multiple filter predicates. Returns a vector of clones, each with host that
   * match the provided predicates.
   * @param predicates vector of predicates on Host entries.
   * @return vector of HostsPerLocalityConstSharedPtr clones of the HostsPerLocality that match
   *         hosts according to predicates.
   */
  virtual std::vector<std::shared_ptr<const HostsPerLocality>>
  filter(const std::vector<std::function<bool(const Host&)>>& predicates) const PURE;

  /**
   * Clone object.
   * @return HostsPerLocalityConstSharedPtr clone of the HostsPerLocality.
   */
  std::shared_ptr<const HostsPerLocality> clone() const {
    return filter({[](const Host&) { return true; }})[0];
  }
};

using HostsPerLocalitySharedPtr = std::shared_ptr<HostsPerLocality>;
using HostsPerLocalityConstSharedPtr = std::shared_ptr<const HostsPerLocality>;

// Weight for each locality index in HostsPerLocality.
using LocalityWeights = std::vector<uint32_t>;
using LocalityWeightsSharedPtr = std::shared_ptr<LocalityWeights>;
using LocalityWeightsConstSharedPtr = std::shared_ptr<const LocalityWeights>;

/**
 * Base host set interface. This contains all of the endpoints for a given LocalityLbEndpoints
 * priority level.
 */
// TODO(snowp): Remove the const ref accessors in favor of the shared_ptr ones.
class HostSet {
public:
  virtual ~HostSet() = default;

  /**
   * @return all hosts that make up the set at the current time.
   */
  virtual const HostVector& hosts() const PURE;

  /**
   * @return a shared ptr to the vector returned by hosts().
   */
  virtual HostVectorConstSharedPtr hostsPtr() const PURE;

  /**
   * @return all healthy hosts contained in the set at the current time. NOTE: This set is
   *         eventually consistent. There is a time window where a host in this set may become
   *         unhealthy and calling healthy() on it will return false. Code should be written to
   *         deal with this case if it matters.
   */
  virtual const HostVector& healthyHosts() const PURE;

  /**
   * @return a shared ptr to the vector returned by healthyHosts().
   */
  virtual HealthyHostVectorConstSharedPtr healthyHostsPtr() const PURE;

  /**
   * @return all degraded hosts contained in the set at the current time. NOTE: This set is
   *         eventually consistent. There is a time window where a host in this set may become
   *         undegraded and calling degraded() on it will return false. Code should be written to
   *         deal with this case if it matters.
   */
  virtual const HostVector& degradedHosts() const PURE;

  /**
   * @return a shared ptr to the vector returned by degradedHosts().
   */
  virtual DegradedHostVectorConstSharedPtr degradedHostsPtr() const PURE;

  /*
   * @return all excluded hosts contained in the set at the current time. Excluded hosts should be
   * ignored when computing load balancing weights, but may overlap with hosts in hosts().
   */
  virtual const HostVector& excludedHosts() const PURE;

  /**
   * @return a shared ptr to the vector returned by excludedHosts().
   */
  virtual ExcludedHostVectorConstSharedPtr excludedHostsPtr() const PURE;

  /**
   * @return hosts per locality.
   */
  virtual const HostsPerLocality& hostsPerLocality() const PURE;

  /**
   * @return a shared ptr to the HostsPerLocality returned by hostsPerLocality().
   */
  virtual HostsPerLocalityConstSharedPtr hostsPerLocalityPtr() const PURE;

  /**
   * @return same as hostsPerLocality but only contains healthy hosts.
   */
  virtual const HostsPerLocality& healthyHostsPerLocality() const PURE;

  /**
   * @return a shared ptr to the HostsPerLocality returned by healthyHostsPerLocality().
   */
  virtual HostsPerLocalityConstSharedPtr healthyHostsPerLocalityPtr() const PURE;

  /**
   * @return same as hostsPerLocality but only contains degraded hosts.
   */
  virtual const HostsPerLocality& degradedHostsPerLocality() const PURE;

  /**
   * @return a shared ptr to the HostsPerLocality returned by degradedHostsPerLocality().
   */
  virtual HostsPerLocalityConstSharedPtr degradedHostsPerLocalityPtr() const PURE;

  /**
   * @return same as hostsPerLocality but only contains excluded hosts.
   */
  virtual const HostsPerLocality& excludedHostsPerLocality() const PURE;

  /**
   * @return a shared ptr to the HostsPerLocality returned by excludedHostsPerLocality().
   */
  virtual HostsPerLocalityConstSharedPtr excludedHostsPerLocalityPtr() const PURE;

  /**
   * @return weights for each locality in the host set.
   */
  virtual LocalityWeightsConstSharedPtr localityWeights() const PURE;

  /**
   * @return next locality index to route to if performing locality weighted balancing
   * against healthy hosts.
   */
  virtual absl::optional<uint32_t> chooseHealthyLocality() PURE;

  /**
   * @return next locality index to route to if performing locality weighted balancing
   * against degraded hosts.
   */
  virtual absl::optional<uint32_t> chooseDegradedLocality() PURE;

  /**
   * @return uint32_t the priority of this host set.
   */
  virtual uint32_t priority() const PURE;

  /**
   * @return uint32_t the overprovisioning factor of this host set.
   */
  virtual uint32_t overprovisioningFactor() const PURE;

  /**
   * @return true to use host weights to calculate the health of a priority.
   */
  virtual bool weightedPriorityHealth() const PURE;
};

using HostSetPtr = std::unique_ptr<HostSet>;

/**
 * This class contains all of the HostSets for a given cluster grouped by priority, for
 * ease of load balancing.
 */
class PrioritySet {
public:
  using MemberUpdateCb =
      std::function<absl::Status(const HostVector& hosts_added, const HostVector& hosts_removed)>;

  using PriorityUpdateCb = std::function<absl::Status(
      uint32_t priority, const HostVector& hosts_added, const HostVector& hosts_removed)>;

  virtual ~PrioritySet() = default;

  /**
   * Install a callback that will be invoked when any of the HostSets in the PrioritySet changes.
   * hosts_added and hosts_removed will only be populated when a host is added or completely removed
   * from the PrioritySet.
   * This includes when a new HostSet is created.
   *
   * @param callback supplies the callback to invoke.
   * @return Common::CallbackHandlePtr a handle which can be used to unregister the callback.
   */
  ABSL_MUST_USE_RESULT virtual Common::CallbackHandlePtr
  addMemberUpdateCb(MemberUpdateCb callback) const PURE;

  /**
   * Install a callback that will be invoked when a host set changes. Triggers when any change
   * happens to the hosts within the host set. If hosts are added/removed from the host set, the
   * added/removed hosts will be passed to the callback.
   *
   * @param callback supplies the callback to invoke.
   * @return Common::CallbackHandlePtr a handle which can be used to unregister the callback.
   */
  ABSL_MUST_USE_RESULT virtual Common::CallbackHandlePtr
  addPriorityUpdateCb(PriorityUpdateCb callback) const PURE;

  /**
   * @return const std::vector<HostSetPtr>& the host sets, ordered by priority.
   */
  virtual const std::vector<HostSetPtr>& hostSetsPerPriority() const PURE;

  /**
   * @return HostMapConstSharedPtr read only cross priority host map that indexed by host address
   * string.
   */
  virtual HostMapConstSharedPtr crossPriorityHostMap() const PURE;

  /**
   * Parameter class for updateHosts.
   */
  struct UpdateHostsParams {
    HostVectorConstSharedPtr hosts;
    HealthyHostVectorConstSharedPtr healthy_hosts;
    DegradedHostVectorConstSharedPtr degraded_hosts;
    ExcludedHostVectorConstSharedPtr excluded_hosts;
    HostsPerLocalityConstSharedPtr hosts_per_locality;
    HostsPerLocalityConstSharedPtr healthy_hosts_per_locality;
    HostsPerLocalityConstSharedPtr degraded_hosts_per_locality;
    HostsPerLocalityConstSharedPtr excluded_hosts_per_locality;
  };

  /**
   * Updates the hosts in a given host set.
   *
   * @param priority the priority of the host set to update.
   * @param update_hosts_param supplies the list of hosts and hosts per locality.
   * @param locality_weights supplies a map from locality to associated weight.
   * @param hosts_added supplies the hosts added since the last update.
   * @param hosts_removed supplies the hosts removed since the last update.
   * @param seed a random number to initialize the locality load-balancing algorithm.
   * @param weighted_priority_health if present, overwrites the current weighted_priority_health.
   * @param overprovisioning_factor if present, overwrites the current overprovisioning_factor.
   * @param cross_priority_host_map read only cross-priority host map which is created in the main
   * thread and shared by all the worker threads.
   */
  virtual void updateHosts(uint32_t priority, UpdateHostsParams&& update_hosts_params,
                           LocalityWeightsConstSharedPtr locality_weights,
                           const HostVector& hosts_added, const HostVector& hosts_removed,
                           uint64_t seed, absl::optional<bool> weighted_priority_health,
                           absl::optional<uint32_t> overprovisioning_factor,
                           HostMapConstSharedPtr cross_priority_host_map = nullptr) PURE;

  /**
   * Callback provided during batch updates that can be used to update hosts.
   */
  class HostUpdateCb {
  public:
    virtual ~HostUpdateCb() = default;
    /**
     * Updates the hosts in a given host set.
     *
     * @param priority the priority of the host set to update.
     * @param update_hosts_param supplies the list of hosts and hosts per locality.
     * @param locality_weights supplies a map from locality to associated weight.
     * @param hosts_added supplies the hosts added since the last update.
     * @param hosts_removed supplies the hosts removed since the last update.
     * @param weighted_priority_health if present, overwrites the current weighted_priority_health.
     * @param overprovisioning_factor if present, overwrites the current overprovisioning_factor.
     */
    virtual void updateHosts(uint32_t priority, UpdateHostsParams&& update_hosts_params,
                             LocalityWeightsConstSharedPtr locality_weights,
                             const HostVector& hosts_added, const HostVector& hosts_removed,
                             uint64_t seed, absl::optional<bool> weighted_priority_health,
                             absl::optional<uint32_t> overprovisioning_factor) PURE;
  };

  /**
   * Callback that provides the mechanism for performing batch host updates for a PrioritySet.
   */
  class BatchUpdateCb {
  public:
    virtual ~BatchUpdateCb() = default;

    /**
     * Performs a batch host update. Implementors should use the provided callback to update hosts
     * in the PrioritySet.
     */
    virtual void batchUpdate(HostUpdateCb& host_update_cb) PURE;
  };

  /**
   * Allows updating hosts for multiple priorities at once, deferring the MemberUpdateCb from
   * triggering until all priorities have been updated. The resulting callback will take into
   * account hosts moved from one priority to another.
   *
   * @param callback callback to use to add hosts.
   */
  virtual void batchHostUpdate(BatchUpdateCb& callback) PURE;
};

/**
 * All cluster config update related stats.
 * See https://github.com/envoyproxy/envoy/issues/23575 for details. Stats from ClusterInfo::stats()
 * will be split into subgroups "config-update", "lb", "endpoint" and "the rest"(which are mainly
 * upstream related), roughly based on their semantics.
 */
#define ALL_CLUSTER_CONFIG_UPDATE_STATS(COUNTER, GAUGE, HISTOGRAM, TEXT_READOUT, STATNAME)         \
  COUNTER(assignment_stale)                                                                        \
  COUNTER(assignment_timeout_received)                                                             \
  COUNTER(assignment_use_cached)                                                                   \
  COUNTER(update_attempt)                                                                          \
  COUNTER(update_empty)                                                                            \
  COUNTER(update_failure)                                                                          \
  COUNTER(update_no_rebuild)                                                                       \
  COUNTER(update_success)                                                                          \
  GAUGE(version, NeverImport)                                                                      \
  GAUGE(warming_state, NeverImport)

/**
 * All cluster endpoints related stats.
 */
#define ALL_CLUSTER_ENDPOINT_STATS(COUNTER, GAUGE, HISTOGRAM, TEXT_READOUT, STATNAME)              \
  GAUGE(max_host_weight, NeverImport)                                                              \
  COUNTER(membership_change)                                                                       \
  GAUGE(membership_degraded, NeverImport)                                                          \
  GAUGE(membership_excluded, NeverImport)                                                          \
  GAUGE(membership_healthy, NeverImport)                                                           \
  GAUGE(membership_total, NeverImport)

/**
 * All cluster load balancing related stats.
 */
#define ALL_CLUSTER_LB_STATS(COUNTER, GAUGE, HISTOGRAM, TEXT_READOUT, STATNAME)                    \
  COUNTER(lb_healthy_panic)                                                                        \
  COUNTER(lb_local_cluster_not_ok)                                                                 \
  COUNTER(lb_recalculate_zone_structures)                                                          \
  COUNTER(lb_subsets_created)                                                                      \
  COUNTER(lb_subsets_fallback)                                                                     \
  COUNTER(lb_subsets_fallback_panic)                                                               \
  COUNTER(lb_subsets_removed)                                                                      \
  COUNTER(lb_subsets_selected)                                                                     \
  COUNTER(lb_zone_cluster_too_small)                                                               \
  COUNTER(lb_zone_no_capacity_left)                                                                \
  COUNTER(lb_zone_routing_all_directly)                                                            \
  COUNTER(lb_zone_routing_cross_zone)                                                              \
  COUNTER(lb_zone_routing_sampled)                                                                 \
  GAUGE(lb_subsets_active, Accumulate)

/**
 * All cluster stats. @see stats_macros.h
 */
#define ALL_CLUSTER_TRAFFIC_STATS(COUNTER, GAUGE, HISTOGRAM, TEXT_READOUT, STATNAME)               \
  COUNTER(bind_errors)                                                                             \
  COUNTER(original_dst_host_invalid)                                                               \
  COUNTER(retry_or_shadow_abandoned)                                                               \
  COUNTER(upstream_cx_close_notify)                                                                \
  COUNTER(upstream_cx_connect_attempts_exceeded)                                                   \
  COUNTER(upstream_cx_connect_fail)                                                                \
  COUNTER(upstream_cx_connect_timeout)                                                             \
  COUNTER(upstream_cx_connect_with_0_rtt)                                                          \
  COUNTER(upstream_cx_destroy)                                                                     \
  COUNTER(upstream_cx_destroy_local)                                                               \
  COUNTER(upstream_cx_destroy_local_with_active_rq)                                                \
  COUNTER(upstream_cx_destroy_remote)                                                              \
  COUNTER(upstream_cx_destroy_remote_with_active_rq)                                               \
  COUNTER(upstream_cx_destroy_with_active_rq)                                                      \
  COUNTER(upstream_cx_http1_total)                                                                 \
  COUNTER(upstream_cx_http2_total)                                                                 \
  COUNTER(upstream_cx_http3_total)                                                                 \
  COUNTER(upstream_cx_idle_timeout)                                                                \
  COUNTER(upstream_cx_max_duration_reached)                                                        \
  COUNTER(upstream_cx_max_requests)                                                                \
  COUNTER(upstream_cx_none_healthy)                                                                \
  COUNTER(upstream_cx_overflow)                                                                    \
  COUNTER(upstream_cx_pool_overflow)                                                               \
  COUNTER(upstream_cx_protocol_error)                                                              \
  COUNTER(upstream_cx_rx_bytes_total)                                                              \
  COUNTER(upstream_cx_total)                                                                       \
  COUNTER(upstream_cx_tx_bytes_total)                                                              \
  COUNTER(upstream_flow_control_backed_up_total)                                                   \
  COUNTER(upstream_flow_control_drained_total)                                                     \
  COUNTER(upstream_flow_control_paused_reading_total)                                              \
  COUNTER(upstream_flow_control_resumed_reading_total)                                             \
  COUNTER(upstream_internal_redirect_failed_total)                                                 \
  COUNTER(upstream_internal_redirect_succeeded_total)                                              \
  COUNTER(upstream_rq_cancelled)                                                                   \
  COUNTER(upstream_rq_completed)                                                                   \
  COUNTER(upstream_rq_maintenance_mode)                                                            \
  COUNTER(upstream_rq_max_duration_reached)                                                        \
  COUNTER(upstream_rq_pending_failure_eject)                                                       \
  COUNTER(upstream_rq_pending_overflow)                                                            \
  COUNTER(upstream_rq_pending_total)                                                               \
  COUNTER(upstream_rq_0rtt)                                                                        \
  COUNTER(upstream_rq_per_try_timeout)                                                             \
  COUNTER(upstream_rq_per_try_idle_timeout)                                                        \
  COUNTER(upstream_rq_retry)                                                                       \
  COUNTER(upstream_rq_retry_backoff_exponential)                                                   \
  COUNTER(upstream_rq_retry_backoff_ratelimited)                                                   \
  COUNTER(upstream_rq_retry_limit_exceeded)                                                        \
  COUNTER(upstream_rq_retry_overflow)                                                              \
  COUNTER(upstream_rq_retry_success)                                                               \
  COUNTER(upstream_rq_rx_reset)                                                                    \
  COUNTER(upstream_rq_timeout)                                                                     \
  COUNTER(upstream_rq_total)                                                                       \
  COUNTER(upstream_rq_tx_reset)                                                                    \
  COUNTER(upstream_http3_broken)                                                                   \
  GAUGE(upstream_cx_active, Accumulate)                                                            \
  GAUGE(upstream_cx_rx_bytes_buffered, Accumulate)                                                 \
  GAUGE(upstream_cx_tx_bytes_buffered, Accumulate)                                                 \
  GAUGE(upstream_rq_active, Accumulate)                                                            \
  GAUGE(upstream_rq_pending_active, Accumulate)                                                    \
  HISTOGRAM(upstream_cx_connect_ms, Milliseconds)                                                  \
  HISTOGRAM(upstream_cx_length_ms, Milliseconds)

/**
 * All cluster load report stats. These are only use for EDS load reporting and not sent to the
 * stats sink. See envoy.config.endpoint.v3.ClusterStats for the definition of
 * total_dropped_requests and dropped_requests, which correspond to the upstream_rq_dropped and
 * upstream_rq_drop_overload counter here. These are latched by LoadStatsReporter, independent of
 * the normal stats sink flushing.
 */
#define ALL_CLUSTER_LOAD_REPORT_STATS(COUNTER, GAUGE, HISTOGRAM, TEXT_READOUT, STATNAME)           \
  COUNTER(upstream_rq_dropped)                                                                     \
  COUNTER(upstream_rq_drop_overload)

/**
 * Cluster circuit breakers gauges. Note that we do not generate a stats
 * structure from this macro. This is because depending on flags, we want to use
 * null gauges for all the "remaining" ones. This is hard to automate with the
 * 2-phase macros, so ClusterInfoImpl::generateCircuitBreakersStats is
 * hand-coded and must be changed if we alter the set of gauges in this macro.
 * We also include stat-names in this structure that are used when composing
 * the circuit breaker names, depending on priority settings.
 */
#define ALL_CLUSTER_CIRCUIT_BREAKERS_STATS(COUNTER, GAUGE, HISTOGRAM, TEXT_READOUT, STATNAME)      \
  GAUGE(cx_open, Accumulate)                                                                       \
  GAUGE(cx_pool_open, Accumulate)                                                                  \
  GAUGE(rq_open, Accumulate)                                                                       \
  GAUGE(rq_pending_open, Accumulate)                                                               \
  GAUGE(rq_retry_open, Accumulate)                                                                 \
  GAUGE(remaining_cx, Accumulate)                                                                  \
  GAUGE(remaining_cx_pools, Accumulate)                                                            \
  GAUGE(remaining_pending, Accumulate)                                                             \
  GAUGE(remaining_retries, Accumulate)                                                             \
  GAUGE(remaining_rq, Accumulate)                                                                  \
  STATNAME(circuit_breakers)                                                                       \
  STATNAME(default)                                                                                \
  STATNAME(high)

/**
 * All stats tracking request/response headers and body sizes. Not used by default.
 */
#define ALL_CLUSTER_REQUEST_RESPONSE_SIZE_STATS(COUNTER, GAUGE, HISTOGRAM, TEXT_READOUT, STATNAME) \
  HISTOGRAM(upstream_rq_headers_size, Bytes)                                                       \
  HISTOGRAM(upstream_rq_headers_count, Unspecified)                                                \
  HISTOGRAM(upstream_rq_body_size, Bytes)                                                          \
  HISTOGRAM(upstream_rs_headers_size, Bytes)                                                       \
  HISTOGRAM(upstream_rs_headers_count, Unspecified)                                                \
  HISTOGRAM(upstream_rs_body_size, Bytes)

/**
 * All stats around timeout budgets. Not used by default.
 */
#define ALL_CLUSTER_TIMEOUT_BUDGET_STATS(COUNTER, GAUGE, HISTOGRAM, TEXT_READOUT, STATNAME)        \
  HISTOGRAM(upstream_rq_timeout_budget_percent_used, Unspecified)                                  \
  HISTOGRAM(upstream_rq_timeout_budget_per_try_percent_used, Unspecified)

/**
 * Struct definition for cluster config update stats. @see stats_macros.h
 */
MAKE_STAT_NAMES_STRUCT(ClusterConfigUpdateStatNames, ALL_CLUSTER_CONFIG_UPDATE_STATS);
MAKE_STATS_STRUCT(ClusterConfigUpdateStats, ClusterConfigUpdateStatNames,
                  ALL_CLUSTER_CONFIG_UPDATE_STATS);

/**
 * Struct definition for cluster endpoint related stats. @see stats_macros.h
 */
MAKE_STAT_NAMES_STRUCT(ClusterEndpointStatNames, ALL_CLUSTER_ENDPOINT_STATS);
MAKE_STATS_STRUCT(ClusterEndpointStats, ClusterEndpointStatNames, ALL_CLUSTER_ENDPOINT_STATS);

/**
 * Struct definition for cluster load balancing stats. @see stats_macros.h
 */
MAKE_STAT_NAMES_STRUCT(ClusterLbStatNames, ALL_CLUSTER_LB_STATS);
MAKE_STATS_STRUCT(ClusterLbStats, ClusterLbStatNames, ALL_CLUSTER_LB_STATS);

/**
 * Struct definition for all cluster traffic stats. @see stats_macros.h
 */
MAKE_STAT_NAMES_STRUCT(ClusterTrafficStatNames, ALL_CLUSTER_TRAFFIC_STATS);
MAKE_STATS_STRUCT(ClusterTrafficStats, ClusterTrafficStatNames, ALL_CLUSTER_TRAFFIC_STATS);
using DeferredCreationCompatibleClusterTrafficStats =
    Stats::DeferredCreationCompatibleStats<ClusterTrafficStats>;

MAKE_STAT_NAMES_STRUCT(ClusterLoadReportStatNames, ALL_CLUSTER_LOAD_REPORT_STATS);
MAKE_STATS_STRUCT(ClusterLoadReportStats, ClusterLoadReportStatNames,
                  ALL_CLUSTER_LOAD_REPORT_STATS);

// We can't use macros to make the Stats class for circuit breakers due to
// the conditional inclusion of 'remaining' gauges. But we do auto-generate
// the StatNames struct.
MAKE_STAT_NAMES_STRUCT(ClusterCircuitBreakersStatNames, ALL_CLUSTER_CIRCUIT_BREAKERS_STATS);

MAKE_STAT_NAMES_STRUCT(ClusterRequestResponseSizeStatNames,
                       ALL_CLUSTER_REQUEST_RESPONSE_SIZE_STATS);
MAKE_STATS_STRUCT(ClusterRequestResponseSizeStats, ClusterRequestResponseSizeStatNames,
                  ALL_CLUSTER_REQUEST_RESPONSE_SIZE_STATS);

MAKE_STAT_NAMES_STRUCT(ClusterTimeoutBudgetStatNames, ALL_CLUSTER_TIMEOUT_BUDGET_STATS);
MAKE_STATS_STRUCT(ClusterTimeoutBudgetStats, ClusterTimeoutBudgetStatNames,
                  ALL_CLUSTER_TIMEOUT_BUDGET_STATS);

/**
 * Struct definition for cluster circuit breakers stats. @see stats_macros.h
 */
struct ClusterCircuitBreakersStats {
  ALL_CLUSTER_CIRCUIT_BREAKERS_STATS(c, GENERATE_GAUGE_STRUCT, h, tr, GENERATE_STATNAME_STRUCT)
};

using ClusterRequestResponseSizeStatsPtr = std::unique_ptr<ClusterRequestResponseSizeStats>;
using ClusterRequestResponseSizeStatsOptRef =
    absl::optional<std::reference_wrapper<ClusterRequestResponseSizeStats>>;

using ClusterTimeoutBudgetStatsPtr = std::unique_ptr<ClusterTimeoutBudgetStats>;
using ClusterTimeoutBudgetStatsOptRef =
    absl::optional<std::reference_wrapper<ClusterTimeoutBudgetStats>>;

/**
 * All extension protocol specific options returned by the method at
 *   NamedNetworkFilterConfigFactory::createProtocolOptions
 * must be derived from this class.
 */
class ProtocolOptionsConfig {
public:
  virtual ~ProtocolOptionsConfig() = default;
};
using ProtocolOptionsConfigConstSharedPtr = std::shared_ptr<const ProtocolOptionsConfig>;

/**
 *  Base class for all cluster typed metadata factory.
 */
class ClusterTypedMetadataFactory : public Envoy::Config::TypedMetadataFactory {};

class LoadBalancerConfig;
class TypedLoadBalancerFactory;

/**
 * This is a function used by upstream binding config to select the source address based on the
 * target address. Given the target address through the parameter expect the source address
 * returned.
 */
using AddressSelectFn = std::function<const Network::Address::InstanceConstSharedPtr(
    const Network::Address::InstanceConstSharedPtr&)>;

/**
 * Information about a given upstream cluster.
 * This includes the information and interfaces for building an upstream filter chain.
 */
class ClusterInfo : public Http::FilterChainFactory {
public:
  struct Features {
    // Whether the upstream supports HTTP2. This is used when creating connection pools.
    static constexpr uint64_t HTTP2 = 0x1;
    // Use the downstream protocol (HTTP1.1, HTTP2) for upstream connections as well, if available.
    // This is used when creating connection pools.
    static constexpr uint64_t USE_DOWNSTREAM_PROTOCOL = 0x2;
    // Whether connections should be immediately closed upon health failure.
    static constexpr uint64_t CLOSE_CONNECTIONS_ON_HOST_HEALTH_FAILURE = 0x4;
    // If USE_ALPN and HTTP2 are true, the upstream protocol will be negotiated using ALPN.
    // If ALPN is attempted but not supported by the upstream HTTP/1.1 is used.
    static constexpr uint64_t USE_ALPN = 0x8;
    // Whether the upstream supports HTTP3. This is used when creating connection pools.
    static constexpr uint64_t HTTP3 = 0x10;
  };

  ~ClusterInfo() override = default;

  /**
   * @return bool whether the cluster was added via API (if false the cluster was present in the
   *         initial configuration and cannot be removed or updated).
   */
  virtual bool addedViaApi() const PURE;

  /**
   * @return the connect timeout for upstream hosts that belong to this cluster.
   */
  virtual std::chrono::milliseconds connectTimeout() const PURE;

  /**
   * @return the idle timeout for upstream HTTP connection pool connections.
   */
  virtual const absl::optional<std::chrono::milliseconds> idleTimeout() const PURE;

  /**
   * @return the idle timeout for each connection in TCP connection pool.
   */
  virtual const absl::optional<std::chrono::milliseconds> tcpPoolIdleTimeout() const PURE;

  /**
   * @return optional maximum connection duration timeout for manager connections.
   */
  virtual const absl::optional<std::chrono::milliseconds> maxConnectionDuration() const PURE;

  /**
   * @return how many streams should be anticipated per each current stream.
   */
  virtual float perUpstreamPreconnectRatio() const PURE;

  /**
   * @return how many streams should be anticipated per each current stream.
   */
  virtual float peekaheadRatio() const PURE;

  /**
   * @return soft limit on size of the cluster's connections read and write buffers.
   */
  virtual uint32_t perConnectionBufferLimitBytes() const PURE;

  /**
   * @return uint64_t features supported by the cluster. @see Features.
   */
  virtual uint64_t features() const PURE;

  /**
   * @return const Http::Http1Settings& for HTTP/1.1 connections created on behalf of this cluster.
   *         @see Http::Http1Settings.
   */
  virtual const Http::Http1Settings& http1Settings() const PURE;

  /**
   * @return const envoy::config::core::v3::Http2ProtocolOptions& for HTTP/2 connections
   * created on behalf of this cluster.
   *         @see envoy::config::core::v3::Http2ProtocolOptions.
   */
  virtual const envoy::config::core::v3::Http2ProtocolOptions& http2Options() const PURE;

  /**
   * @return const envoy::config::core::v3::Http3ProtocolOptions& for HTTP/3 connections
   * created on behalf of this cluster. @see envoy::config::core::v3::Http3ProtocolOptions.
   */
  virtual const envoy::config::core::v3::Http3ProtocolOptions& http3Options() const PURE;

  /**
   * @return const envoy::config::core::v3::HttpProtocolOptions for all of HTTP versions.
   */
  virtual const envoy::config::core::v3::HttpProtocolOptions&
  commonHttpProtocolOptions() const PURE;

  /**
   * @param name std::string containing the well-known name of the extension for which protocol
   *        options are desired
   * @return std::shared_ptr<const Derived> where Derived is a subclass of ProtocolOptionsConfig
   *         and contains extension-specific protocol options for upstream connections.
   */
  template <class Derived>
  std::shared_ptr<const Derived> extensionProtocolOptionsTyped(const std::string& name) const {
    return std::dynamic_pointer_cast<const Derived>(extensionProtocolOptions(name));
  }

  /**
   * @return OptRef<const LoadBalancerConfig> the validated load balancing policy configuration to
   * use for this cluster.
   */
  virtual OptRef<const LoadBalancerConfig> loadBalancerConfig() const PURE;

  /**
   * @return the load balancer factory for this cluster. Cluster will always has a valid load
   * balancer factory if it is created successfully.
   */
  virtual TypedLoadBalancerFactory& loadBalancerFactory() const PURE;

  /**
   * @return const envoy::config::cluster::v3::Cluster::CommonLbConfig& the common configuration for
   * all load balancers for this cluster.
   */
  virtual const envoy::config::cluster::v3::Cluster::CommonLbConfig& lbConfig() const PURE;

  /**
   * @return the service discovery type to use for resolving the cluster.
   */
  virtual envoy::config::cluster::v3::Cluster::DiscoveryType type() const PURE;

  /**
   * @return the type of cluster, only used for custom discovery types.
   */
  virtual OptRef<const envoy::config::cluster::v3::Cluster::CustomClusterType>
  clusterType() const PURE;

  /**
   * @return const absl::optional<envoy::config::core::v3::TypedExtensionConfig>& the configuration
   *         for the upstream, if a custom upstream is configured.
   */
  virtual OptRef<const envoy::config::core::v3::TypedExtensionConfig> upstreamConfig() const PURE;

  /**
   * @return Whether the cluster is currently in maintenance mode and should not be routed to.
   *         Different filters may handle this situation in different ways. The implementation
   *         of this routine is typically based on randomness and may not return the same answer
   *         on each call.
   */
  virtual bool maintenanceMode() const PURE;

  /**
   * @return uint32_t the maximum number of outbound requests that a connection pool will make on
   *         each upstream connection. This can be used to increase spread if the backends cannot
   *         tolerate imbalance. 0 indicates no maximum.
   */
  virtual uint32_t maxRequestsPerConnection() const PURE;

  /**
   * @return uint32_t the maximum number of response headers. The default value is 100. Results in a
   * reset if the number of headers exceeds this value.
   */
  virtual uint32_t maxResponseHeadersCount() const PURE;

  /**
   * @return uint32_t the maximum total size of response headers in KB.
   */
  virtual absl::optional<uint16_t> maxResponseHeadersKb() const PURE;

  /**
   * @return the human readable name of the cluster.
   */
  virtual const std::string& name() const PURE;

  /**
   * @return the observability name associated to the cluster. Used in stats, tracing, logging, and
   * config dumps. The observability name is configured with :ref:`alt_stat_name
   * <envoy_api_field_config.cluster.v3.Cluster.alt_stat_name>`. If unprovided, the default value is
   * the cluster name.
   */
  virtual const std::string& observabilityName() const PURE;

  /**
   * @return ResourceManager& the resource manager to use by proxy agents for this cluster (at
   *         a particular priority).
   */
  virtual ResourceManager& resourceManager(ResourcePriority priority) const PURE;

  /**
   * @return TransportSocketMatcher& the transport socket matcher associated
   * factory.
   */
  virtual TransportSocketMatcher& transportSocketMatcher() const PURE;

  /**
   * @return ClusterConfigUpdateStats& config update stats for this cluster.
   */
  virtual ClusterConfigUpdateStats& configUpdateStats() const PURE;

  /**
   * @return ClusterLbStats& load-balancer-related stats for this cluster.
   */
  virtual ClusterLbStats& lbStats() const PURE;

  /**
   * @return ClusterEndpointStats& endpoint related stats for this cluster.
   */
  virtual ClusterEndpointStats& endpointStats() const PURE;

  /**
   * @return  all traffic related stats for this cluster.
   */
  virtual DeferredCreationCompatibleClusterTrafficStats& trafficStats() const PURE;
  /**
   * @return the stats scope that contains all cluster stats. This can be used to produce dynamic
   *         stats that will be freed when the cluster is removed.
   */
  virtual Stats::Scope& statsScope() const PURE;

  /**
   * @return ClusterLoadReportStats& load report stats for this cluster.
   */
  virtual ClusterLoadReportStats& loadReportStats() const PURE;

  /**
   * @return absl::optional<std::reference_wrapper<ClusterRequestResponseSizeStats>> stats to track
   * headers/body sizes of request/response for this cluster.
   */
  virtual ClusterRequestResponseSizeStatsOptRef requestResponseSizeStats() const PURE;

  /**
   * @return absl::optional<std::reference_wrapper<ClusterTimeoutBudgetStats>> stats on timeout
   * budgets for this cluster.
   */
  virtual ClusterTimeoutBudgetStatsOptRef timeoutBudgetStats() const PURE;

  /**
   * @return true if this cluster should produce per-endpoint stats.
   */
  virtual bool perEndpointStatsEnabled() const PURE;

  /**
   * @return std::shared_ptr<const UpstreamLocalAddressSelector> as upstream local address selector.
   */
  virtual UpstreamLocalAddressSelectorConstSharedPtr getUpstreamLocalAddressSelector() const PURE;

  /**
   * @return const envoy::config::core::v3::Metadata& the configuration metadata for this cluster.
   */
  virtual const envoy::config::core::v3::Metadata& metadata() const PURE;

  /**
   * @return const Envoy::Config::TypedMetadata&& the typed metadata for this cluster.
   */
  virtual const Envoy::Config::TypedMetadata& typedMetadata() const PURE;

  /**
   * @return whether to skip waiting for health checking before draining connections
   *         after a host is removed from service discovery.
   */
  virtual bool drainConnectionsOnHostRemoval() const PURE;

  /**
   *  @return whether to create a new connection pool for each downstream connection routed to
   *          the cluster
   */
  virtual bool connectionPoolPerDownstreamConnection() const PURE;

  /**
   * @return true if this cluster is configured to ignore hosts for the purpose of load balancing
   * computations until they have been health checked for the first time.
   */
  virtual bool warmHosts() const PURE;

  /**
   * @return true if this cluster is configured to set local interface name on upstream connections.
   */
  virtual bool setLocalInterfaceNameOnUpstreamConnections() const PURE;

  /**
   * @return const std::string& eds cluster service_name of the cluster. Empty if not an EDS
   * cluster or eds cluster service_name is not set.
   */
  virtual const std::string& edsServiceName() const PURE;

  /**
   * Create network filters on a new upstream connection.
   */
  virtual void createNetworkFilterChain(Network::Connection& connection) const PURE;

  /**
   * Calculate upstream protocol(s) based on features.
   */
  virtual std::vector<Http::Protocol>
  upstreamHttpProtocol(absl::optional<Http::Protocol> downstream_protocol) const PURE;

  /**
   * @return http protocol options for upstream connection
   */
  virtual const absl::optional<envoy::config::core::v3::UpstreamHttpProtocolOptions>&
  upstreamHttpProtocolOptions() const PURE;

  /**
   * @return alternate protocols cache options for upstream connections.
   */
  virtual const absl::optional<const envoy::config::core::v3::AlternateProtocolsCacheOptions>&
  alternateProtocolsCacheOptions() const PURE;

  /**
   * @return the Http1 Codec Stats.
   */
  virtual Http::Http1::CodecStats& http1CodecStats() const PURE;

  /**
   * @return the Http2 Codec Stats.
   */
  virtual Http::Http2::CodecStats& http2CodecStats() const PURE;

  /**
   * @return the Http3 Codec Stats.
   */
  virtual Http::Http3::CodecStats& http3CodecStats() const PURE;

  /**
   * @return create header validator based on cluster configuration. Returns nullptr if
   * ENVOY_ENABLE_UHV is undefined.
   */
  virtual Http::ClientHeaderValidatorPtr makeHeaderValidator(Http::Protocol protocol) const PURE;

  /**
   * @return OptRef<const envoy::config::cluster::v3::Cluster::HappyEyeballsConfig>
   * an optional value of the configuration for happy eyeballs for this cluster.
   */
  virtual OptRef<const envoy::config::cluster::v3::UpstreamConnectionOptions::HappyEyeballsConfig>
  happyEyeballsConfig() const PURE;

  /**
   * @return Reference to the optional config for LRS endpoint metric reporting.
   */
  virtual OptRef<const std::vector<std::string>> lrsReportMetricNames() const PURE;

protected:
  /**
   * Invoked by extensionProtocolOptionsTyped.
   * @param name std::string containing the well-known name of the extension for which protocol
   *        options are desired
   * @return ProtocolOptionsConfigConstSharedPtr with extension-specific protocol options for
   *         upstream connections.
   */
  virtual ProtocolOptionsConfigConstSharedPtr
  extensionProtocolOptions(const std::string& name) const PURE;
};

using ClusterInfoConstSharedPtr = std::shared_ptr<const ClusterInfo>;

class HealthChecker;

/**
 * An upstream cluster (group of hosts). This class is the "primary" singleton cluster used amongst
 * all forwarding threads/workers. Individual HostSets are used on the workers themselves.
 */
class Cluster {
public:
  virtual ~Cluster() = default;

  enum class InitializePhase { Primary, Secondary };

  /**
   * @return a pointer to the cluster's health checker. If a health checker has not been installed,
   *         returns nullptr.
   */
  virtual HealthChecker* healthChecker() PURE;

  /**
   * @return the information about this upstream cluster.
   */
  virtual ClusterInfoConstSharedPtr info() const PURE;

  /**
   * @return a pointer to the cluster's outlier detector. If an outlier detector has not been
   *         installed, returns nullptr.
   */
  virtual Outlier::Detector* outlierDetector() PURE;
  virtual const Outlier::Detector* outlierDetector() const PURE;

  /**
   * Initialize the cluster. This will be called either immediately at creation or after all primary
   * clusters have been initialized (determined via initializePhase()).
   * @param callback supplies a callback that will be invoked after the cluster has undergone first
   *        time initialization. E.g., for a dynamic DNS cluster the initialize callback will be
   *        called when initial DNS resolution is complete.
   */
  virtual void initialize(std::function<absl::Status()> callback) PURE;

  /**
   * @return the phase in which the cluster is initialized at boot. This mechanism is used such that
   *         clusters that depend on other clusters can correctly initialize. (E.g., an EDS cluster
   *         that depends on resolution of the EDS server itself).
   */
  virtual InitializePhase initializePhase() const PURE;

  /**
   * @return the PrioritySet for the cluster.
   */
  virtual PrioritySet& prioritySet() PURE;

  /**
   * @return the const PrioritySet for the cluster.
   */
  virtual const PrioritySet& prioritySet() const PURE;

  /**
   * @return the cluster drop_overload configuration.
   */
  virtual UnitFloat dropOverload() const PURE;

  /**
   * @return the cluster drop_category_ configuration.
   */
  virtual const std::string& dropCategory() const PURE;

  /**
   * Set up the drop_overload value for the cluster.
   */
  virtual void setDropOverload(UnitFloat drop_overload) PURE;

  /**
   * Set up the drop_category value for the thread local cluster.
   */
  virtual void setDropCategory(absl::string_view drop_category) PURE;
};

using ClusterSharedPtr = std::shared_ptr<Cluster>;
using ClusterConstOptRef = absl::optional<std::reference_wrapper<const Cluster>>;

} // namespace Upstream
} // namespace Envoy

// NOLINT(namespace-envoy)
namespace fmt {

// fmt formatter class for Host
template <> struct formatter<Envoy::Upstream::Host> : formatter<absl::string_view> {
  template <typename FormatContext>
  auto format(const Envoy::Upstream::Host& host, FormatContext& ctx) const -> decltype(ctx.out()) {
    absl::string_view out = !host.hostname().empty() ? host.hostname()
                            : host.address()         ? host.address()->asStringView()
                                                     : "<empty>";
    return formatter<absl::string_view>().format(out, ctx);
  }
};

} // namespace fmt
