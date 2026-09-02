#!/usr/bin/env python3
"""Stress-test producer_consumer.

Runs the compiled binary many times and verifies that the consumer
threads collectively print each letter a-z exactly twice (52 letters
total, per the README FAQ), regardless of interleaving order.
"""

import argparse
import os
import re
import subprocess
import sys
from collections import Counter
from concurrent.futures import ThreadPoolExecutor, as_completed
from pathlib import Path

SCRIPT_DIR = Path(__file__).resolve().parent
BINARY = SCRIPT_DIR / "producer_consumer"

# Fixed-format status messages the program prints. Each is emitted by a
# single printf/fprintf call (glibc serializes each such call on stdout),
# so a message is never split mid-string, but its position relative to
# other threads' single-character prints is nondeterministic. Stripping
# these (with \d+ for the variable thread ids / counts) leaves only the
# actual queued data characters behind, no matter how they interleaved.
FIXED_PATTERNS = [
    r"\nMain thread started with thread id \d+",
    r"\nProducer thread started with thread id \d+",
    r"\nConsumer thread started with thread id \d+",
    r"\n Values: ",
    r"\nPrinted \d+ characters - inner\n",
    r"\nPrinted \d+ characters\.\n",
]

EXPECTED = Counter({chr(c): 2 for c in range(ord("a"), ord("z") + 1)})


def extract_letters(stdout: str) -> str:
    remaining = stdout
    for pattern in FIXED_PATTERNS:
        remaining = re.sub(pattern, "", remaining)
    return remaining


def run_once(index: int, timeout: float):
    try:
        proc = subprocess.run(
            [str(BINARY)], capture_output=True, text=True, timeout=timeout
        )
    except subprocess.TimeoutExpired:
        return (index, False, f"timed out after {timeout}s (possible deadlock)", "")

    if proc.returncode != 0:
        reason = f"exited with code {proc.returncode} (stderr: {proc.stderr.strip()!r})"
        return (index, False, reason, proc.stdout)

    letters = extract_letters(proc.stdout)
    counts = Counter(letters)
    if counts != EXPECTED:
        missing = EXPECTED - counts
        extra = counts - EXPECTED
        reason = (
            f"letter mismatch: missing={dict(missing)} extra={dict(extra)} "
            f"total={len(letters)} (expected 52)"
        )
        return (index, False, reason, proc.stdout)

    return (index, True, "", "")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("-n", "--runs", type=int, default=10000,
                         help="number of times to run the binary (default: 10000)")
    parser.add_argument("-j", "--jobs", type=int, default=os.cpu_count() or 4,
                         help="number of runs to execute concurrently (default: cpu count)")
    parser.add_argument("-t", "--timeout", type=float, default=5.0,
                         help="per-run timeout in seconds, catches deadlocks (default: 5)")
    parser.add_argument("--max-failures-shown", type=int, default=5,
                         help="max number of failing runs to print details for")
    args = parser.parse_args()

    print(f"Building {BINARY.name} ...")
    subprocess.run(["make"], cwd=SCRIPT_DIR, check=True)

    if not BINARY.exists():
        print(f"error: {BINARY} not found after build", file=sys.stderr)
        sys.exit(1)

    print(f"Running {args.runs} iterations with {args.jobs} parallel jobs ...")

    failures = []
    completed = 0
    report_every = max(1, args.runs // 20)
    with ThreadPoolExecutor(max_workers=args.jobs) as pool:
        futures = [pool.submit(run_once, i, args.timeout) for i in range(1, args.runs + 1)]
        for future in as_completed(futures):
            index, ok, reason, stdout = future.result()
            completed += 1
            if not ok:
                failures.append((index, reason, stdout))
            if completed % report_every == 0 or completed == args.runs:
                print(f"...{completed}/{args.runs} runs completed ({len(failures)} failures so far)")

    print()
    if failures:
        failures.sort(key=lambda f: f[0])
        print(f"FAILED: {len(failures)}/{args.runs} runs did not print 52 letters (a-z twice each).")
        for index, reason, stdout in failures[: args.max_failures_shown]:
            print(f"\n--- run {index} ---")
            print(f"reason: {reason}")
            print("stdout:")
            print(stdout)
        if len(failures) > args.max_failures_shown:
            print(f"\n... and {len(failures) - args.max_failures_shown} more failures.")
        sys.exit(1)
    else:
        print(f"PASSED: {args.runs}/{args.runs} runs each printed exactly 52 letters (a-z twice each).")
        sys.exit(0)


if __name__ == "__main__":
    main()
