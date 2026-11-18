#!/usr/bin/env python3
"""100,000 small write+flush bursts, like tmux split refresh/progress output."""
import sys


def main() -> int:
    for i in range(100_000):
        sys.stdout.write(f"\rprogress {i:06d}")
        sys.stdout.flush()
    sys.stdout.write("\n")
    sys.stdout.flush()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
