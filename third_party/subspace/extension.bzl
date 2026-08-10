"""Generate a repository for the Subspace library"""

load("@bazel_tools//tools/build_defs/repo:http.bzl", "http_archive")

def _subspace_ext(_ctx):
    http_archive(
        name = "subspace",
        integrity = "sha256-FyhLG3FQpXhK7Yy9aMNvf+jpWOsZ+7xia8nQJltR8Ms=",
        strip_prefix = "subspace-2.9.1",
        urls = ["https://github.com/dallison/subspace/archive/refs/tags/2.9.1.tar.gz"],
    )

    # Toolbelt
    http_archive(
        name = "cpp_toolbelt",
        integrity = "sha256-cRCJxgfiKS5RAbjm7OWyMxYI6qRrcXoX8lpuedtbVGQ=",
        strip_prefix = "cpp_toolbelt-2.1.3",
        urls = ["https://github.com/dallison/cpp_toolbelt/archive/refs/tags/2.1.3.tar.gz"],
    )

    # Coroutines
    http_archive(
        name = "coroutines",
        integrity = "sha256-VyXQL9hLMw1UPlZleGPDl+syaX1LN209FvFz27yqWo4=",
        strip_prefix = "co-3.3.2",
        urls = ["https://github.com/dallison/co/archive/refs/tags/3.3.2.tar.gz"],
    )

subspace_ext = module_extension(
    implementation = _subspace_ext,
)
