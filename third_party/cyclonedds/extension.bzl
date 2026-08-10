# trunk-ignore-all(buildifier/module-docstring)

def _get_cyclonedds_impl(rctx):
    rctx.download_and_extract(
        sha256 = "ec3ec898c52b02f939a969cd1a276e219420e5e8419b21cea276db35b4821848",
        strip_prefix = "cyclonedds-0.10.5",
        url = "https://github.com/eclipse-cyclonedds/cyclonedds/archive/refs/tags/0.10.5.tar.gz",
    )
    rctx.template(
        "BUILD.bazel",
        Label("@mw_benchmark//third_party/cyclonedds:cyclonedds.BUILD"),
        substitutions = {
            "%iceoryx_prefix%": rctx.attr._iceoryx.workspace_root,
        },
    )

def _get_cyclonedds_cxx_impl(rctx):
    rctx.download_and_extract(
        sha256 = "94a6287f617e689690fc17a068b60be1ae003099702d3e1a95162f74edb88442",
        strip_prefix = "cyclonedds-cxx-0.10.5",
        url = "https://github.com/eclipse-cyclonedds/cyclonedds-cxx/archive/refs/tags/0.10.5.tar.gz",
    )
    rctx.template(
        "BUILD.bazel",
        Label("@mw_benchmark//third_party/cyclonedds:cyclonedds-cxx.BUILD"),
        substitutions = {
            "%cyclonedds_prefix%": rctx.attr._cyclonedds.workspace_root,
            "%iceoryx_prefix%": rctx.attr._iceoryx.workspace_root,
        },
    )

get_cyclonedds = repository_rule(
    implementation = _get_cyclonedds_impl,
    attrs = {
        "_iceoryx": attr.label(
            default = Label("@iceoryx"),
        ),
    },
)

get_cyclonedds_cxx = repository_rule(
    implementation = _get_cyclonedds_cxx_impl,
    attrs = {
        "_cyclonedds": attr.label(
            default = Label("@cyclonedds"),
        ),
        "_iceoryx": attr.label(
            default = Label("@iceoryx"),
        ),
    },
)

def _non_module_dependencies_impl(_rctx):
    get_cyclonedds(
        name = "cyclonedds",
    )
    get_cyclonedds_cxx(
        name = "cyclonedds-cxx",
    )

third_party = module_extension(
    implementation = _non_module_dependencies_impl,
)
