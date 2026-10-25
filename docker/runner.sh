#!/usr/bin/env bash
# runner.sh - build and run local GitHub Actions self-hosted runners in Docker.
# gh api -X POST repos/devopsnextgenx/open-bc/actions/runners/registration-token --jq .token
set -euo pipefail

# Git Bash on Windows rewrites arguments that look like paths (-v name:/home/..., URLs); keep them as-is.
export MSYS_NO_PATHCONV=1

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
DOCKER="${DOCKER:-docker}"
IMAGE_BASE="docker.io/amitkshirsagar13/openbc-runner"

COMMAND=""
TARGET="all"
TOKEN="${RUNNER_TOKEN:-}"
REPO_URL="${REPO_URL:-}"
PREFIX="openbc-local"
NO_CACHE=0
DO_BUILD=0
RUNNER_VERSION="latest"

usage() {
    cat <<'USAGE'
runner.sh - build and run local GitHub Actions runners (Docker)

Usage:
  runner.sh <command> [options]

Commands:
  build     Build the runner image(s)
  start     Start runner container(s) and register them with GitHub
            (builds the image automatically if it does not exist)
  stop      Stop runner container(s)
  status    Show runner containers
  logs      Show runner logs (follows when a single runner is selected)
  help      Show this help

Options:
  -r, --runner <list>                Comma-separated: debian, fedora, windows, all (default: all)
                                       debian  = .deb + Flatpak job          (Linux container)
                                       fedora  = .rpm job                    (Linux container)
                                       windows = .msi job                    (Windows container)
                                       all     = debian + fedora (windows must be requested explicitly)
                                       e.g.  -r debian,fedora,windows
  -t, --token <TOKEN>                Registration token (start). Also read from $RUNNER_TOKEN.
                                       Repo > Settings > Actions > Runners > New self-hosted runner,
                                       or: gh api -X POST repos/devopsnextgenx/open-bc/actions/runners/registration-token --jq .token
                                       Valid ~1 hour; one token registers all runners.
  -u, --url <URL>                    Repository URL (start). Also read from $REPO_URL;
                                       defaults to this directory's git "origin" remote.
  -n, --name-prefix <PREFIX>         Container/runner name prefix (default: openbc-local)
  -b, --build                        With start: rebuild the image(s) first
      --no-cache                     With build/--build: ignore the Docker cache
      --runner-version <X.Y.Z>       With build: pin the Actions runner version (default: latest)
  -h, --help                         Show this help

Runner labels:  self-hosted + openbc-debian / openbc-fedora / openbc-windows

Docker engines:
  Linux runners use context "desktop-linux", the Windows runner uses "desktop-windows" when those
  contexts exist (Docker Desktop). Override with LINUX_DOCKER_CONTEXT / WINDOWS_DOCKER_CONTEXT.
  The Windows runner needs Docker Desktop's Windows containers enabled (tray icon > "Switch to
  Windows containers", once). Tunables: WINDOWS_ISOLATION (default hyperv), WINDOWS_MEMORY (default 8g),
  WINDOWS_CPUS (default: unset).
  The debian container runs --privileged (flatpak-builder needs namespaces) and keeps named volumes
  for Flatpak runtimes and the Cargo registry.

Examples:
  ./runner.sh build                              # debian + fedora images
  ./runner.sh build -r windows                   # slow first time (several GB)
  ./runner.sh start -t AAAA... -u https://github.com/me/repo
  ./runner.sh start -r debian,fedora,windows -t AAAA...
  ./runner.sh start -r debian -t AAAA... --build
  ./runner.sh logs -r windows
  ./runner.sh stop -r all

Then trigger a build on these runners:
  gh workflow run release.yml -f runner=self-hosted                         # deb, flatpak, rpm
  gh workflow run release.yml -f runner=self-hosted -f windows_runner=self-hosted   # + msi

Env: DOCKER=podman to use podman instead of docker.
USAGE
}

die() { echo "error: $*" >&2; exit 1; }
need_val() { [ "$#" -ge 2 ] && [ -n "$2" ] || die "option $1 needs a value"; }

# ---- argument parsing -------------------------------------------------------
if [ "$#" -eq 0 ]; then usage; exit 0; fi

while [ "$#" -gt 0 ]; do
    case "$1" in
        -h|--help|help) usage; exit 0 ;;
        build|start|stop|status|logs)
            [ -z "$COMMAND" ] || die "only one command allowed (got '$COMMAND' and '$1')"
            COMMAND="$1"; shift ;;
        -r|--runner)         need_val "$@"; TARGET="$2"; shift 2 ;;
        -t|--token)          need_val "$@"; TOKEN="$2"; shift 2 ;;
        -u|--url)            need_val "$@"; REPO_URL="$2"; shift 2 ;;
        -n|--name-prefix)    need_val "$@"; PREFIX="$2"; shift 2 ;;
        --runner-version)    need_val "$@"; RUNNER_VERSION="$2"; shift 2 ;;
        -b|--build)          DO_BUILD=1; shift ;;
        --no-cache)          NO_CACHE=1; shift ;;
        *) die "unknown argument '$1' (see --help)" ;;
    esac
done

[ -n "$COMMAND" ] || { usage; exit 1; }

TARGETS=()
IFS=',' read -ra REQUESTED <<< "$TARGET"
for r in "${REQUESTED[@]}"; do
    case "$r" in
        all)                     TARGETS+=(debian fedora) ;;
        debian|fedora|windows)   TARGETS+=("$r") ;;
        *) die "--runner entries must be debian, fedora, windows or all (got '$r')" ;;
    esac
