FROM alpine:latest

# Install required dependencies
RUN apk add --no-cache \
    libxml2-dev \
    curl-dev \
    shadow \
    git \
    make \
    gcc \
    musl-dev \
    pkgconf \
    openssl-dev \
    openrc

# Clone and install beaker
RUN git clone https://git.bwaaa.monster/beaker /tmp/beaker \
    && cd /tmp/beaker \
    && make \
    && make install \
    && rm -rf /tmp/beaker

# Import omnisearch source
WORKDIR /app
COPY . /app

# Clone and install omnisearch
RUN cd /app \
    && make \
    && make install-openrc

# Enable OpenRC and start the service
RUN rc-update add omnisearch default

# Expose the default port
EXPOSE 5000

# Start OpenRC and the service
CMD sh -c "openrc default && touch /run/openrc/softlevel && omnisearch"
