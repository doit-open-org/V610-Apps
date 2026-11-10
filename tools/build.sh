#!/bin/sh
# Build applications of this repository with hifw.py, for callers that have
# not sourced the SDK's export.sh (a surrounding build, a CI job):
#
#   APP_SDK_PATH=<V610-App-SDK> tools/build.sh [<app> ...]
#   tools/build.sh --list
#
# An application is named by its path under the repository
# (examples/mipk/mipk_object_detection), by its directory name alone when that is
# unambiguous, or by a path. Without a name every application is built.
#
# CV610_TOOLCHAIN_PATH, when set, is a compiler bin directory to put on
# PATH; JOBS limits the parallel jobs.
set -eu

APPS_ROOT="$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)"

# Applications are the <level>/<category>/<name> directories holding a project.
list_apps() {
    for cmake_file in "$APPS_ROOT"/*/*/*/CMakeLists.txt; do
        app_dir="$(dirname "$cmake_file")"
        [ -d "$app_dir/main" ] || continue
        echo "${app_dir#"$APPS_ROOT"/}"
    done
}

# Print the directory of the application named by $1.
find_app() {
    if [ -f "$1/CMakeLists.txt" ] && [ -d "$1/main" ]; then
        (CDPATH= cd -- "$1" && pwd)
        return
    fi
    matches="$(list_apps | grep -E "(^|/)$1\$" || true)"
    case "$(printf '%s\n' "$matches" | grep -c .)" in
        1) echo "$APPS_ROOT/$matches" ;;
        0) echo "no application '$1'; see tools/build.sh --list" >&2; return 1 ;;
        *) echo "'$1' is ambiguous:" >&2; printf '  %s\n' $matches >&2; return 1 ;;
    esac
}

if [ "${1:-}" = "--list" ]; then
    list_apps
    exit 0
fi

if [ -z "${APP_SDK_PATH:-}" ] || [ ! -f "${APP_SDK_PATH}/hifw.py" ]; then
    echo "V610-App-SDK not found; set APP_SDK_PATH to it" >&2
    exit 1
fi
export APP_SDK_PATH
if [ -n "${CV610_TOOLCHAIN_PATH:-}" ]; then
    PATH="${CV610_TOOLCHAIN_PATH}:${PATH}"
    export PATH
fi
if [ -z "${JOBS:-}" ]; then
    JOBS="$(nproc)"
fi

if [ "$#" -eq 0 ]; then
    set -- $(list_apps)
fi

for app in "$@"; do
    project_dir="$(find_app "$app")"
    echo "[V610-Apps] building ${project_dir#"$APPS_ROOT"/}"
    if [ ! -f "${project_dir}/build/hifw.conf" ]; then
        python3 "${APP_SDK_PATH}/hifw.py" -C "${project_dir}" set-target hi3516cv610
    fi
    python3 "${APP_SDK_PATH}/hifw.py" -C "${project_dir}" build --jobs "${JOBS}"
done
