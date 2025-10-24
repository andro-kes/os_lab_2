FROM ubuntu:24.04

ENV DEBIAN_FRONTEND=noninteractive

RUN apt-get update && \
    apt-get install -y --no-install-recommends \
      cmake \
      build-essential \
      gcc \
      g++ \
      ninja-build \
      pkg-config \
      ca-certificates && \
    rm -rf /var/lib/apt/lists/*

WORKDIR /workspaces


