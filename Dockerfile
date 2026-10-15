# anoa in a container: a headless browser with a CDP endpoint and the agent CLI.
#
# Built from the published release tarball rather than from source. The tarball
# is what users actually get, so an image built from it exercises the same
# artifact — and the alternative is compiling Qt WebEngine, which turns a
# one-minute build into an hour.
#
#   docker build -t anoa .
#   docker run --rm -p 9222:9222 anoa
#   docker run --rm anoa open example.com     # one-shot, needs a browser first
#
# The bundle carries its own Qt. The packages below are the system libraries
# Chromium dlopen()s regardless — they resolve outside the bundle's RPATH, so
# they cannot be vendored with it.

FROM debian:12-slim

# Pinned at build time so an image is reproducible. `latest` is resolved by the
# workflow, not here — a Dockerfile that silently changes what it installs is a
# Dockerfile you cannot roll back.
ARG ANOA_VERSION=latest
ARG TARGETARCH=amd64

# hadolint ignore=DL3008
RUN set -eux; \
    apt-get update; \
    DEBIAN_FRONTEND=noninteractive apt-get install -y --no-install-recommends \
        ca-certificates \
        curl \
        # Chromium's own dependencies. Missing any of these fails at *runtime*
        # with a dlopen error rather than at build, so they are listed
        # explicitly instead of trusting a meta-package.
        libnss3 \
        libnspr4 \
        libxcomposite1 \
        libxdamage1 \
        libxrandr2 \
        libxkbcommon0 \
        libxkbfile1 \
        libdrm2 \
        libasound2 \
        libcups2 \
        libatk1.0-0 \
        libatk-bridge2.0-0 \
        libatspi2.0-0 \
        libxshmfence1 \
        libglib2.0-0 \
        # The GL stack comes from the host, never from the bundle: libGL and
        # friends are dispatch layers that load *this* machine's driver, and a
        # vendored copy looks for one that is not here.
        libgl1 \
        libglx-mesa0 \
        libegl1 \
        libgbm1 \
        libfontconfig1 \
        libfreetype6 \
        libdbus-1-3 \
        # Without a font nothing renders as text — screenshots come back as
        # boxes, which looks like a broken browser rather than a missing font.
        fonts-liberation \
        fonts-noto-color-emoji \
    ; \
    rm -rf /var/lib/apt/lists/*

# Only x86_64 is published today; fail loudly on anything else rather than
# producing an image that dies on first run.
RUN set -eux; \
    case "${TARGETARCH}" in \
        amd64) asset="anoa-linux-x86_64.tar.gz" ;; \
        *) echo "no published bundle for TARGETARCH=${TARGETARCH}" >&2; exit 1 ;; \
    esac; \
    if [ "${ANOA_VERSION}" = "latest" ]; then \
        # No -m1: it closes the pipe on the first match and curl exits 23
        # writing the remainder. Harmless under sh, fatal under pipefail —
        # which is exactly how this broke in container.yml.
        version="$(curl -fsSL https://api.github.com/repos/porcupine-md/anoa-browser/releases/latest \
                   | grep '"tag_name"' | sed -E 's/.*"([^"]+)".*/\1/')"; \
    else \
        version="${ANOA_VERSION}"; \
    fi; \
    case "${version}" in v*) ;; *) version="v${version}" ;; esac; \
    echo "installing anoa ${version}"; \
    curl -fsSL -o /tmp/anoa.tar.gz \
        "https://github.com/porcupine-md/anoa-browser/releases/download/${version}/${asset}"; \
    # A 404 arrives as an HTML page and tar would blame a truncated archive.
    head -c2 /tmp/anoa.tar.gz | od -An -tx1 | tr -d ' \n' | grep -q '^1f8b'; \
    mkdir -p /opt; \
    tar xzf /tmp/anoa.tar.gz -C /opt; \
    rm /tmp/anoa.tar.gz; \
    # The launcher, never the raw binary: it is what sets LD_LIBRARY_PATH and
    # the QtWebEngine paths that make the bundle self-contained.
    ln -sf /opt/anoa/anoa.sh /usr/local/bin/anoa; \
    anoa --version

# Chromium's sandbox needs privileges a default container does not have, and
# asking users to run --privileged is worse than turning it off inside an
# already-isolated container.
# Qt prints a four-line complaint on *every* invocation when the locale is not
# UTF-8, which lands in the middle of anything a caller captures.
ENV LANG=C.UTF-8 \
    LC_ALL=C.UTF-8

ENV QTWEBENGINE_CHROMIUM_FLAGS="--disable-gpu --no-sandbox"

# Not root. Chromium refuses to start as root without --no-sandbox anyway, and
# an image whose browser runs as root is a browser that renders untrusted pages
# as root.
RUN useradd --create-home --uid 1000 anoa
USER anoa
WORKDIR /home/anoa

EXPOSE 9222

# Listening on all interfaces so `-p 9222:9222` reaches it. The endpoint is
# unauthenticated by default; pass --auth-token to require a bearer token, and
# do not publish the port to an untrusted network without one.
ENTRYPOINT ["anoa"]
CMD ["--headless", "--no-sandbox", "--port", "9222"]
