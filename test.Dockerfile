ARG UBUNTU_VERSION=22.04
FROM ubuntu:${UBUNTU_VERSION}

ENV DEBIAN_FRONTEND=noninteractive

RUN apt-get update && apt-get install -y --no-install-recommends \
        ca-certificates curl gnupg gnupg2 wget lsb-release sudo \
        python3 python3-pip python3-venv \
    && rm -rf /var/lib/apt/lists/*
