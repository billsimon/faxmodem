# faxmodem - T.30 fax over SIP, all logs on stdout.
#
#   docker build -t faxmodem .
#   docker run --rm --network host \
#       -e FAXMODEM_SERVER=sip.example.com -e FAXMODEM_USERNAME=1001 \
#       -e FAXMODEM_PASSWORD=secret \
#       -v "$PWD:/data" faxmodem send +15551234567 /data/invoice.tif
#
# SIP and RTP need reachable ports, so --network host is usually the least
# painful option. Otherwise publish the SIP port and an RTP range and set
# --public-addr to the address the far end should send media to.

ARG PJPROJECT_VERSION=2.17

FROM debian:bookworm-slim AS builder
ARG PJPROJECT_VERSION

RUN apt-get update && apt-get install -y --no-install-recommends \
        build-essential \
        ca-certificates \
        cmake \
        curl \
        libspandsp-dev \
        libssl-dev \
        libtiff-dev \
        pkg-config \
    && rm -rf /var/lib/apt/lists/*

# pjproject is not packaged in Debian; build the static libraries we need.
WORKDIR /build
RUN curl -fsSL "https://github.com/pjsip/pjproject/archive/refs/tags/${PJPROJECT_VERSION}.tar.gz" \
        -o pjproject.tar.gz \
    && tar xf pjproject.tar.gz \
    && mv "pjproject-${PJPROJECT_VERSION}" pjproject

RUN printf '%s\n' \
        '#define PJ_HAS_IPV6 1' \
        '#define PJMEDIA_HAS_VIDEO 0' \
        > pjproject/pjlib/include/pj/config_site.h

WORKDIR /build/pjproject
RUN ./configure \
        --prefix=/usr/local \
        --disable-video \
        --disable-sound \
        --disable-opencore-amr \
        --disable-silk \
        --disable-bcg729 \
        CFLAGS="-O2 -fPIC" \
    && make dep && make -j"$(nproc)" && make install

COPY CMakeLists.txt /src/CMakeLists.txt
COPY include /src/include
COPY src /src/src
WORKDIR /src
RUN cmake -S . -B build -DCMAKE_BUILD_TYPE=RelWithDebInfo \
    && cmake --build build -j"$(nproc)" \
    && cmake --install build --prefix /usr/local

FROM debian:bookworm-slim

RUN apt-get update && apt-get install -y --no-install-recommends \
        ca-certificates \
        ghostscript \
        libspandsp2 \
        libssl3 \
        libtiff6 \
    && rm -rf /var/lib/apt/lists/* \
    && useradd --system --create-home --home-dir /var/lib/faxmodem faxmodem \
    && mkdir -p /var/spool/faxmodem /var/lib/faxmodem/received \
    && chown -R faxmodem:faxmodem /var/spool/faxmodem /var/lib/faxmodem

COPY --from=builder /usr/local/bin/faxmodem /usr/local/bin/faxmodem
COPY scripts/make-test-page.sh /usr/local/bin/faxmodem-make-test-page

ENV FAXMODEM_SPOOL_DIR=/var/spool/faxmodem \
    FAXMODEM_OUTPUT_DIR=/var/lib/faxmodem/received

USER faxmodem
WORKDIR /var/lib/faxmodem

# SIP signalling; RTP is negotiated dynamically (see --rtp-port).
EXPOSE 5060/udp

ENTRYPOINT ["faxmodem"]
CMD ["help"]
