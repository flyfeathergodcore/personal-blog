# Local CI image. Build it from the repository root:
# docker compose -f webcpp-engine/docker-compose.ci.yml run --rm ci
FROM ubuntu:22.04

ENV DEBIAN_FRONTEND=noninteractive
RUN apt-get update && apt-get install -y --no-install-recommends \
        build-essential cmake ninja-build pkg-config ca-certificates \
        clang cppcheck curl python3 openssl \
        libssl-dev libyaml-cpp-dev libnghttp2-dev libmysqlclient-dev libmimalloc-dev \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /src
COPY mysql_connection_pool/ /src/mysql_connection_pool/
COPY webcpp-engine/ /src/webcpp-engine/
COPY CMakeLists.txt /src/CMakeLists.txt

COPY webcpp-engine/ci/run-checks.sh /usr/local/bin/webcpp-ci
RUN chmod +x /usr/local/bin/webcpp-ci

CMD ["/usr/local/bin/webcpp-ci"]
