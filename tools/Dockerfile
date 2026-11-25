# mbedTLS built with EC-JPAKE enabled, for the Thread 1.4 ePSKc commissioner flow.
# Distro mbedtls packages ship with MBEDTLS_KEY_EXCHANGE_ECJPAKE_ENABLED off,
# so we build from source with it switched on.
FROM debian:bookworm-slim

# python3-jsonschema / jinja2 are needed by mbedtls' PSA driver-wrapper codegen.
RUN apt-get update && apt-get install -y --no-install-recommends \
        build-essential cmake git python3 python3-jsonschema python3-jinja2 \
        ca-certificates iproute2 iputils-ping \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /src
# --recurse-submodules: 3.6.x needs the `framework` submodule or the build aborts.
RUN git clone --depth 1 --branch mbedtls-3.6.8 --recurse-submodules --shallow-submodules \
        https://github.com/Mbed-TLS/mbedtls.git \
 && test "$(git -C mbedtls rev-parse HEAD)" = 82e69999f91126589a28b70dd75b69fbcabe2711

WORKDIR /src/mbedtls
# EC-JPAKE: the key exchange itself, plus the raw primitive.
RUN python3 scripts/config.py set MBEDTLS_KEY_EXCHANGE_ECJPAKE_ENABLED \
 && python3 scripts/config.py set MBEDTLS_ECJPAKE_C \
 && python3 scripts/config.py set MBEDTLS_SSL_PROTO_DTLS \
 && python3 scripts/config.py set MBEDTLS_SSL_DTLS_ANTI_REPLAY \
 && python3 scripts/config.py set MBEDTLS_SSL_DTLS_HELLO_VERIFY \
 && python3 scripts/config.py set MBEDTLS_CCM_C \
 && python3 scripts/config.py set MBEDTLS_ECP_DP_SECP256R1_ENABLED

# No pipe here: piping to tail would mask a compile failure behind tail's exit code.
RUN make -j"$(nproc)" no_test

# Check the compiled configuration. All of these features are required by the
# commissioner's Thread DTLS ciphersuite.
RUN set -e; for feature in MBEDTLS_KEY_EXCHANGE_ECJPAKE_ENABLED MBEDTLS_ECJPAKE_C \
        MBEDTLS_SSL_PROTO_TLS1_2 MBEDTLS_SSL_PROTO_DTLS MBEDTLS_SSL_DTLS_ANTI_REPLAY \
        MBEDTLS_SSL_DTLS_HELLO_VERIFY MBEDTLS_CCM_C MBEDTLS_ECP_DP_SECP256R1_ENABLED; do \
        ./programs/test/query_compile_time_config "$feature"; \
    done

COPY epskc_commissioner.c /src/epskc_commissioner.c
RUN cc -O2 -Wall -Wextra -Werror=implicit-function-declaration -Iinclude \
        /src/epskc_commissioner.c library/libmbedtls.a library/libmbedx509.a \
        library/libmbedcrypto.a -o /usr/local/bin/epskc_commissioner

WORKDIR /work
