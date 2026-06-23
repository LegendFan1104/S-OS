#!/usr/bin/env python3
import argparse
import re
import sys
from collections import defaultdict


ANSI_RE = re.compile(r"\x1b\[[0-9;]*[A-Za-z]")
GROUP_START_RE = re.compile(r"#### OS COMP TEST GROUP START ([^#]+) ####")
GROUP_END_RE = re.compile(r"#### OS COMP TEST GROUP END ([^#]+) ####")
RUN_CASE_RE = re.compile(r"RUN LTP CASE (.+)")

SUMMARY_KEY_MAP = {
    "passed": "passed",
    "failed": "failed",
    "broken": "broken",
    "skipped": "skipped",
    "warnings": "warnings",
}

TAG_KEY_MAP = {
    "TPASS": "passed",
    "TFAIL": "failed",
    "TBROK": "broken",
    "TSKIP": "skipped",
    "TWARN": "warnings",
}


def strip_ansi(line):
    return ANSI_RE.sub("", line).rstrip("\n")


def new_run(case_name):
    return {
        "case": case_name,
        "lines": [],
        "summary": None,
    }


def empty_counts():
    return {
        "passed": 0,
        "failed": 0,
        "broken": 0,
        "skipped": 0,
        "warnings": 0,
    }


def parse_summary(lines):
    for i, line in enumerate(lines):
        if line.strip() != "Summary:":
            continue
        counts = empty_counts()
        seen = False
        for subline in lines[i + 1 : i + 8]:
            parts = subline.strip().split()
            if len(parts) != 2:
                continue
            key, value = parts
            if key not in SUMMARY_KEY_MAP:
                continue
            try:
                counts[SUMMARY_KEY_MAP[key]] = int(value)
                seen = True
            except ValueError:
                continue
        if seen:
            return counts
    return None


def parse_tags(lines):
    counts = empty_counts()
    for line in lines:
        for tag, key in TAG_KEY_MAP.items():
            if tag in line:
                counts[key] += 1
    return counts


def finalize_run(profile, run, stats):
    if profile not in ("musl", "glibc") or run is None:
        return

    summary_counts = parse_summary(run["lines"])
    if summary_counts is not None:
        counts = summary_counts
        has_summary = True
    else:
        counts = parse_tags(run["lines"])
        has_summary = False

    item = stats[profile][run["case"]]
    item["runs"] += 1
    item["summary_runs"] += 1 if has_summary else 0
    item["nosummary_runs"] += 0 if has_summary else 1
    for key, value in counts.items():
        item[key] += value


def detect_profile(group_name):
    name = group_name.strip()
    if name == "ltp-musl":
        return "musl"
    if name == "ltp-glibc":
        return "glibc"
    return None


def print_group(profile, entries):
    print(f"== {profile} ==")
    if not entries:
        print("(no cases found)")
        return

    ordered = sorted(
        entries.items(),
        key=lambda kv: (-kv[1]["passed"], kv[0]),
    )
    for case_name, item in ordered:
        summary_label = "yes" if item["nosummary_runs"] == 0 else (
            "no" if item["summary_runs"] == 0 else "mixed"
        )
        print(
            f"{case_name}\tpassed={item['passed']}\tfailed={item['failed']}"
            f"\tbroken={item['broken']}\tskipped={item['skipped']}"
            f"\twarnings={item['warnings']}\truns={item['runs']}"
            f"\tsummary={summary_label}"
        )


def read_lines(path):
    if path == "-":
        return sys.stdin.readlines()
    with open(path, "r", encoding="utf-8", errors="replace") as f:
        return f.readlines()


def main():
    parser = argparse.ArgumentParser(
        description="Parse LTP logs and summarize case pass/fail counts by musl/glibc."
    )
    parser.add_argument("logfile", help="Path to log file, or - for stdin")
    args = parser.parse_args()

    stats = {
        "musl": defaultdict(lambda: {"runs": 0, "summary_runs": 0, "nosummary_runs": 0, **empty_counts()}),
        "glibc": defaultdict(lambda: {"runs": 0, "summary_runs": 0, "nosummary_runs": 0, **empty_counts()}),
    }

    current_profile = None
    current_run = None

    for raw_line in read_lines(args.logfile):
        line = strip_ansi(raw_line)

        m = GROUP_START_RE.match(line)
        if m:
            finalize_run(current_profile, current_run, stats)
            current_run = None
            current_profile = detect_profile(m.group(1))
            continue

        m = GROUP_END_RE.match(line)
        if m:
            finalize_run(current_profile, current_run, stats)
            current_run = None
            current_profile = None
            continue

        m = RUN_CASE_RE.match(line)
        if m and current_profile in ("musl", "glibc"):
            finalize_run(current_profile, current_run, stats)
            current_run = new_run(m.group(1).strip())
            continue

        if current_run is not None:
            current_run["lines"].append(line)

    finalize_run(current_profile, current_run, stats)

    print_group("musl", stats["musl"])
    print()
    print_group("glibc", stats["glibc"])


if __name__ == "__main__":
    main()
