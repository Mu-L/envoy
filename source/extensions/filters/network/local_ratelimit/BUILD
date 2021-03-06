load(
    "//bazel:envoy_build_system.bzl",
    "envoy_cc_extension",
    "envoy_cc_library",
    "envoy_extension_package",
)

licenses(["notice"])  # Apache 2

# Local ratelimit L4 network filter
# Public docs: docs/root/configuration/network_filters/local_rate_limit_filter.rst

envoy_extension_package()

envoy_cc_library(
    name = "local_ratelimit_lib",
    srcs = ["local_ratelimit.cc"],
    hdrs = ["local_ratelimit.h"],
    deps = [
        "//include/envoy/event:dispatcher_interface",
        "//include/envoy/event:timer_interface",
        "//include/envoy/network:filter_interface",
        "//include/envoy/runtime:runtime_interface",
        "//include/envoy/stats:stats_macros",
        "//source/common/protobuf:utility_lib",
        "//source/common/runtime:runtime_lib",
        "//source/extensions/filters/common/local_ratelimit:local_ratelimit_lib",
        "@envoy_api//envoy/extensions/filters/network/local_ratelimit/v3:pkg_cc_proto",
    ],
)

envoy_cc_extension(
    name = "config",
    srcs = ["config.cc"],
    hdrs = ["config.h"],
    deps = [
        "//source/extensions/filters/network:well_known_names",
        "//source/extensions/filters/network/common:factory_base_lib",
        "//source/extensions/filters/network/local_ratelimit:local_ratelimit_lib",
        "@envoy_api//envoy/extensions/filters/network/local_ratelimit/v3:pkg_cc_proto",
    ],
)
