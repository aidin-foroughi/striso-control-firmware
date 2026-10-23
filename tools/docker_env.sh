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
_STRISO_TOOLS_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
_winpath() { (cd "$1" && pwd -W 2>/dev/null || pwd); }
TOOLS="$(_winpath "$_STRISO_TOOLS_DIR")"
FW="$(_winpath "${FW_DIR:-$_STRISO_TOOLS_DIR/..}")"

_df="$_STRISO_TOOLS_DIR/build-env/Dockerfile"
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

# the version string's git part, from the host (inside Docker a worktree's .git link points nowhere)
striso_git_version() {
  echo "$(git -C "$FW" --no-pager show -s --date=short --format=%ad)_$(git -C "$FW" --no-pager describe --tags --always --long --dirty)"
}
