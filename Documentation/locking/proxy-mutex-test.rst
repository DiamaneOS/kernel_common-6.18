.. SPDX-License-Identifier: GPL-2.0-only

Mutex proxy-execution regression tests
=====================================

These opt-in KUnit tests intentionally acquire a mutex recursively. Run them
in a disposable kernel with two allowed online CPUs: a FIFO waiter and a
heartbeat share one CPU, while the controller uses the other. Insufficient
CPUs fail the test instead of producing a successful run consisting of skips.

From the kernel source root, build and run with proxy execution enabled::

    ./tools/testing/kunit/kunit.py run --arch=x86_64 \
        --kunitconfig=kernel/locking --build_dir=.kunit-proxy-mutex \
        --qemu_args='-smp 2' --kernel_args='sched_proxy_exec=1'

Repeat using the same build with ``--kernel_args='sched_proxy_exec=0'``.
Each of the six cases has two parameter variants, ``normal_spin`` and
``hook_no_spin``. Require all twelve variants to pass on each run, with no
skips. The matrix therefore executes twenty-four case/parameter combinations
across the two boot settings. ``CONFIG_NR_CPUS=2`` sets the kernel limit; it
does not make QEMU start two CPUs without ``-smp 2``.

The hooks affect only the mutex owned by that test instance. The initial-spin
hook rejects optimistic spinning; the spin-loop hook also stops waiter
spinning, which bypasses the initial check. This exercises normal and
suppressed spinning in one build without changing ``MUTEX_SPIN_ON_OWNER``.
It does not provide coverage of a kernel compiled with spinning removed.

Recursive cases must remain blocked until cancellation, then return
``-EINTR``. They also verify original ownership, continued heartbeat progress
and zero self-spin iterations. Ordinary handoff must succeed; cancellation
must return ``-EINTR``. Cleanup can still handle an unexpected successful
recursive acquisition on a regressed kernel without accepting it as a pass.

A once-only ``mutex: recursive locking`` diagnostic is expected from the
intentional recursion. It includes the task and caller. This uses a log
message rather than ``WARN_ONCE`` so ``panic_on_warn`` does not turn the
containment fix into a panic. Lock debugging configurations have their own
recursive-lock diagnostics and are excluded by the test Kconfig dependencies.

The fix contains CPU starvation from one-task recursive locking. It does not
make recursive acquisition valid, resolve an uninterruptible deadlock, or
detect longer dependency cycles such as ABBA. Those require separate tests.
