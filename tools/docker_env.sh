# Sourced by the tools that build or run code in Docker (build.sh, harness/*.sh, usbaudio/*.sh).
# Needs HERE (the directory of the sourcing script). Sets:
#   TOOLS     this tools/ directory
#   FW        the firmware checkout: FW_DIR (relative to the current directory, or absolute), else
#             the checkout these tools are in; as a Docker-friendly path (D:/... on Windows)
#   IMAGE     the build image, tagged with a hash of tools/build-env/Dockerfile (built if missing),
#             so checkouts with different Dockerfiles never overwrite each other's image
#   MOUNTS    docker run arguments: FW at /src, plus the ChibiOS and uf2 submodules from the main
#             checkout (read only) when FW is a git worktree without its own copies
# and the function in_docker <command...>: runs a command in IMAGE at /src.
# Inside the image itself (STRISO_IN_IMAGE, set by the Dockerfile: CI, or a shell in the
# container) the commands run directly in the checkout instead.
_STRISO_TOOLS_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
_winpath() { (cd "$1" && pwd -W 2>/dev/null || pwd); }
TOOLS="$(_winpath "$_STRISO_TOOLS_DIR")"
FW="$(_winpath "${FW_DIR:-$_STRISO_TOOLS_DIR/..}")"

_df="$_STRISO_TOOLS_DIR/build-env/Dockerfile"
if [ -n "${STRISO_IN_IMAGE:-}" ]; then
  IMAGE="(this image)"
  MOUNTS=()
  in_docker() { (cd "$FW" && "$@"); }
else
IMAGE="striso-fw:$( (sha256sum "$_df" 2>/dev/null || shasum -a 256 "$_df") | cut -c1-12)"
if ! docker image inspect "$IMAGE" >/dev/null 2>&1; then
  echo "building the Docker image $IMAGE (once)" >&2
  docker build -q -t "$IMAGE" "$_STRISO_TOOLS_DIR/build-env" >/dev/null
fi

# git worktrees don't have the submodules checked out: use the main checkout's
MOUNTS=(-v "$FW:/src")
_common="$(git -C "$FW" rev-parse --path-format=absolute --git-common-dir 2>/dev/null || true)"
for _sub in ChibiOS:os/rt/include/ch.h uf2:utils/uf2conv.py; do
  _name="${_sub%%:*}"; _probe="${_sub#*:}"
  [ -f "$FW/third_party/$_name/$_probe" ] && continue
  for _cand in "$_common/../third_party/$_name" "$_common/../$_name"; do
    if [ -n "$_common" ] && [ -f "$_cand/$_probe" ]; then
      MOUNTS+=(-v "$(_winpath "$_cand"):/src/third_party/$_name:ro"); break
    fi
  done
done

in_docker() { MSYS_NO_PATHCONV=1 docker run --rm "${MOUNTS[@]}" -w /src "$IMAGE" "$@"; }
fi

# a build directory made by another image (e.g. before a toolchain update) is started afresh:
# its CMake cache names compilers that may not exist in this one
image_build_dir() {
  local d="$FW/$1"
  if [ -f "$d/CMakeCache.txt" ] && [ "$(cat "$d/.striso-image" 2>/dev/null)" != "$IMAGE" ]; then
    echo "build/${1#build/}: made with another build image, starting it afresh" >&2
    rm -rf "$d"
  fi
  mkdir -p "$d" && echo "$IMAGE" > "$d/.striso-image"
}

# the version string's git part, from the host (inside Docker a worktree's .git link points nowhere)
striso_git_version() {
  local d v
  d="$(git -C "$FW" --no-pager show -s --date=short --format=%ad)" || return 1
  v="$(git -C "$FW" --no-pager describe --tags --always --long --dirty)" || return 1
  echo "${d}_${v}"
}
