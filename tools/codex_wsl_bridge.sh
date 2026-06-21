#!/usr/bin/env bash
set -eu

ROOT_DIR="$(cd "$(dirname "$0")/.." && pwd)"
BRIDGE_DIR="$ROOT_DIR/.codex-bridge"
JOBS_DIR="$BRIDGE_DIR/jobs"
RUNNING_DIR="$BRIDGE_DIR/running"
DONE_DIR="$BRIDGE_DIR/done"
RESULTS_DIR="$BRIDGE_DIR/results"
HEARTBEAT_FILE="$BRIDGE_DIR/heartbeat.txt"
STATUS_FILE="$BRIDGE_DIR/status.txt"

mkdir -p "$JOBS_DIR" "$RUNNING_DIR" "$DONE_DIR" "$RESULTS_DIR"

echo "codex-wsl-bridge: root=$ROOT_DIR"
echo "idle" >"$STATUS_FILE"

while true; do
    date '+%Y-%m-%d %H:%M:%S' >"$HEARTBEAT_FILE"

    job_file="$(find "$JOBS_DIR" -maxdepth 1 -type f -name '*.sh' | sort | head -n 1 || true)"
    if [ -z "$job_file" ]; then
        sleep 1
        continue
    fi

    job_name="$(basename "$job_file")"
    run_file="$RUNNING_DIR/$job_name"
    done_file="$DONE_DIR/$job_name"
    result_log="$RESULTS_DIR/${job_name%.sh}.log"
    result_exit="$RESULTS_DIR/${job_name%.sh}.exit"

    mv "$job_file" "$run_file"
    echo "running $job_name" >"$STATUS_FILE"

    {
        echo "===== JOB $job_name START $(date '+%Y-%m-%d %H:%M:%S') ====="
        cd "$ROOT_DIR"
        bash "$run_file"
        rc=$?
        echo "===== JOB $job_name EXIT $rc $(date '+%Y-%m-%d %H:%M:%S') ====="
        printf '%s\n' "$rc" >"$result_exit"
        exit "$rc"
    } >"$result_log" 2>&1 || true

    mv "$run_file" "$done_file"
    echo "idle" >"$STATUS_FILE"
done
