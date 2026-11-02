# Build environment for the Kindle (kindlehf) target; used by scripts/build-kindle-docker.sh.
# koxtoolchain only ships x86_64 Linux binaries, hence the fixed platform.
FROM --platform=linux/amd64 debian:bookworm-slim

RUN apt-get update \
    && apt-get install -y --no-install-recommends ca-certificates cmake curl file git make zip zstd \
    && rm -rf /var/lib/apt/lists/*

ARG KOXTOOLCHAIN_RELEASE=2026.08
ARG KOXTOOLCHAIN_SHA256=8cc7dfbd71abd78f9e947d6b2e20670288a4402edc7b07176bca791f7eaf87d0
RUN curl -fsSL -o /tmp/kindlehf.tar.zst \
        "https://github.com/koreader/koxtoolchain/releases/download/${KOXTOOLCHAIN_RELEASE}/kindlehf.tar.zst" \
    && echo "${KOXTOOLCHAIN_SHA256}  /tmp/kindlehf.tar.zst" | sha256sum -c - \
    && tar --zstd -xf /tmp/kindlehf.tar.zst -C /opt \
    && rm /tmp/kindlehf.tar.zst

ENV XS_KINDLE_TC=/opt/x-tools/arm-kindlehf-linux-gnueabihf
# The source tree is bind-mounted and owned by the host user.
RUN git config --global --add safe.directory '*'
