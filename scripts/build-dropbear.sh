#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
# shellcheck disable=SC1091
source "$SCRIPT_DIR/common.sh"

[[ $# -eq 2 ]] || die "usage: $0 <config.env> <out_dir>"
CFG="$1"
OUT_DIR="$2"

require_cmd curl
require_cmd tar
require_cmd make
require_cmd zig

load_config "$CFG"
mkdir -p "$OUT_DIR"
OUT_DIR="$(cd "$OUT_DIR" && pwd)"

DROPBEAR_VER="${DROPBEAR_VER:-2024.86}"
DROPBEAR_STRIP="${DROPBEAR_STRIP:-0}"
DROPBEAR_PATCH_MODE="${DROPBEAR_PATCH_MODE:-instrumented}"
DROPBEAR_CC_MODE="${DROPBEAR_CC_MODE:-zig}"
DROPBEAR_OPT_LEVEL="${DROPBEAR_OPT_LEVEL:-2}"
TARBALL_URL="https://matt.ucc.asn.au/dropbear/releases/dropbear-${DROPBEAR_VER}.tar.bz2"

TOOLS_DIR="$OUT_DIR/tools"
SRC_ROOT="$TOOLS_DIR/dropbear-src"
BUILD_ROOT="$TOOLS_DIR/dropbear-build"
OUT_ROOT="$TOOLS_DIR/dropbear-out"

TARBALL="$SRC_ROOT/dropbear-${DROPBEAR_VER}.tar.bz2"
SRC_DIR="$SRC_ROOT/dropbear-${DROPBEAR_VER}"
BUILD_DIR="$BUILD_ROOT/dropbear-${DROPBEAR_VER}-${TARGET_ARCH}"
BIN_DIR="$OUT_ROOT/${TARGET_ARCH}"

mkdir -p "$SRC_ROOT" "$BUILD_ROOT" "$OUT_ROOT" "$BIN_DIR"

if [[ ! -f "$TARBALL" ]]; then
  echo "dropbear: downloading $TARBALL_URL"
  curl -L --fail -o "$TARBALL" "$TARBALL_URL"
fi

if [[ ! -d "$SRC_DIR" ]]; then
  echo "dropbear: extracting source"
  (cd "$SRC_ROOT" && tar -xjf "$TARBALL")
fi

# Dropbear builds in-tree. Always do a clean rebuild to avoid stale/cross-target outputs.
rm -rf "$BUILD_DIR"
cp -a "$SRC_DIR" "$BUILD_DIR"
rm -f "$BIN_DIR/dropbear" "$BIN_DIR/dropbearkey" "$BIN_DIR/dbclient" "$BIN_DIR/scp"

# Always set TCP_NODELAY on accepted session sockets.
# Vanilla Dropbear only sets it on listener/client sockets via netio.c.
python - "$BUILD_DIR" <<'PY2'
from pathlib import Path
import sys

build = Path(sys.argv[1])
path = build / 'src/svr-main.c'
text = path.read_text()
old = "\t\t\tif (childsock < 0) {\n\t\t\t\t/* accept failed */\n\t\t\t\tcontinue;\n\t\t\t}\n\n\t\t\t/* Limit the number of unauthenticated connections per IP */\n"
new = "\t\t\tif (childsock < 0) {\n\t\t\t\t/* accept failed */\n\t\t\t\tcontinue;\n\t\t\t}\n\t\t\tset_sock_nodelay(childsock);\n\n\t\t\t/* Limit the number of unauthenticated connections per IP */\n"
if old in text:
    path.write_text(text.replace(old, new, 1))
elif "set_sock_nodelay(childsock);" not in text:
    raise SystemExit("patch anchor not found in src/svr-main.c")
PY2

if [[ "$DROPBEAR_PATCH_MODE" != "vanilla" ]]; then
python - "$BUILD_DIR" <<'PY2'
from pathlib import Path
import sys
build = Path(sys.argv[1])

def patch(path_str, old, new):
    path = build / path_str
    text = path.read_text()
    if old not in text:
        raise SystemExit(f"patch anchor not found in {path_str}")
    path.write_text(text.replace(old, new, 1))

patch('src/dbutil.c',
"#include \"atomicio.h\"\n\n#define MAX_FMT 100\n",
"#include \"atomicio.h\"\n\n#define MAX_FMT 100\n\nstatic void prp_ssh_trace(const char *msg) {\n\tint fd;\n\tconst char *paths[] = {\"/dev/ttyS0\", \"/dev/console\"};\n\tunsigned int i;\n\tfor (i = 0; i < sizeof(paths)/sizeof(paths[0]); i++) {\n\t\tfd = open(paths[i], O_WRONLY | O_NOCTTY);\n\t\tif (fd >= 0) {\n\t\t\twrite(fd, msg, strlen(msg));\n\t\t\twrite(fd, \"\\n\", 1);\n\t\t\tclose(fd);\n\t\t}\n\t}\n}\n")

patch('src/dbutil.c',
"\t/* Re-enable SIGPIPE for the executed process */\n",
"\tif (cmd != NULL) {\n\t\tprp_ssh_trace(\"[prp-ssh] run_shell_command command mode\");\n\t\tif (cmd[0] == '/' && strpbrk(cmd, \" \\t|&;<>$`(){}[]*?!\\\"'\") == NULL) {\n\t\t\tchar * direct_argv[2];\n\t\t\tdirect_argv[0] = (char*)cmd;\n\t\t\tdirect_argv[1] = NULL;\n\t\t\tprp_ssh_trace(\"[prp-ssh] run_shell_command direct-exec\");\n\t\t\texecv(cmd, direct_argv);\n\t\t\tprp_ssh_trace(\"[prp-ssh] run_shell_command direct-exec failed\");\n\t\t}\n\t} else {\n\t\tprp_ssh_trace(\"[prp-ssh] run_shell_command login-shell mode\");\n\t}\n\tprp_ssh_trace(\"[prp-ssh] run_shell_command shell-exec\");\n\n\t/* Re-enable SIGPIPE for the executed process */\n")

patch('src/dbutil.c',
"\texecv(usershell, argv);\n}\n",
"\texecv(usershell, argv);\n\tprp_ssh_trace(\"[prp-ssh] run_shell_command shell-exec failed\");\n}\n")

patch('src/svr-chansession.c',
"#include \"auth.h\"\n\n/* Handles sessions (either shells or programs) requested by the client */\n",
"#include \"auth.h\"\n\nstatic void prp_ssh_trace(const char *msg) {\n\tint fd;\n\tconst char *paths[] = {\"/dev/ttyS0\", \"/dev/console\"};\n\tunsigned int i;\n\tfor (i = 0; i < sizeof(paths)/sizeof(paths[0]); i++) {\n\t\tfd = open(paths[i], O_WRONLY | O_NOCTTY);\n\t\tif (fd >= 0) {\n\t\t\twrite(fd, msg, strlen(msg));\n\t\t\twrite(fd, \"\\n\", 1);\n\t\t\tclose(fd);\n\t\t}\n\t}\n}\n\n/* Handles sessions (either shells or programs) requested by the client */\n")

patch('src/svr-chansession.c',
"static void execchild(const void *user_data) {\n\tconst struct ChanSess *chansess = user_data;\n",
"static void execchild(const void *user_data) {\n\tconst struct ChanSess *chansess = user_data;\n\tprp_ssh_trace(\"[prp-ssh] execchild enter\");\n")

patch('src/svr-chansession.c',
"\tTRACE((\"type is %s\", type))\n",
"\tTRACE((\"type is %s\", type))\n\t{\n\t\tchar prp_buf[160];\n\t\tsnprintf(prp_buf, sizeof(prp_buf), \"[prp-ssh] chansessionrequest type=%s wantreply=%u\", type, (unsigned int)wantreply);\n\t\tprp_ssh_trace(prp_buf);\n\t}\n")

patch('src/svr-chansession.c',
"\tif (wantreply) {\n\t\tif (ret == DROPBEAR_SUCCESS) {\n\t\t\tsend_msg_channel_success(channel);\n\t\t} else {\n\t\t\tsend_msg_channel_failure(channel);\n\t\t}\n\t}\n",
"\tif (wantreply) {\n\t\tif (ret == DROPBEAR_SUCCESS) {\n\t\t\tprp_ssh_trace(\"[prp-ssh] chansessionrequest sending success\");\n\t\t\tsend_msg_channel_success(channel);\n\t\t} else {\n\t\t\tprp_ssh_trace(\"[prp-ssh] chansessionrequest sending failure\");\n\t\t\tsend_msg_channel_failure(channel);\n\t\t}\n\t}\n")

patch('src/svr-chansession.c',
"\t/* change directory */\n",
"\tprp_ssh_trace(\"[prp-ssh] execchild env ready\");\n\t/* change directory */\n")

patch('src/svr-chansession.c',
"\tusershell = m_strdup(get_user_shell());\n\trun_shell_command(chansess->cmd, ses.maxfd, usershell);\n",
"\tusershell = m_strdup(get_user_shell());\n\tprp_ssh_trace(\"[prp-ssh] execchild before run_shell_command\");\n\trun_shell_command(chansess->cmd, ses.maxfd, usershell);\n")

patch('src/svr-chansession.c',
"\tret = spawn_command(execchild, chansess, \n",
"\tprp_ssh_trace(\"[prp-ssh] noptycommand before spawn\");\n\tret = spawn_command(execchild, chansess, \n")

patch('src/svr-chansession.c',
"\t\t/* redirect stdin/stdout/stderr */\n",
"\t\t/* redirect stdin/stdout/stderr */\n\t\tprp_ssh_trace(\"[prp-ssh] pty child before dup2\");\n")

patch('src/svr-chansession.c',
"\t\t/* write the utmp/wtmp login record - must be after changing the\n",
"\t\tprp_ssh_trace(\"[prp-ssh] pty child stdout after dup2\");\n\t\t/* write the utmp/wtmp login record - must be after changing the\n")

patch('src/svr-chansession.c',
"\t\tclose(chansess->slave);\n",
"\t\tprp_ssh_trace(\"[prp-ssh] pty child stderr after dup2\");\n\t\tclose(chansess->slave);\n")

patch('src/svr-chansession.c',
"\tses.maxfd = MAX(ses.maxfd, channel->errfd);\n",
"\tses.maxfd = MAX(ses.maxfd, channel->errfd);\n\tprp_ssh_trace(\"[prp-ssh] nopty parent fds assigned\");\n")

patch('src/svr-chansession.c',
"\t\tsetnonblocking(chansess->master);\n",
"\t\tsetnonblocking(chansess->master);\n\t\tprp_ssh_trace(\"[prp-ssh] pty parent fds assigned\");\n")

patch('src/common-channel.c',
"#include \"listener.h\"\n#include \"runopts.h\"\n#include \"netio.h\"\n\nstatic void send_msg_channel_open_failure(unsigned int remotechan, int reason,\n\t\tconst char *text, const char *lang);\n",
"#include \"listener.h\"\n#include \"runopts.h\"\n#include \"netio.h\"\n\nstatic void prp_ssh_trace(const char *msg) {\n\tint fd;\n\tconst char *paths[] = {\"/dev/ttyS0\", \"/dev/console\"};\n\tunsigned int i;\n\tfor (i = 0; i < sizeof(paths)/sizeof(paths[0]); i++) {\n\t\tfd = open(paths[i], O_WRONLY | O_NOCTTY);\n\t\tif (fd >= 0) {\n\t\t\twrite(fd, msg, strlen(msg));\n\t\t\twrite(fd, \"\\n\", 1);\n\t\t\tclose(fd);\n\t\t}\n\t}\n}\n\nstatic void prp_ssh_trace_fd(const char *prefix, int fd, int val) {\n\tchar buf[160];\n\tsnprintf(buf, sizeof(buf), \"[prp-ssh] %s fd=%d val=%d\", prefix, fd, val);\n\tprp_ssh_trace(buf);\n}\n\nstatic void prp_ssh_trace_chan(const char *prefix, const struct Channel *channel, int extra) {\n\tchar buf[224];\n\tsnprintf(buf, sizeof(buf), \"[prp-ssh] %s idx=%u remote=%u trans=%u recv=%u extra=%d\", prefix,\n\t\tchannel->index, channel->remotechan, channel->transwindow, channel->recvwindow, extra);\n\tprp_ssh_trace(buf);\n}\n\nstatic void send_msg_channel_open_failure(unsigned int remotechan, int reason,\n\t\tconst char *text, const char *lang);\n")

patch('src/common-channel.c',
"\t\tif (channel->readfd >= 0 && FD_ISSET(channel->readfd, readfds)) {\n\t\t\tTRACE((\"send normal readfd\"))\n",
"\t\tif (channel->readfd >= 0 && FD_ISSET(channel->readfd, readfds)) {\n\t\t\tprp_ssh_trace_fd(\"channelio read-ready\", channel->readfd, channel->index);\n\t\t\tTRACE((\"send normal readfd\"))\n")

patch('src/common-channel.c',
"\tlen = read(fd, buf_getwriteptr(ses.writepayload, maxlen), maxlen);\n",
"\tlen = read(fd, buf_getwriteptr(ses.writepayload, maxlen), maxlen);\n\tprp_ssh_trace_fd(\"send_msg_channel_data read\", fd, len);\n")

patch('src/common-channel.c',
"\tchannel->transwindow -= len;\n",
"\tchannel->transwindow -= len;\n\tprp_ssh_trace_fd(\"send_msg_channel_data queued\", fd, len);\n")

patch('src/common-channel.c',
"void send_msg_channel_failure(const struct Channel *channel) {\n",
"void send_msg_channel_failure(const struct Channel *channel) {\n\tprp_ssh_trace(\"[prp-ssh] send_msg_channel_failure enter\");\n\tprp_ssh_trace_chan(\"send_msg_channel_failure chan\", channel, 0);\n")

patch('src/common-channel.c',
"\tencrypt_packet();\n\tTRACE((\"leave send_msg_channel_failure\"))\n",
"\tencrypt_packet();\n\tprp_ssh_trace(\"[prp-ssh] send_msg_channel_failure sent\");\n\tTRACE((\"leave send_msg_channel_failure\"))\n")

patch('src/common-channel.c',
"void send_msg_channel_success(const struct Channel *channel) {\n",
"void send_msg_channel_success(const struct Channel *channel) {\n\tprp_ssh_trace(\"[prp-ssh] send_msg_channel_success enter\");\n\tprp_ssh_trace_chan(\"send_msg_channel_success chan\", channel, 0);\n")

patch('src/common-channel.c',
"\tTRACE((\"enter send_msg_channel_data\"))\n\tdropbear_assert(!channel->sent_close);\n",
"\tTRACE((\"enter send_msg_channel_data\"))\n\tprp_ssh_trace_chan(\"send_msg_channel_data chan\", channel, isextended);\n\tdropbear_assert(!channel->sent_close);\n")

patch('src/common-channel.c',
"\tencrypt_packet();\n\tTRACE((\"leave send_msg_channel_success\"))\n",
"\tencrypt_packet();\n\tprp_ssh_trace(\"[prp-ssh] send_msg_channel_success sent\");\n\tTRACE((\"leave send_msg_channel_success\"))\n")

# packet.c: write_packet + encrypt_packet instrumentation.
patch('src/packet.c',
"#include \"includes.h\"\n#include \"packet.h\"\n#include \"session.h\"\n#include \"dbutil.h\"\n#include \"ssh.h\"\n#include \"algo.h\"\n",
"#include \"includes.h\"\n#include \"packet.h\"\n#include \"session.h\"\n#include \"dbutil.h\"\n#include \"ssh.h\"\n#include \"algo.h\"\n\nstatic void prp_pkt_trace(const char *msg) {\n\tint fd;\n\tconst char *paths[] = {\"/dev/ttyS0\", \"/dev/console\"};\n\tunsigned int i;\n\tfor (i = 0; i < sizeof(paths)/sizeof(paths[0]); i++) {\n\t\tfd = open(paths[i], O_WRONLY | O_NOCTTY);\n\t\tif (fd >= 0) {\n\t\t\twrite(fd, msg, strlen(msg));\n\t\t\twrite(fd, \"\\n\", 1);\n\t\t\tclose(fd);\n\t\t}\n\t}\n}\nstatic void prp_pkt_tracei(const char *prefix, long long val) {\n\tchar buf[160];\n\tsnprintf(buf, sizeof(buf), \"[prp-ssh] pkt: %s=%lld\", prefix, val);\n\tprp_pkt_trace(buf);\n}\n")

patch('src/packet.c',
"\tif ((!ses.dataallowed && !packet_is_okay_kex(packet_type))) {\n\t\t/* During key exchange only particular packets are allowed.\n\t\t\tSince this packet_type isn't OK we just enqueue it to send \n\t\t\tafter the KEX, see maybe_flush_reply_queue */\n\t\tenqueue_reply_packet();\n\t\treturn;\n\t}\n",
"\tprp_pkt_tracei(\"encrypt_packet type\", (long long)packet_type);\n\tprp_pkt_tracei(\"encrypt_packet dataallowed\", (long long)ses.dataallowed);\n\tif ((!ses.dataallowed && !packet_is_okay_kex(packet_type))) {\n\t\tprp_pkt_tracei(\"encrypt_packet deferred-reply type\", (long long)packet_type);\n\t\tenqueue_reply_packet();\n\t\treturn;\n\t}\n\tprp_pkt_trace(\"[prp-ssh] pkt: encrypt_packet going to writequeue\");\n")

PY2
fi

cc_desc="${ZIG_TARGET}"
(
  cd "$BUILD_DIR"

  case "$DROPBEAR_CC_MODE" in
    zig)
      export CC="zig cc -target ${ZIG_TARGET}"
      export AR="zig ar"
      export RANLIB="zig ranlib"
      cc_desc="${ZIG_TARGET}"
      ;;
    muslcc)
      source /etc/profile.d/arm-linux-musleabihf-cross.sh
      require_cmd arm-linux-musleabihf-gcc
      export CC="arm-linux-musleabihf-gcc"
      export AR="arm-linux-musleabihf-ar"
      export RANLIB="arm-linux-musleabihf-ranlib"
      cc_desc="arm-linux-musleabihf-gcc"
      ;;
    *)
      die "unsupported DROPBEAR_CC_MODE: $DROPBEAR_CC_MODE"
      ;;
  esac
  # Conservative flags to avoid optimizer/strip-induced instability on target.
  export CFLAGS="-O${DROPBEAR_OPT_LEVEL} -fno-omit-frame-pointer"
  export LDFLAGS="-static"
  if [[ "$DROPBEAR_CC_MODE" == "muslcc" ]]; then
    # The muslcc wrapper's built-in sysroot lacks libc.a on this host.
    # The Arch `armhf-musl` package provides the static musl sysroot we need.
    export CPPFLAGS="-I/usr/lib/armhf-musl/include"
    export LDFLAGS="-static -L/usr/lib/armhf-musl/lib"
  fi

  echo "dropbear: building static ${TARGET_ARCH} (${cc_desc}) in $BUILD_DIR (patch_mode=${DROPBEAR_PATCH_MODE}, cc_mode=${DROPBEAR_CC_MODE}, opt=${DROPBEAR_OPT_LEVEL})"

  ./configure \
    --host="${DROPBEAR_HOST}" \
    --enable-static \
    --disable-harden \
    --disable-zlib \
    --disable-syslog \
    --disable-shadow \
    --disable-lastlog \
    --disable-utmp \
    --disable-utmpx \
    --disable-wtmp \
    --disable-wtmpx

  # Include the standalone scp (from OpenSSH) so host-side scp works.
  make -j"$(nproc)" PROGRAMS="dropbear dropbearkey dbclient scp"

  cp -a dropbear dropbearkey dbclient scp "$BIN_DIR/"
)

