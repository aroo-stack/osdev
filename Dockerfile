FROM --platform=linux/amd64 debian:bookworm-slim

RUN apt-get update && apt-get install -y \
    build-essential gcc-multilib nasm xorriso mtools \
    grub-pc-bin grub-common qemu-system-x86 \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /root/osdev
