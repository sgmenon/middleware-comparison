""" Builds CycloneDDS.
"""

load("@bazel_skylib//lib:dicts.bzl", "dicts")
load("@bazel_skylib//lib:selects.bzl", "selects")
load("@bazel_skylib//rules:common_settings.bzl", "bool_flag")
load("@rules_foreign_cc//foreign_cc:defs.bzl", "cmake")

bool_flag(
    name = "enable_shm",
    build_setting_default = True,
)

config_setting(
    name = "enable_shm_on",
    flag_values = {":enable_shm": "True"},
    visibility = ["//visibility:public"],
)

config_setting(
    name = "enable_shm_off",
    flag_values = {":enable_shm": "False"},
    visibility = ["//visibility:public"],
)

selects.config_setting_group(
    name = "linux_or_macos",
    match_any = [
        "@platforms//os:linux",
        "@platforms//os:macos",
    ],
)

selects.config_setting_group(
    name = "linux_or_macos_with_shm",
    match_all = [
        ":linux_or_macos",
        ":enable_shm_on",
    ],
)

selects.config_setting_group(
    name = "android_with_shm",
    match_all = [
        "@platforms//os:android",
        ":enable_shm_on",
    ],
)

selects.config_setting_group(
    name = "qnx_with_shm",
    match_all = [
        "@platforms//os:qnx",
        ":enable_shm_on",
    ],
)

selects.config_setting_group(
    name = "linux_or_macos_without_shm",
    match_all = [
        ":linux_or_macos",
        ":enable_shm_off",
    ],
)

selects.config_setting_group(
    name = "android_without_shm",
    match_all = [
        "@platforms//os:android",
        ":enable_shm_off",
    ],
)

selects.config_setting_group(
    name = "qnx_without_shm",
    match_all = [
        "@platforms//os:qnx",
        ":enable_shm_off",
    ],
)

filegroup(
    name = "all_srcs",
    srcs = glob(["**"]),
)

cache_entries = {
    # CycloneDDS specific options.
    "BUILD_DOCS": "OFF",
    "BUILD_EXAMPLES": "OFF",
    "BUILD_IDLLIB": "OFF",
    "BUILD_SHARED_LIBS": "OFF",
    "BUILD_TESTING": "OFF",
    "CMAKE_POSITION_INDEPENDENT_CODE": "ON",  # Must be set!
    "CycloneDDS_DIR": "%cyclonedds_prefix%",
    "ENABLE_COVERAGE": "OFF",
    "ENABLE_LEGACY": "OFF",
    "ENABLE_TOPIC_DISCOVERY": "ON",
    "ENABLE_TYPE_DISCOVERY": "ON",
}

cache_entries_linux_and_macos = {
    "ENABLE_SOURCE_SPECIFIC_MULTICAST": "ON",
}

cache_entries_android_and_qnx = {
    "ENABLE_IPV6": "OFF",
    "ENABLE_SOURCE_SPECIFIC_MULTICAST": "OFF",
}

cache_entries_android = {
    "CMAKE_SYSTEM_NAME": "Android",
}

cache_entries_qnx = {
    "CMAKE_SYSTEM_NAME": "QNX",
}

cache_entries_with_shm = {
    "CMAKE_PREFIX_PATH": "%iceoryx_prefix%",
    "ENABLE_SHM": "ON",
}

cache_entries_without_shm = {
    "ENABLE_SHM": "OFF",
}

# see docs at https://bazel-contrib.github.io/rules_foreign_cc/cmake.html
cmake(
    name = "cyclonedds-cxx",
    build_args = [
        "--",
        "-j8",
    ],
    cache_entries = select(
        {
            ":android_with_shm": dicts.add(
                cache_entries,
                cache_entries_android,
                cache_entries_android_and_qnx,
                cache_entries_with_shm,
            ),
            ":android_without_shm": dicts.add(
                cache_entries,
                cache_entries_android,
                cache_entries_android_and_qnx,
                cache_entries_without_shm,
            ),
            ":linux_or_macos_with_shm": dicts.add(
                cache_entries,
                cache_entries_linux_and_macos,
                cache_entries_with_shm,
            ),
            ":linux_or_macos_without_shm": dicts.add(
                cache_entries,
                cache_entries_linux_and_macos,
                cache_entries_without_shm,
            ),
            ":qnx_with_shm": dicts.add(
                cache_entries,
                cache_entries_qnx,
                cache_entries_android_and_qnx,
                cache_entries_with_shm,
            ),
            ":qnx_without_shm": dicts.add(
                cache_entries,
                cache_entries_qnx,
                cache_entries_android_and_qnx,
                cache_entries_without_shm,
            ),
        },
        no_match_error = "Unsupported build configuration",
    ),
    copts = [
        "-w",
    ],
    features = [
        #messses with foreign_cc
        "-dependency_file",
    ],
    generate_args = ["-GNinja"],
    lib_source = ":all_srcs",
    linkopts = selects.with_or(
        {
            ":linux_or_macos": ["-lpthread"],
            (
                "@platforms//os:android",
                "@platforms//os:qnx",
            ): [],
        },
        no_match_error = "Supported OSs: Android, Linux, macOS, QNX",
    ),
    out_include_dir = "include/ddscxx",
    out_static_libs = ["libddscxx.a"],
    visibility = ["//visibility:public"],
    deps = select({
        ":enable_shm_on": ["@iceoryx"],
        "//conditions:default": [],
    }) + ["@cyclonedds"],
)

cmake(
    name = "idlcxx",
    build_args = [
        "--",
        "-j8",
    ],
    cache_entries = cache_entries.update({
        "BUILD_IDLLIB": "ON",
    }),
    copts = [
        "-w",
    ],
    env = select({
        "@platforms//os:qnx": {
            "QNX_CONFIGURATION_EXCLUSIVE": "external/sdv2-toolchains++toolchains_qnx+acp_qnx_aarch64_sdp/host/linux/x86_64/.qnx",
            "QNX_HOST": "external/sdv2-toolchains++toolchains_qnx+acp_qnx_aarch64_sdp/host/linux/x86_64",
            "QNX_SHARED_LICENSE_FILE": "external/sdv2-toolchains++toolchains_qnx+acp_qnx_aarch64_sdp/host/linux/x86_64/.qnx/license/licenses",
            "QNX_TARGET": "external/sdv2-toolchains++toolchains_qnx+acp_qnx_aarch64_sdp/target/qnx7",
        },
        "//conditions:default": {},
    }),
    features = [
        #messses with foreign_cc
        "-dependency_file",
    ],
    generate_args = ["-GNinja"],
    lib_source = ":all_srcs",
    out_shared_libs = [
        "libcycloneddsidlcxx.so",
    ],
    visibility = ["//visibility:public"],
    deps = ["@cyclonedds"],
)
