load(
    "//bazel:envoy_build_system.bzl",
    "envoy_cc_extension",
    "envoy_extension_package",
)

licenses(["notice"])  # Apache 2

# Access log implementation that writes to a file.
# Public docs: docs/root/configuration/access_log.rst

envoy_extension_package()

envoy_cc_extension(
    name = "config",
    srcs = ["config.cc"],
    hdrs = ["config.h"],
    # TODO(#9953) determine if this is core or should be cleaned up.
    extra_visibility = [
        "//test:__subpackages__",
    ],
    deps = [
        "//include/envoy/registry",
        "//source/common/config:config_provider_lib",
        "//source/common/formatter:substitution_format_string_lib",
        "//source/common/protobuf",
        "//source/extensions/access_loggers/common:file_access_log_lib",
        "@envoy_api//envoy/extensions/access_loggers/file/v3:pkg_cc_proto",
    ],
)
