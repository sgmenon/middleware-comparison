# Runtime image for mw-benchmark network CLIs (host-built --config=opt binaries).
FROM ubuntu:24.04

RUN apt-get update && apt-get install -y --no-install-recommends \
        ca-certificates \
        libacl1 \
        libatomic1 \
        libstdc++6 \
    && rm -rf /var/lib/apt/lists/*

# Staged as bazel-bin layout so RPATH ($ORIGIN/../../_solib_local/...) resolves.
COPY staging/ /opt/mw/

ENV PATH="/opt/mw/benchmarks/net:${PATH}"
# Bazel RPATH expects $ORIGIN/../../_solib_local; our layout keeps solibs under /opt/mw.
ENV LD_LIBRARY_PATH="/opt/mw/_solib_local/_U_A_Azenoh-c+_S_S_Czenoh-c___Uexternal_Szenoh-c+:/opt/mw/_solib_local/_U_A_A+third_Uparty2+cyclonedds_S_S_Ccyclonedds___Uexternal_S+third_Uparty2+cyclonedds_Scyclonedds_Slib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
WORKDIR /opt/mw

COPY entrypoint.sh /entrypoint.sh
RUN chmod +x /entrypoint.sh

ENTRYPOINT ["/entrypoint.sh"]
