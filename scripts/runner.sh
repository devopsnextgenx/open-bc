#!/usr/bin/env bash
# runner.sh - build and run local GitHub Actions self-hosted runners in Docker.
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
DOCKER="${DOCKER:-docker}"
IMAGE_BASE="openbc-runner"

COMMAND=""
TARGET="all"
TOKEN="${RUNNER_TOKEN:-}"
REPO_URL="${REPO_URL:-}"
PREFIX="openbc-local"
NO_CACHE=0
DO_BUILD=0
RUNNER_VERSION="latest"

usage() {
    cat <<'EOF'
runner.sh - build and run local GitHub Actions runners (Docker)

Usage:
  runner.sh <command> [options]

Commands:
  build     Build the runner image(s)
  start     Start runner container(s) and register them with GitHub
            (builds the image automatically if it does not exist)
  stop      Stop runner container(s); they deregister from GitHub on the way out
  status    Show runner containers
  logs      Show runner logs (follows when a single runner is selected)
  help      Show this help

Options:
  -r, --runner <debian|fedora|all>   Which runner (default: all)
                                       debian = .deb + Flatpak job
                                       fedora = .rpm job
  -t, --token <TOKEN>                Registration token (start). Also read from $RUNNER_TOKEN.
                                       Repo > Settings > Actions > Runners > New self-hosted runner.
                                       Valid for ~1 hour; one token registers both runners.
  -u, --url <URL>                    Repository URL (start). Also read from $REPO_URL;
                                       defaults to this directory's git "origin" remote.
  -n, --name-prefix <PREFIX>         Container/runner name prefix (default: openbc-local)
  -b, --build                        With start: rebuild the image(s) first
      --no-cache                     With build/--build: ignore the Docker cache
      --runner-version <X.Y.Z>       With build: pin the Actions runner version (default: latest)
  -h, --help                         Show this help

Runner labels:  self-hosted + openbc-debian / openbc-fedora
The debian container runs --privileged (flatpak-builder needs namespaces) and keeps
named volumes for the Flatpak runtimes and Cargo registry, so reruns are fast.

Examples:
  ./runner.sh build                              # build both images
  ./runner.sh build -r fedora --no-cache
  ./runner.sh start -t AAAA... -u https://github.com/me/openbc
  ./runner.sh start -r debian -t AAAA... --build
  ./runner.sh logs -r debian
  ./runner.sh stop

Then trigger a build on these runners:
  gh workflow run release.yml -f runner=self-hosted
  (or GitHub > Actions > release > Run workflow > runner: self-hosted)

Env: DOCKER=podman to use podman instead of docker.
EOF
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
case "$TARGET" in
    all)           TARGETS=(debian fedora) ;;
    debian|fedora) TARGETS=("$TARGET") ;;
    *) die "--runner must be debian, fedora or all (got '$TARGET')" ;;
esac
command -v "$DOCKER" >/dev/null 2>&1 || die "'$DOCKER' not found in PATH"

image_of()     { echo "${IMAGE_BASE}:$1"; }
container_of() { echo "${PREFIX}-$1"; }
exists()       { "$DOCKER" container inspect "$1" >/dev/null 2>&1; }

# ---- commands ---------------------------------------------------------------
build_one() {
    local t="$1" args
    [ -f "$SCRIPT_DIR/Dockerfile.$t" ] || die "missing $SCRIPT_DIR/Dockerfile.$t"
    args=(build -f "$SCRIPT_DIR/Dockerfile.$t" -t "$(image_of "$t")" --build-arg "RUNNER_VERSION=$RUNNER_VERSION")
    if [ "$NO_CACHE" -eq 1 ]; then args+=(--no-cache); fi
    echo "==> Building $(image_of "$t")"
    "$DOCKER" "${args[@]}" "$SCRIPT_DIR"
}

stop_one() {
    local name; name="$(container_of "$1")"
    if exists "$name"; then
        echo "==> Stopping $name"
        "$DOCKER" stop -t 90 "$name" >/dev/null || true
        "$DOCKER" rm -f "$name" >/dev/null
    else
        echo "==> $name is not running"
    fi
}

start_one() {
    local t="$1" name image args
    name="$(container_of "$t")"; image="$(image_of "$t")"

    if [ "$DO_BUILD" -eq 1 ] || ! "$DOCKER" image inspect "$image" >/dev/null 2>&1; then
        build_one "$t"
    fi
    if exists "$name"; then stop_one "$t"; fi

    # Token goes through the environment, not argv, so it stays out of `ps`.
    export REG_TOKEN="$TOKEN"
    args=(run -d --name "$name"
          -e REPO_URL="$REPO_URL" -e REG_TOKEN
          -e RUNNER_NAME="$name" -e RUNNER_LABELS="openbc-$t"
          -v "${name}-cargo:/home/runner/.cargo/registry")
    if [ "$t" = "debian" ]; then
        args+=(--privileged -v "${name}-flatpak:/home/runner/.local/share/flatpak")
    fi
    args+=("$image")

    echo "==> Starting $name (label: openbc-$t)"
    "$DOCKER" "${args[@]}" >/dev/null
    sleep 5
    if [ "$("$DOCKER" inspect -f '{{.State.Running}}' "$name")" != "true" ]; then
        echo "---- $name exited early; logs:" >&2
        "$DOCKER" logs --tail 30 "$name" >&2 || true
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
        echo "==> Done. Runners should appear as Idle under Settings > Actions > Runners."
        echo "    Run the workflow with:  gh workflow run release.yml -f runner=self-hosted" ;;
    stop)
        for t in "${TARGETS[@]}"; do stop_one "$t"; done ;;
    status)
        for t in "${TARGETS[@]}"; do
            "$DOCKER" ps -a --filter "name=^$(container_of "$t")$" \
                --format 'table {{.Names}}\t{{.Status}}\t{{.Image}}'
        done ;;
    logs)
        if [ "${#TARGETS[@]}" -eq 1 ]; then
            "$DOCKER" logs -f "$(container_of "${TARGETS[0]}")"
        else
            for t in "${TARGETS[@]}"; do
                echo "==== $(container_of "$t")"
                "$DOCKER" logs --tail 50 "$(container_of "$t")" || true
            done
        fi ;;
esac