done
command -v "$DOCKER" >/dev/null 2>&1 || die "'$DOCKER' not found in PATH"

image_of()     { echo "${IMAGE_BASE}:$1"; }
container_of() { echo "${PREFIX}-$1"; }

# docker wrapper: picks the Linux or Windows engine (Docker Desktop contexts) for a target.
# usage: dk <target> <docker args...>
# docker wrapper: picks the Linux or Windows engine (Docker Desktop contexts) for a target.
# usage: dk <target> <docker args...>
dk() {
    local t="$1" ctx; shift
    if [ "$t" = "windows" ]; then ctx="${WINDOWS_DOCKER_CONTEXT:-desktop-windows}"
    else ctx="${LINUX_DOCKER_CONTEXT:-desktop-linux}"; fi
    # Only pass --context when the engine actually advertises that context.
    # (podman has no --context flag; a stale Docker context shows up as [] from `inspect`.)
    if "$DOCKER" context ls --format '{{.Name}}' 2>/dev/null | grep -qx "$ctx"; then
        "$DOCKER" --context "$ctx" "$@"
    else
        "$DOCKER" "$@"
    fi
}
exists() { dk "$1" container inspect "$2" >/dev/null 2>&1; }

# ---- commands ---------------------------------------------------------------
build_one() {
    local t="$1" args
    [ -f "$SCRIPT_DIR/Dockerfile.$t" ] || die "missing $SCRIPT_DIR/Dockerfile.$t"
    args=(build -f "Dockerfile.$t" -t "$(image_of "$t")" --build-arg "RUNNER_VERSION=$RUNNER_VERSION")
    if [ "$NO_CACHE" -eq 1 ]; then args+=(--no-cache); fi
    if [ "$t" = "windows" ]; then args+=(--memory "${WINDOWS_MEMORY:-8g}"); fi
    echo "==> Building $(image_of "$t")"
    # Build from the script dir with relative paths (avoids /d/... vs D:\... path issues in Git Bash).
    (cd "$SCRIPT_DIR" && dk "$t" "${args[@]}" .)
}

stop_one() {
    local t="$1" name; name="$(container_of "$t")"
    if exists "$t" "$name"; then
        echo "==> Stopping $name"
        dk "$t" stop -t 90 "$name" >/dev/null || true
        dk "$t" rm -f "$name" >/dev/null
    else
        echo "==> $name is not running"
    fi
}

start_one() {
    local t="$1" name image args
    name="$(container_of "$t")"; image="$(image_of "$t")"

    if [ "$DO_BUILD" -eq 1 ] || ! dk "$t" image inspect "$image" >/dev/null 2>&1; then
        build_one "$t"
    fi
    if exists "$t" "$name"; then stop_one "$t"; fi

    # Token goes through the environment, not argv, so it stays out of `ps`.
    export REG_TOKEN="$TOKEN"
    args=(run -d --name "$name"
          -e REPO_URL="$REPO_URL" -e REG_TOKEN
          -e RUNNER_NAME="$name" -e RUNNER_LABELS="openbc-$t"
          -e ACTIONS_RUNNER_PRINT_LOG_TO_STDOUT=1)
    case "$t" in
        debian)
            args+=(--privileged
                   -v "${name}-cargo:/home/runner/.cargo/registry"
                   -v "${name}-flatpak:/home/runner/.local/share/flatpak") ;;
        fedora)
            args+=(-v "${name}-cargo:/home/runner/.cargo/registry") ;;
        windows)
            args+=(--isolation="${WINDOWS_ISOLATION:-hyperv}" --memory "${WINDOWS_MEMORY:-8g}")
            if [ -n "${WINDOWS_CPUS:-}" ]; then args+=(--cpus "$WINDOWS_CPUS"); fi ;;
    esac
    args+=("$image")

    echo "==> Starting $name (label: openbc-$t)"
    dk "$t" "${args[@]}" >/dev/null
    sleep 5
    if [ "$(dk "$t" inspect -f '{{.State.Running}}' "$name")" != "true" ]; then
        echo "---- $name exited early; logs:" >&2
        dk "$t" logs --tail 30 "$name" >&2 || true
        die "$name failed to start (expired token or wrong URL?)"
    fi
}

case "$COMMAND" in
    build)
        for t in "${TARGETS[@]}"; do build_one "$t"; done ;;
    start)
        [ -n "$TOKEN" ] || die "start needs a registration token (-t TOKEN or \$RUNNER_TOKEN)"
        if [ -z "$REPO_URL" ]; then
            REPO_URL="$(git -C "$SCRIPT_DIR" remote get-url origin 2>/dev/null \
                | sed -E 's#^git@github\.com:#https://github.com/#; s#\.git$##' || true)"
        fi
        [ -n "$REPO_URL" ] || die "start needs the repository URL (-u URL or \$REPO_URL)"
        for t in "${TARGETS[@]}"; do start_one "$t"; done
        echo "==> Done. Runners should appear as Idle under Settings > Actions > Runners." ;;
    stop)
        for t in "${TARGETS[@]}"; do stop_one "$t"; done ;;
    status)
        for t in "${TARGETS[@]}"; do
            dk "$t" ps -a --filter "name=^$(container_of "$t")$" \
                --format 'table {{.Names}}\t{{.Status}}\t{{.Image}}'
        done ;;
    logs)
        if [ "${#TARGETS[@]}" -eq 1 ]; then
            dk "${TARGETS[0]}" logs -f "$(container_of "${TARGETS[0]}")"
        else
            for t in "${TARGETS[@]}"; do
                echo "==== $(container_of "$t")"
                dk "$t" logs --tail 50 "$(container_of "$t")" || true
            done
        fi ;;
esac