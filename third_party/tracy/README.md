# Tracy (mw-benchmark)

**Client:** depend on `@tracy//:tracy`; enable with `--config=tracy` (no-ops when off). See `DEVOPS.ccu-stack/third_party/tracy` for the same Bazel pattern.

**Server:** build and run **`tracy-capture`** (or the Tracy GUI) yourself on the host that can reach the instrumented processes — not wired into this repo. Typical headless flow:

```bash
tracy-capture -o /tmp/vsomeip.tracy -a 0.0.0.0
```

Build instructions: [Tracy capture](https://github.com/wolfpld/tracy/tree/master/capture) or the Docker recipe in `DEVOPS.ccu-stack/third_party/tracy/README.md`.

**vsomeip:** `VSOMEIP_TRACY=1 vsomeip/docker/run_profile.sh` (after capture is listening on port **8086**). Set `TRACY_CLIENT_ADDRESS` to an IP containers can reach.
