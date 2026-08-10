# trunk-ignore-all(buildifier/module-docstring)

def _get_iceoryx_impl(rctx):
    rctx.download_and_extract(
        strip_prefix = "iceoryx-2.0.5",
        sha256 = "bf6de70e3edee71223f993a29bff5e61af95ce4871104929d8bd1729f544bafb",
        url = "https://github.com/eclipse-iceoryx/iceoryx/archive/refs/tags/v2.0.5.tar.gz",
    )
    rctx.template(
        "BUILD.bazel",
        Label("@mw_benchmark//third_party/iceoryx:iceoryx.BUILD"),
        substitutions = {},
    )

get_iceoryx = repository_rule(
    implementation = _get_iceoryx_impl,
    attrs = {
    },
)

def _non_module_dependencies_impl(_ctx):
    get_iceoryx(name = "iceoryx")

third_party = module_extension(
    implementation = _non_module_dependencies_impl,
)
