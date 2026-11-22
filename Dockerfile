# The one image of this repository (`dc_determinism-cpu:local`): Ubuntu 24.04 with Python 3, numpy, g++, clang,
# cmake/ninja, git, Node.js and Emscripten. The Linux commands of dc_determinism, the Linux builds of `dc_determinism prebuild`
# and the pybind features (Python, amd64 only) all run in it; commands run directly (no ENTRYPOINT).
# Building it is the only step that downloads packages (apt, inside the Docker build).
#
#   dc_determinism.sh image build          (or .\dc_determinism.ps1 image build)
#
# The wrappers build it from this file on stdin, so no build context is sent to the Docker daemon (the
# .dockerignore next to this file excludes everything for the same reason). The same by hand:
#
#   docker build -t dc_determinism-cpu:local - < Dockerfile
FROM ubuntu:24.04
ENV DEBIAN_FRONTEND=noninteractive
RUN apt-get update && apt-get install -y --no-install-recommends \
    python3 python3-numpy ca-certificates git cmake make ninja-build g++ clang \
    nodejs emscripten && rm -rf /var/lib/apt/lists/*
WORKDIR /work
