#!/usr/bin/env bash
set -euo pipefail

TARGET="${PRP_SSH_TARGET:-root@172.16.42.1}"
PORT="${PRP_SSH_PORT:-22}"
KNOWN_HOSTS="${PRP_SSH_KNOWN_HOSTS:-/tmp/prp_known_hosts}"

usage() {
  cat >&2 <<EOF
usage: $0 <src> <dst> [extra-scp-args...]

Examples:
  $0 ./file.bin ${TARGET}:/tmp/file.bin
  $0 ./file.bin /tmp/file.bin
  $0 ${TARGET}:/tmp/file.bin ./file.bin

Notes:
  - This wrapper forces legacy SCP protocol (-O), required for Dropbear targets.
  - If neither side includes host:, destination is treated as a path on ${TARGET}.
EOF
  exit 1
}

(( $# >= 2 )) || usage

src="$1"
dst="$2"
shift 2

is_remote() {
  [[ "$1" == *:* ]]
}

remote_src=0
remote_dst=0
is_remote "$src" && remote_src=1
is_remote "$dst" && remote_dst=1

if (( remote_src == 1 && remote_dst == 1 )); then
  echo "error: both src and dst look remote; one side must be local" >&2
  exit 1
fi

if (( remote_src == 0 && remote_dst == 0 )); then
  dst="${TARGET}:${dst}"
elif (( remote_src == 0 && remote_dst == 1 )); then
  case "$dst" in
    :*) dst="${TARGET}${dst}" ;;
  esac
elif (( remote_src == 1 && remote_dst == 0 )); then
  case "$src" in
    :*) src="${TARGET}${src}" ;;
  esac
fi

exec scp \
  -O \
  -P "$PORT" \
  -o StrictHostKeyChecking=accept-new \
  -o UserKnownHostsFile="$KNOWN_HOSTS" \
  "$@" \
  "$src" "$dst"