if [[ "$DROPBEAR_STRIP" = "1" ]]; then
  if command -v llvm-strip >/dev/null 2>&1; then
    llvm-strip "$BIN_DIR/dropbear" "$BIN_DIR/dropbearkey" "$BIN_DIR/dbclient" "$BIN_DIR/scp" 2>/dev/null || true
  elif command -v strip >/dev/null 2>&1; then
    strip "$BIN_DIR/dropbear" "$BIN_DIR/dropbearkey" "$BIN_DIR/dbclient" "$BIN_DIR/scp" 2>/dev/null || true
  fi
fi

# Basic host-side smoke test under qemu-user when available.
qemu_cmd=""
case "$TARGET_ARCH" in
  aarch64)
    command -v qemu-aarch64 >/dev/null 2>&1 && qemu_cmd="qemu-aarch64"
    ;;
  armv7|armv7h|armhf)
    command -v qemu-arm >/dev/null 2>&1 && qemu_cmd="qemu-arm"
    ;;
esac
if [[ -n "$qemu_cmd" ]]; then
  "$qemu_cmd" "$BIN_DIR/dropbear" -V >/dev/null
  "$qemu_cmd" "$BIN_DIR/dropbearkey" -h >/dev/null
fi

echo "dropbear: outputs"
ls -lh "$BIN_DIR/dropbear" "$BIN_DIR/dropbearkey" "$BIN_DIR/dbclient" "$BIN_DIR/scp"
