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
RUN git clone --depth 1 --branch v3.6.2 --recurse-submodules --shallow-submodules \
        https://github.com/Mbed-TLS/mbedtls.git

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

# Sanity report only -- diagnosed interactively rather than failing the build.
RUN echo "--- ECJPAKE config ---" \
 && (./programs/test/query_compile_time_config MBEDTLS_KEY_EXCHANGE_ECJPAKE_ENABLED \
      && echo "KEY_EXCHANGE_ECJPAKE: on" || echo "KEY_EXCHANGE_ECJPAKE: OFF") \
 && (./programs/test/query_compile_time_config MBEDTLS_ECJPAKE_C \
      && echo "ECJPAKE_C: on" || echo "ECJPAKE_C: OFF") \
 && (./programs/test/query_compile_time_config MBEDTLS_USE_PSA_CRYPTO \
      && echo "USE_PSA_CRYPTO: on" || echo "USE_PSA_CRYPTO: off")

WORKDIR /work
