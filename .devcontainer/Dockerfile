# ------------------------------
# Base image (Micromamba)
# ------------------------------
FROM mambaorg/micromamba:latest

USER root
ENV DEBIAN_FRONTEND=noninteractive

# ------------------------------
# System dependencies
# ------------------------------
RUN apt-get update && apt-get install -y --no-install-recommends \
    sudo \
    git \
    curl \
    wget \
    build-essential \
    ca-certificates \
    tmux \
    unzip \
    vim \
    less \
    locales \
    && rm -rf /var/lib/apt/lists/*

# ------------------------------
# Locale setup
# ------------------------------
RUN echo "en_US.UTF-8 UTF-8" >> /etc/locale.gen && locale-gen en_US.UTF-8
ENV LANG=en_US.UTF-8
ENV LANGUAGE=en_US:en
ENV LC_ALL=en_US.UTF-8

# ------------------------------
# JVM + Scala UTF-8 protection
# ------------------------------
ENV SBT_OPTS="-Dfile.encoding=UTF-8 -Dsun.jnu.encoding=UTF-8 -Dfile.encoding.pkg=UTF-8"
ENV _JAVA_OPTIONS="-Dfile.encoding=UTF-8"
ENV JAVA_TOOL_OPTIONS="-Dfile.encoding=UTF-8"

# ------------------------------
# Micromamba environment setup
# ------------------------------
RUN micromamba install -y -n base -c conda-forge -c ucb-bar \
    'conda-lock<1.5' \
    'libstdcxx-ng>=12' \
    'firtool' && \
    micromamba clean --all --yes

# ------------------------------
# Create non-root user
# ------------------------------
ARG USERNAME=vscode
ARG USER_UID=1000
ARG USER_GID=$USER_UID

RUN groupadd --gid $USER_GID $USERNAME && \
    useradd -m -s /bin/bash --uid $USER_UID --gid $USER_GID $USERNAME && \
    echo "$USERNAME ALL=(ALL) NOPASSWD:ALL" > /etc/sudoers.d/$USERNAME && \
    chmod 0440 /etc/sudoers.d/$USERNAME && \
    mkdir -p /workspace && chown -R $USERNAME:$USERNAME /workspace

USER $USERNAME
WORKDIR /workspace

# ------------------------------
# Clone and initialize Chipyard
# ------------------------------
ARG CHIPYARD_REPO=https://github.com/AnshKetchum/chipyard.git
ARG CHIPYARD_BRANCH=main

RUN git clone --depth 1 --branch $CHIPYARD_BRANCH $CHIPYARD_REPO chipyard
WORKDIR /workspace/chipyard
RUN git config --global --add safe.directory '*'

# ------------------------------
# Initialize submodules and build minimal toolchain
# ------------------------------
RUN ./scripts/init-submodules-no-riscv-tools.sh && \
    ./scripts/build-toolchains.sh riscv-tools

# ------------------------------
# Default entrypoint
# ------------------------------
CMD ["bash"]
