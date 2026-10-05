# Bazel-in-container: repo bind-mounted at /workspace; entrypoint build+execs runfiles wrappers.
FROM ubuntu:24.04

RUN apt-get update && apt-get install -y --no-install-recommends \
        ca-certificates \
        curl \
        git \
        iproute2 \
        libstdc++6 \
        python3 \
        python3-jinja2 \
        util-linux \
        unzip \
        zip \
    && rm -rf /var/lib/apt/lists/*

ARG BAZELISK_VERSION=1.27.0
RUN curl -fsSL "https://github.com/bazelbuild/bazelisk/releases/download/v${BAZELISK_VERSION}/bazelisk-linux-amd64" \
        -o /usr/local/bin/bazel \
    && chmod +x /usr/local/bin/bazel

# Compose runs as host UID/GID; cache mounted at $HOME/.cache (see docker-compose.yml).
WORKDIR /workspace

COPY multicast_setup.sh bazel_entrypoint.sh /usr/local/bin/
RUN chmod +x /usr/local/bin/multicast_setup.sh /usr/local/bin/bazel_entrypoint.sh

ENTRYPOINT ["/usr/local/bin/bazel_entrypoint.sh"]
