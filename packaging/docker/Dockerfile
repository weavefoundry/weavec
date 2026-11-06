# RFC 0038, section 6: ghcr.io/weavefoundry/weavec:<version>, Ubuntu 24.04
# with the Linux toolchain on PATH and build-essential (the system linker, the
# C library's headers and start-up files). The release workflow builds it for
# linux/amd64 and linux/arm64 from the release archives, unpacked into
# dist/linux-amd64 and dist/linux-arm64.
FROM ubuntu:24.04
ARG TARGETARCH
RUN apt-get update \
    && apt-get install -y --no-install-recommends build-essential ca-certificates \
    && rm -rf /var/lib/apt/lists/*
COPY dist/linux-${TARGETARCH}/ /opt/weavec/
ENV PATH=/opt/weavec/bin:$PATH \
    CC=weavec-cc
WORKDIR /src
CMD ["weavec-cc", "--version"]
