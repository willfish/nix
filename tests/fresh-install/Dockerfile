FROM debian:bookworm-slim

# Deliberately install Nix from scratch, not from the host store or a Nix image.
RUN apt-get update \
    && apt-get install -y --no-install-recommends ca-certificates curl git xz-utils \
    && rm -rf /var/lib/apt/lists/* \
    && useradd --create-home --shell /bin/bash laptop \
    && mkdir -m 0755 /nix \
    && chown laptop /nix

USER laptop
ENV HOME=/home/laptop USER=laptop
WORKDIR /home/laptop
RUN curl --fail --silent --show-error --location https://nixos.org/nix/install -o /tmp/install-nix \
    && sh /tmp/install-nix --no-daemon \
    && rm /tmp/install-nix

# No host mounts, SSH agent, Nix store, access tokens or private configuration.
COPY --chown=laptop:laptop . /home/laptop/dotfiles
WORKDIR /home/laptop/dotfiles
ENTRYPOINT ["/bin/bash", "-lc"]
