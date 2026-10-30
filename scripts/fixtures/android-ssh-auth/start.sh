#!/bin/sh
set -eu
mkdir -p /run/sshd /run/ghosterm
ssh-keygen -q -t ed25519 -N '' -f /run/ghosterm/hostkey
exec /usr/sbin/sshd -D -e -f /fixture/sshd_config "$@"
