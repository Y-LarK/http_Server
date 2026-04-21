FROM ubuntu:22.04

ENV DEBIAN_FRONTEND=noninteractive

RUN apt-get update && apt-get install -y \
    build-essential \
    cmake \
    g++ \
    gdb \
    clangd \
    socat \
    git \
    iputils-ping \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /workspace
COPY start_docker.sh /usr/local/bin/start_docker.sh
RUN chmod +x /usr/local/bin/start_docker.sh

EXPOSE 8080 9527

ENTRYPOINT ["/usr/local/bin/start_docker.sh"]

CMD ["sleep", "infinity"]
