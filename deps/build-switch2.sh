#!/usr/bin/env bash
# Reproducible cross-compilation of the libpeer WebRTC stack for Nintendo Switch 2
# (ounce-devkit / libounce).
#
# Builds (in order): mbedtls (libpeer's pinned 3.4.0, with DTLS-SRTP enabled),
# usrsctp, libsrtp (mbedtls crypto backend) and libpeer itself, and installs
# everything into deps/switch2/ (lib/*.a + include/).
#
# Usage:
#   bash deps/build-switch2.sh             # host -> runs inside ounce-devkit docker
#   bash deps/build-switch2.sh --in-place  # already inside container with ounce-devkit installed
#
# Internal: "--inside" is passed when re-executing inside a container.

set -euo pipefail

LIBPEER_REPO="https://github.com/sepfy/libpeer"
LIBPEER_REF="9319aa434cb9e893faed0293ba9d2a21eca59c8b"
DOCKER_IMAGE="${OUNCE_DOCKER_IMAGE:-ounce-devkit}"

run_build() {
  local deps_root="$1"
  local devkitpro="${DEVKITPRO:-/opt/devkitpro}"
  if [[ ! -f "$devkitpro/switch2vars.sh" ]]; then
    echo "switch2vars.sh not found in $devkitpro (is ounce-devkit installed?)" >&2
    exit 1
  fi

  # shellcheck disable=SC1091
  source "$devkitpro/switch2vars.sh"
  export PATH="$devkitpro/portlibs/switch2/bin:$devkitpro/portlibs/switch/bin:$PATH"

  local prefix="$deps_root/switch2"
  local shim="$deps_root/shim/include"
  local src="$deps_root/src/libpeer"
  local jobs
  jobs="$(nproc)"

  rm -rf "$prefix"
  mkdir -p "$prefix"

  build() { # name srcdir extra-cflags... -- cmake-args...
    local name="$1" dir="$2"; shift 2
    local cflags=()
    while [[ $# -gt 0 && "$1" != "--" ]]; do cflags+=("$1"); shift; done
    [[ "${1:-}" == "--" ]] && shift
    echo "=== [$name] configure (Switch 2)"
    rm -rf "$dir/build-switch2"
    CFLAGS="${cflags[*]:-}" aarch64-none-elf-cmake -S "$dir" -B "$dir/build-switch2" \
      -DCMAKE_BUILD_TYPE=Release \
      -DCMAKE_INSTALL_PREFIX="$prefix" \
      "$@"
    echo "=== [$name] build (Switch 2)"
    cmake --build "$dir/build-switch2" -j"$jobs"
    echo "=== [$name] install (Switch 2)"
    cmake --install "$dir/build-switch2"
  }

  # 1) mbedtls 3.4.0 (libpeer's pinned submodule), DTLS-SRTP enabled via patch.
  build mbedtls "$src/third_party/mbedtls" -- \
    -DENABLE_TESTING=OFF -DENABLE_PROGRAMS=OFF -DGEN_FILES=OFF

  # 2) usrsctp (IPv4 only).
  build usrsctp "$src/third_party/usrsctp" \
    -D_DEFAULT_SOURCE -D_BSD_SOURCE "-I$shim" -include switch_missing.h -- \
    -Dsctp_werror=0 -Dsctp_build_programs=0 -Dsctp_build_shared_lib=0 \
    -Dsctp_debug=0 -Dsctp_inet6=0

  # 3) libsrtp with the mbedtls crypto engine built in step 1.
  build libsrtp "$src/third_party/libsrtp" \
    "-I$prefix/include" "-I$shim" -include switch_missing.h -- \
    -DENABLE_MBEDTLS=ON -DTEST_APPS=OFF -DBUILD_SHARED_LIBS=OFF \
    -DMBEDTLS_ROOT_DIR="$prefix" -DCMAKE_PREFIX_PATH="$prefix"

  # 4) libpeer core (signaling disabled).
  build libpeer "$src" \
    "-I$prefix/include" "-I$shim" -DLOG_REDIRECT=1 -DLOG_LEVEL=2 -- \
    -DDISABLE_PEER_SIGNALING=ON -DCMAKE_PREFIX_PATH="$prefix"

  # 5) Link smoke test against libounce (not libnx!).
  echo "=== smoke test link (Switch 2 / libounce)"
  local switch2_cpu
  switch2_cpu="$(sed -n 's/^SWITCH2_CPU[[:space:]]*:=[[:space:]]*//p' "$devkitpro/libounce/switch2_cpu.mk")"
  cat > /tmp/smoketest_s2.c <<'EOF'
#include <peer.h>
#include <peer_connection.h>
#include <stddef.h>

struct ifaddrs;
void peer_log(char* lvl, const char* file, int line, const char* fmt, ...) {}
int getifaddrs(struct ifaddrs** ifap) { return -1; }
void freeifaddrs(struct ifaddrs* ifa) {}
unsigned int if_nametoindex(const char* ifname) { return 0; }
char* if_indextoname(unsigned int ifindex, char* ifname) { return 0; }
int mbedtls_hardware_poll(void* data, unsigned char* output, size_t len,
                          size_t* olen) { return 0; }

int main(void) {
  peer_init();
  PeerConfiguration config = {0};
  PeerConnection* pc = peer_connection_create(&config);
  peer_connection_create_offer(pc);
  peer_connection_loop(pc);
  peer_connection_destroy(pc);
  peer_deinit();
  return 0;
}
EOF
  "$devkitpro/devkitA64/bin/aarch64-none-elf-gcc" \
    $switch2_cpu -mtp=soft -fPIE -D__SWITCH__ -D__SWITCH2__ \
    -I"$prefix/include" -I"$devkitpro/libounce/include" \
    /tmp/smoketest_s2.c \
    -specs="$devkitpro/libounce/switch.specs" \
    -L"$prefix/lib" -L"$devkitpro/libounce/lib" -L"$devkitpro/portlibs/switch/lib" \
    -lpeer -lsrtp2 -lusrsctp -lmbedtls -lmbedx509 -lmbedcrypto -lnx -lm \
    -o /tmp/smoketest_s2.elf
  echo "=== smoke test link (Switch 2) OK"
}

if [[ "${1:-}" == "--inside" ]]; then
  run_build /work/deps
  exit 0
fi

DEPS_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
LIBPEER_DIR="$DEPS_DIR/src/libpeer"

command -v git >/dev/null || { echo "git is required" >&2; exit 1; }

# 1) Clone the pinned commit (shallow, with submodules) if missing.
if [[ ! -e "$LIBPEER_DIR/.git" ]]; then
  mkdir -p "$DEPS_DIR/src"
  git init -q "$LIBPEER_DIR"
  git -C "$LIBPEER_DIR" remote add origin "$LIBPEER_REPO"
  git -C "$LIBPEER_DIR" fetch --depth 1 origin "$LIBPEER_REF"
  git -C "$LIBPEER_DIR" checkout -q FETCH_HEAD
  git -C "$LIBPEER_DIR" submodule update --init --recursive --depth 1
fi

# 2) Apply patches (idempotent).
apply_patch() {
  local dir="$1" patch="$2"
  if git -C "$dir" apply --reverse --check "$patch" >/dev/null 2>&1; then
    echo "patch already applied: $(basename "$patch")"
  else
    echo "applying patch: $(basename "$patch")"
    git -C "$dir" apply "$patch"
  fi
}
apply_patch "$LIBPEER_DIR" "$DEPS_DIR/patches/libpeer-switch.patch"
apply_patch "$LIBPEER_DIR/third_party/mbedtls" "$DEPS_DIR/patches/mbedtls-switch.patch"
apply_patch "$LIBPEER_DIR/third_party/usrsctp" "$DEPS_DIR/patches/usrsctp-switch.patch"

if [[ "${1:-}" == "--in-place" ]]; then
  run_build "$DEPS_DIR"
else
  command -v docker >/dev/null || { echo "docker is required (or pass --in-place inside ounce-devkit container)" >&2; exit 1; }
  rm -rf "$DEPS_DIR/switch2"
  docker run --rm -v "$DEPS_DIR":/work/deps "$DOCKER_IMAGE" \
    bash /work/deps/build-switch2.sh --inside
  docker run --rm -v "$DEPS_DIR":/work/deps "$DOCKER_IMAGE" \
    chown -R "$(id -u):$(id -g)" /work/deps/switch2 /work/deps/src >/dev/null 2>&1 || true
fi

echo
echo "=== installed Switch 2 artifacts ==="
for lib in libpeer.a libsrtp2.a libusrsctp.a libmbedtls.a libmbedcrypto.a libmbedx509.a; do
  if [[ -f "$DEPS_DIR/switch2/lib/$lib" ]]; then
    echo "OK  $DEPS_DIR/switch2/lib/$lib ($(ar t "$DEPS_DIR/switch2/lib/$lib" | wc -l | tr -d ' ') objects)"
  else
    echo "MISSING  $DEPS_DIR/switch2/lib/$lib" >&2
    exit 1
  fi
done
echo
echo "Done. Headers in $DEPS_DIR/switch2/include, libraries in $DEPS_DIR/switch2/lib."
