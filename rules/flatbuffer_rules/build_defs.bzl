"""BUILD rules for generating flatbuffer files."""

load("@rules_cc//cc:defs.bzl", "cc_library")
load("@rules_python//python:py_library.bzl", "py_library")

flatc_path = "@flatbuffers//:flatc"

DEFAULT_INCLUDE_PATHS = [
    "./",
    "$(GENDIR)",
    "$(BINDIR)",
]

DEFAULT_FLATC_ARGS = [
    "--no-union-value-namespacing",
    "--gen-object-api",
]

def flatbuffer_library_public(
        name,
        srcs,
        outs,
        language_flag,
        out_prefix = "",
        includes = [],
        include_paths = [],
        compatible_with = [],
        flatc_args = DEFAULT_FLATC_ARGS,
        reflection_name = "",
        output_to_bindir = False):
    """Generates code files for reading/writing the given flatbuffers in the requested language using the public compiler.

    Outs:
      filegroup(name): all generated source files.
      Fileset([reflection_name]): (Optional) all generated reflection binaries.

    Args:
      name: Rule name.
      srcs: Source .fbs files. Sent in order to the compiler.
      outs: Output files from flatc.
      language_flag: Target language flag. One of [-c, -j, -js].
      out_prefix: Prepend this path to the front of all generated files except on
          single source targets. Usually is a directory name.
      includes: Optional, list of filegroups of schemas that the srcs depend on.
      include_paths: Optional, list of paths the includes files can be found in.
      compatible_with: Optional, passed to genrule for environments this rule
          can be built for.
      flatc_args: Optional, list of additional arguments to pass to flatc.
      reflection_name: Optional, if set this will generate the flatbuffer
        reflection binaries for the schemas.
      output_to_bindir: Passed to genrule for output to bin directory.
    """
    include_paths_cmd = ["-I %s" % (s) for s in include_paths]

    # '$(@D)' when given a single source target will give the appropriate
    # directory. Appending 'out_prefix' is only necessary when given a build
    # target with multiple sources.
    output_directory = (
        ("-o $(@D)/%s" % (out_prefix)) if len(srcs) > 1 else ("-o $(@D)")
    )
    genrule_cmd = " ".join([
        "for f in $(SRCS); do",
        "$(location %s)" % (flatc_path),
        " ".join(flatc_args),
        " ".join(include_paths_cmd),
        language_flag,
        output_directory,
        "$$f;",
        "done",
    ])
    native.genrule(
        name = name,
        srcs = srcs,
        outs = outs,
        output_to_bindir = output_to_bindir,
        compatible_with = compatible_with,
        tools = includes + [flatc_path],
        cmd = genrule_cmd,
        message = "Generating flatbuffer files for %s:" % (name),
    )
    if reflection_name:
        reflection_genrule_cmd = " ".join([
            "for f in $(SRCS); do",
            "$(location %s)" % (flatc_path),
            "-b --schema",
            " ".join(flatc_args),
            " ".join(include_paths_cmd),
            language_flag,
            output_directory,
            "$$f;",
            "done",
        ])
        reflection_outs = [
            (out_prefix + "%s.bfbs") % (s.replace(".fbs", "").split("/")[-1])
            for s in srcs
        ]
        native.genrule(
            name = "%s_srcs" % reflection_name,
            srcs = srcs,
            outs = reflection_outs,
            output_to_bindir = output_to_bindir,
            compatible_with = compatible_with,
            tools = includes + [flatc_path],
            cmd = reflection_genrule_cmd,
            message = "Generating flatbuffer reflection binary for %s:" % (name),
        )
        # TODO(b/114456773): Make bazel rules proper and supported by flatbuffer
        # Have to comment this since FilesetEntry is not supported in bazel
        # starlark.
        # native.Fileset(
        #     name = reflection_name,
        #     out = "%s_out" % reflection_name,
        #     entries = [
        #         native.FilesetEntry(files = reflection_outs),
        #     ],
        #     visibility = reflection_visibility,
        #     compatible_with = compatible_with,
        # )

