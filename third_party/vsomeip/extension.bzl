"""Fetch two vsomeip trees for A/B benchmarking (fork improvements vs baseline)."""

load("@bazel_tools//tools/build_defs/repo:http.bzl", "http_archive")

# Latest main @ https://github.com/sgmenon/vsomeip (refresh integrity when moving SHA).
_VSOMEIP_SGMENON_COMMIT = "598f3a79abe7dcba0cdbabdf3737811d5c47f6ba"

# Upstream-style baseline pinned in the fork history (COVESA public archive 404 for this SHA).
_VSOMEIP_COVESA_COMMIT = "e235a3037052340ac51b278385da4a4363e28689"

def _vsomeip_ext(_ctx):
    http_archive(
        name = "vsomeip_sgmenon",
        integrity = "sha256-4IHqGAStSGM0NBh4N4Kh/xqgbIxZZZCP7Jlk2WCtw5c=",
        strip_prefix = "vsomeip-{}".format(_VSOMEIP_SGMENON_COMMIT),
        urls = [
            "https://github.com/sgmenon/vsomeip/archive/{}.tar.gz".format(_VSOMEIP_SGMENON_COMMIT),
        ],
    )
    http_archive(
        name = "vsomeip_covesa",
        integrity = "sha256-SeLReaREL6P1/j3WprHNqBTbbsOasrTtYwDby7TZfl0=",
        patch_args = ["-p1"],
        patches = [
            Label("//third_party/vsomeip:covesa_bazel_cc_import.patch"),
            Label("//third_party/vsomeip:covesa_routingmanagerd_build.patch"),
        ],
        strip_prefix = "vsomeip-{}".format(_VSOMEIP_COVESA_COMMIT),
        urls = [
            "https://github.com/sgmenon/vsomeip/archive/{}.tar.gz".format(_VSOMEIP_COVESA_COMMIT),
        ],
    )

vsomeip_ext = module_extension(
    implementation = _vsomeip_ext,
)
