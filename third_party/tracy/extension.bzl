"""Fetch Tracy client sources (v0.12.1, same pin as DEVOPS.ccu-stack)."""

load("@bazel_tools//tools/build_defs/repo:http.bzl", "http_archive")

def _tracy_ext(_ctx):
    http_archive(
        name = "tracy",
        build_file = Label("//third_party/tracy:tracy.BUILD"),
        sha256 = "03580b01df3c435f74eec165193d6557cdbf3a84d39582ca30969ef5354560aa",
        strip_prefix = "tracy-0.12.1",
        url = "https://github.com/wolfpld/tracy/archive/refs/tags/v0.12.1.tar.gz",
    )

tracy_ext = module_extension(
    implementation = _tracy_ext,
)