def flatbuffer_cc_library(
        name,
        srcs,
        srcs_filegroup_name = "",
        out_prefix = "",
        includes = [],
        include_paths = [],
        compatible_with = [],
        flatc_args = DEFAULT_FLATC_ARGS,
        visibility = None,
        srcs_filegroup_visibility = None,
        gen_reflections = False):
    '''A cc_library with the generated reader/writers for the given flatbuffer definitions.

    Outs:
      filegroup([name]_srcs): all generated .h files.
      filegroup(srcs_filegroup_name if specified, or [name]_includes if not):
          Other flatbuffer_cc_library's can pass this in for their `includes`
          parameter, if they depend on the schemas in this library.
      Fileset([name]_reflection): (Optional) all generated reflection binaries.
      cc_library([name]): library with sources and flatbuffers deps.

    Remarks:
      ** Because the genrule used to call flatc does not have any trivial way of
        computing the output list of files transitively generated by includes and
        --gen-includes (the default) being defined for flatc, the --gen-includes
        flag will not work as expected. The way around this is to add a dependency
        to the flatbuffer_cc_library defined alongside the flatc included Fileset.
        For example you might define:

        flatbuffer_cc_library(
            name = "my_fbs",
            srcs = [ "schemas/foo.fbs" ],
            includes = [ "//third_party/bazz:bazz_fbs_includes" ],
        )

        In which foo.fbs includes a few files from the Fileset defined at
        //third_party/bazz:bazz_fbs_includes. When compiling the library that
        includes foo_generated.h, and therefore has my_fbs as a dependency, it
        will fail to find any of the bazz *_generated.h files unless you also
        add bazz's flatbuffer_cc_library to your own dependency list, e.g.:

        cc_library(
            name = "my_lib",
            deps = [
                ":my_fbs",
                "//third_party/bazz:bazz_fbs"
            ],
        )

        Happy dependent Flatbuffering!

    Args:
      name: Rule name.
      srcs: Source .fbs files. Sent in order to the compiler.
      srcs_filegroup_name: Name of the output filegroup that holds srcs. Pass this
          filegroup into the `includes` parameter of any other
          flatbuffer_cc_library that depends on this one's schemas.
      out_prefix: Prepend this path to the front of all generated files. Usually
          is a directory name.
      includes: Optional, list of filegroups of schemas that the srcs depend on.
          ** SEE REMARKS BELOW **
      include_paths: Optional, list of paths the includes files can be found in.
      compatible_with: Optional, passed to genrule for environments this rule
          can be built for
      flatc_args: Optional list of additional arguments to pass to flatc
          (e.g. --gen-mutable).
      visibility: The visibility of the generated cc_library. By default, use the
          default visibility of the project.
      srcs_filegroup_visibility: The visibility of the generated srcs filegroup.
          By default, use the value of the visibility parameter above.
      gen_reflections: Optional, if true this will generate the flatbuffer
        reflection binaries for the schemas.
    '''
    output_headers = [
        (out_prefix + "%s_generated.h") % (s.replace(".fbs", "").split("/")[-1].split(":")[-1])
        for s in srcs
    ]
    reflection_name = "%s_reflection" % name if gen_reflections else ""

    flatbuffer_library_public(
        name = "%s_srcs" % (name),
        srcs = srcs,
        outs = output_headers,
        language_flag = "-c",
        out_prefix = out_prefix,
        includes = includes,
        include_paths = include_paths,
        compatible_with = compatible_with,
        flatc_args = flatc_args,
        reflection_name = reflection_name,
    )
    cc_library(
        name = name,
        hdrs = output_headers,
        srcs = output_headers,
        features = [
            "-parse_headers",
        ],
        deps = [
            "@flatbuffers//:runtime_cc",
        ],
        includes = ["."],
        linkstatic = 1,
        visibility = visibility,
        compatible_with = compatible_with,
    )

    # A filegroup for the `srcs`. That is, all the schema files for this
    # Flatbuffer set.
    native.filegroup(
        name = srcs_filegroup_name if srcs_filegroup_name else "%s_includes" % (name),
        srcs = srcs,
        visibility = srcs_filegroup_visibility if srcs_filegroup_visibility != None else visibility,
        compatible_with = compatible_with,
    )

# Custom provider to track dependencies transitively.
FlatbufferInfo = provider(
    fields = {
        "transitive_srcs": "flatbuffer schema definitions.",
    },
    doc = "Flat Buffer Schema Definition Provider",
)

