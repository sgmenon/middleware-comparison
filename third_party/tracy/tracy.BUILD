load("@bazel_skylib//rules:common_settings.bzl", "bool_flag")
load("@rules_cc//cc:defs.bzl", "cc_library")

licenses(["notice"])  # BSD 3-Clause

package(default_visibility = ["//visibility:public"])

bool_flag(
    name = "tracy_enabled",
    build_setting_default = False,
)

config_setting(
    name = "is_tracy_enabled",
    flag_values = {":tracy_enabled": "True"},
)

cc_library(
    name = "tracy",
    srcs = select({
        ":is_tracy_enabled": ["public/TracyClient.cpp"],
        "//conditions:default": [],
    }),
    hdrs = [
        "public/common/TracyApi.h",
        "public/common/TracyColor.hpp",
        "public/common/TracySystem.hpp",
        "public/tracy/Tracy.hpp",
        "public/tracy/TracyC.h",
    ] + select({
        ":is_tracy_enabled": [
            "public/client/TracyAlloc.cpp",
            "public/client/TracyArmCpuTable.hpp",
            "public/client/TracyCallstack.cpp",
            "public/client/TracyCallstack.h",
            "public/client/TracyCallstack.hpp",
            "public/client/TracyCpuid.hpp",
            "public/client/TracyDebug.hpp",
            "public/client/TracyDxt1.cpp",
            "public/client/TracyDxt1.hpp",
            "public/client/TracyFastVector.hpp",
            "public/client/TracyKCore.cpp",
            "public/client/TracyKCore.hpp",
            "public/client/TracyLock.hpp",
            "public/client/TracyOverride.cpp",
            "public/client/TracyProfiler.cpp",
            "public/client/TracyProfiler.hpp",
            "public/client/TracyRingBuffer.hpp",
            "public/client/TracyScoped.hpp",
            "public/client/TracyStringHelpers.hpp",
            "public/client/TracySysPower.cpp",
            "public/client/TracySysPower.hpp",
            "public/client/TracySysTime.cpp",
            "public/client/TracySysTime.hpp",
            "public/client/TracySysTrace.cpp",
            "public/client/TracySysTrace.hpp",
            "public/client/TracyThread.hpp",
            "public/client/tracy_SPSCQueue.h",
            "public/client/tracy_concurrentqueue.h",
            "public/client/tracy_rpmalloc.cpp",
            "public/client/tracy_rpmalloc.hpp",
            "public/common/TracyAlign.hpp",
            "public/common/TracyAlloc.hpp",
            "public/common/TracyForceInline.hpp",
            "public/common/TracyMutex.hpp",
            "public/common/TracyProtocol.hpp",
            "public/common/TracyQueue.hpp",
            "public/common/TracySocket.cpp",
            "public/common/TracySocket.hpp",
            "public/common/TracyStackFrames.cpp",
            "public/common/TracyStackFrames.hpp",
            "public/common/TracySystem.cpp",
            "public/common/TracyUwp.hpp",
            "public/common/TracyYield.hpp",
            "public/common/tracy_lz4.cpp",
            "public/common/tracy_lz4.hpp",
            "public/libbacktrace/alloc.cpp",
            "public/libbacktrace/backtrace.hpp",
            "public/libbacktrace/config.h",
            "public/libbacktrace/dwarf.cpp",
            "public/libbacktrace/elf.cpp",
            "public/libbacktrace/fileline.cpp",
            "public/libbacktrace/filenames.hpp",
            "public/libbacktrace/internal.hpp",
            "public/libbacktrace/mmapio.cpp",
            "public/libbacktrace/posix.cpp",
            "public/libbacktrace/sort.cpp",
            "public/libbacktrace/state.cpp",
        ],
        "//conditions:default": [],
    }),
    defines = [
        "NO_PARALLEL_SORT",
        "TRACY_NO_EXIT",
    ] + select({
        ":is_tracy_enabled": ["TRACY_ENABLE"],
        "//conditions:default": [],
    }),
    includes = [
        "public",
        "public/client",
    ],
    linkopts = select({
        "@platforms//os:linux": ["-ldl"],
        "//conditions:default": [],
    }),
    deps = ["@zstd"],
)
