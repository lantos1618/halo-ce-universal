#!/usr/bin/env python3
"""Summarise a HALO_PROFILE=1 profile.txt (port/macos/host/host_profile.c).

The profile samples every thread; most of them wait most of the time. This
prints, for each thread that did work, its busy samples (those not in a
kernel wait) and its hottest functions, and the guest's hottest functions
over all threads:

    tools/perf_lab/profile_summary.py build/macos/Halo/profile.txt [top]
"""

import re
import sys

IDLE = ("psynch_cvwait", "workq_kernreturn", "semaphore_wait_trap", "ulock_wait2", "mach_msg2_trap",
        "semwait_signal", "semaphore_wait_signal_trap", "semaphore_timedwait_trap", "kevent", "__select",
        "iokit_user_client_trap", "swtch_pri", "__psynch_mutexwait")


def main():
    path = sys.argv[1]
    top = int(sys.argv[2]) if len(sys.argv) > 2 else 12
    threads = {}
    totals = {}
    listed = {}
    thread = None
    in_guest_list = False
    for line in open(path, encoding="utf-8", errors="replace"):
        if line.startswith("guest functions:"):
            in_guest_list = True
            continue
        match = re.match(r"thread (\d+) \((\d+) samples\)", line)
        if match:
            in_guest_list = False
            thread = int(match.group(1))
            threads[thread] = (int(match.group(2)), [])
            continue
        match = re.match(r"\s*([\d.]+)%\s+(\d+)\s+(host|guest)\s+(.*)", line)
        if match and in_guest_list:
            listed[match.group(4).strip()] = int(match.group(2))
            continue
        if thread is None or not match:
            continue
        samples, side, name = int(match.group(2)), match.group(3), match.group(4).strip()
        if any(word in name for word in IDLE):
            continue
        threads[thread][1].append((samples, side, name))
        if side == "guest":
            totals[name] = totals.get(name, 0) + samples
    for thread, (count, rows) in sorted(threads.items()):
        busy = sum(row[0] for row in rows)
        if busy < count * 0.02:
            continue
        rows.sort(reverse=True)
        print("thread %d: %d of %d samples busy (%.1f%%)" % (thread, busy, count, 100.0 * busy / count))
        for samples, side, name in rows[:top]:
            print("  %6d %5.1f%%  %-5s %s" % (samples, 100.0 * samples / count, side, name[:90]))
    # (the profile's own list of the guest's functions, which goes further
    # than each thread's, when it has one)
    totals = listed or totals
    print("guest functions, every thread:")
    for name, samples in sorted(totals.items(), key=lambda item: -item[1])[:top * 2]:
        print("  %6d  %s" % (samples, name))


if __name__ == "__main__":
    main()
