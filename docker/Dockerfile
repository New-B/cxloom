FROM ubuntu:24.04

# Allow passing proxy settings at build time
ARG HTTP_PROXY
ARG HTTPS_PROXY
ENV HTTP_PROXY=${HTTP_PROXY:-}
ENV HTTPS_PROXY=${HTTPS_PROXY:-}
ENV http_proxy=${HTTP_PROXY:-}
ENV https_proxy=${HTTPS_PROXY:-}

ENV DEBIAN_FRONTEND=noninteractive

# Use a reachable Ubuntu mirror inside the build to avoid DNS/proxy issues
RUN sed -i 's|http://archive.ubuntu.com/ubuntu|http://mirrors.aliyun.com/ubuntu|g; s|http://security.ubuntu.com/ubuntu|http://mirrors.aliyun.com/ubuntu|g' /etc/apt/sources.list || true

RUN apt-get update && apt-get install -y --no-install-recommends \
    bash build-essential ca-certificates cmake ninja-build numactl procps util-linux \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /workspace
CMD ["sleep", "infinity"]