def _flatbuffer_schemas_aspect_impl(_target, ctx):
    transitive_srcs = depset()
    if hasattr(ctx.rule.attr, "deps"):
        for dep in ctx.rule.attr.deps:
            if FlatbufferInfo in dep:
                # trunk-ignore(buildifier/overly-nested-depset)
                transitive_srcs = depset(dep[FlatbufferInfo].transitive_srcs, transitive = [transitive_srcs])
    if hasattr(ctx.rule.attr, "srcs"):
        for src in ctx.rule.attr.srcs:
            if FlatbufferInfo in src:
                # trunk-ignore(buildifier/overly-nested-depset)
                transitive_srcs = depset(src[FlatbufferInfo].transitive_srcs, transitive = [transitive_srcs])
            for f in src.files:
                if f.extension == "fbs":
                    # trunk-ignore(buildifier/overly-nested-depset)
                    transitive_srcs = depset([f], transitive = [transitive_srcs])
    return [FlatbufferInfo(transitive_srcs = transitive_srcs)]

# An aspect that runs over all dependencies and transitively collects
# flatbuffer schema files.
_flatbuffer_schemas_aspect = aspect(
    attr_aspects = [
        "deps",
        "srcs",
    ],
    implementation = _flatbuffer_schemas_aspect_impl,
)

# Rule to invoke the flatbuffer compiler.
def _gen_flatbuffer_srcs_impl(ctx):
    if not ctx.attr.output:
        output_dir = ctx.actions.declare_directory("{}_all".format(ctx.attr.name))
    else:
        output_dir = ctx.actions.declare_directory(ctx.attr.output)

    flatc_args = [
        ctx.attr.language_flag,
        "-o",
        output_dir.path,  # Output directory for generated files
        "-I",
        "./",
        "-I",
        ctx.genfiles_dir.path,
        "-I",
        ctx.bin_dir.path,
    ] + (
        ["--no-includes"] if ctx.attr.no_includes else []
    ) + (
        ["--gen-onefile"] if ctx.attr.language_flag == "--python" else []
    )

    # Iterate over all `.fbs` sources and run the flatbuffer compiler
    for src in ctx.files.srcs:
        ctx.actions.run(
            inputs = depset(ctx.files.srcs + ctx.files.deps),
            outputs = [output_dir],
            executable = ctx.executable._flatc,
            arguments = flatc_args + [
                src.path,
            ],
            progress_message = "Generating flatbuffer files for {}".format(src.path),
            use_default_shell_env = True,
        )

    # Return the directory as an output
    return [
        DefaultInfo(files = depset([output_dir])),
    ]

_gen_flatbuffer_srcs = rule(
    _gen_flatbuffer_srcs_impl,
    attrs = {
        "deps": attr.label_list(
            default = [],
            mandatory = False,
            aspects = [_flatbuffer_schemas_aspect],
        ),
        "include_paths": attr.string_list(
            default = [],
            mandatory = False,
        ),
        "language_flag": attr.string(
            mandatory = True,
        ),
        "no_includes": attr.bool(
            default = False,
            mandatory = False,
        ),
        "output": attr.string(
            default = "",
            mandatory = False,
        ),
        "srcs": attr.label_list(
            allow_files = [".fbs"],
            mandatory = True,
        ),
        "_flatc": attr.label(
            default = Label("@flatbuffers//:flatc"),
            executable = True,
            cfg = "exec",
        ),
    },
)

def flatbuffer_py_library(
        name,
        srcs,
        deps = [],
        include_paths = []):
    """A py_library with the generated reader/writers for the given schema.

    This rule assumes that the schema files define non-conflicting names, so that
    they can be merged in a single file. This is e.g. the case if only a single
    namespace is used.
    The rule call the flatbuffer compiler for all schema files and merges the
    generated python files into a single file that is wrapped in a py_library.

    Args:
      name: Rule name. (required)
      srcs: List of source .fbs files. (required)
      deps: List of dependencies.
      include_paths: Optional, list of paths the includes files can be found in.
    """
    all_srcs = "{}_srcs".format(name)
    _gen_flatbuffer_srcs(
        name = all_srcs,
        srcs = srcs,
        language_flag = "--python",
        deps = deps,
        output = name,
        no_includes = True,
        include_paths = include_paths,
    )
    py_library(
        name = name,
        srcs = [],
        data = [":{}".format(all_srcs)],
        imports = ["."],
        srcs_version = "PY3",
        deps = deps + [
            "@flatbuffers//:runtime_py",
        ],
    )
