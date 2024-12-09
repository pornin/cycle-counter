# CPU Cycle Counter

Some/most modern CPUs have an in-silicon cycle counter, which, as the
name indicates, counts the number of clock cycles elapsed since some
arbitrary instant. It shall be noted that such counters are often
distinct from "time stamp counters". For instance, x86 CPUs have
featured a sort-of cycle counter, which can be read with the `rdtsc`
opcode, since the original Pentium (introduced in 1993). Such time stamp
counters originally matched the cycle counter, but this is no longer
true now that frequency scaling is a thing: CPUs will commonly lower or
raise their operating frequency depending on current load and
operational conditions (temperature, input voltage...), while
maintaining a fixed update frequency for the time stamp counter.

"Normal" applications are expected to use the time stamp counter, which
is convenient for time synchronization tasks (e.g. ensuring that video
frames show up on the screen at the right time) since the time stamp
counter is expected to track the time experienced in the physical world.
Accessing the cycle counter is useful for debugging and optimization
purposes, specifically for working out small, tight loops; this is the
case for instance in the realm of implementation of cryptographic
primitives.

While the in-CPU cycle counter exists, its access is normally forbidden
by the operating system for "security reasons"; namely, direct and
precise access to that counter helps in running timing attacks that
would allow an unprivileged process to extract secret values from other
processes running as different users. Of course, this attack model makes
sense only in multi-user machines; if you are trying to optimize a tight
loop on a test system which is sitting on your desk, then chances are
that there is a single user to that test system and that's you.
Nevertheless, operating system vendors are trying real hard to prevent
direct access to the cycle counter, and explain that if you want
performance counters then you should use the performance counter APIs,
which are a set of system calls and ad hoc structures by which you can
have the kernel read such counters for you, and report back. [In the
case of Linux](https://web.eece.maine.edu/~vweaver/projects/perf_events/),
this really entails using a specific system call to set up some special
file descriptors, and the value of any performance counter can either be
read with a `read()` system call, or possibly sampled at a periodic
frequency (e.g. 1000 Hz) and transmitted back to the userland process
via some shared memory. On other operating systems, a similar but of
course incompatible and possibly undocumented API can achieve about the
same result (e.g. on
[macOS](https://gist.github.com/ibireme/173517c208c7dc333ba962c1f0d67d12)).
Of course, access to these monitoring system calls normally requires
superuser privilege.

For tight loop optimization, which can be quite hairy in a world where
CPUs implement out-of-order execution, the performance monitoring system
calls are inadequate, since we really are interested in some short
sequences whose execution uses less time than an average system call,
let alone the million or so clock cycles that would happen before two
regular sampling events. For these tasks, you really need to read the
cycle counter directly from userland, with the smallest possible
sequence of instructions. This repository contains notes and some extra
software (e.g. Linux kernel module) to do just that.

Remember that:

  - Interpretation of a cycle count is delicate. You really need to
    consider both bandwidth and latency of instructions, and how they
    get executed on your CPU. For x86 machines, I recommend perusing
    Agner Fog's [optimization manuals](https://agner.org/optimize/),
    especially the [microarchitecture
    description](https://agner.org/optimize/microarchitecture.pdf).

  - Allowing access to performance counters *does* allow some precise
    timing attacks -- to be precise, these attacks were mostly feasible
    without the performance counters, but become quite easier. So, don't
    do that on a multi-tenant machine (though the wisdom of multi-tenant
    machines on modern hardware is a bit questionable these days).

  - On some of the smallest embedded systems (microcontrollers), maintaining
    the cycle counter can incur a noticeable increase in power draw; you'd
    prefer to reserve that to development boards, not production hardware.

  - Cycle counts are per-core. If the operating system decides to
    migrate your process from one core to another, then cycle counts
    will apparently "jump". This is in general not much of a problem for
    development, because the OS tries not to migrate processes (it's
    expensive). You can instruct the OS to "tie" a thread to a specific
    CPU core with the
    [`sched_setaffinity()`](https://man7.org/linux/man-pages/man2/sched_setaffinity.2.html)
    system call; you will want to do that if you have asymmetric
    hardware with "efficiency" and "performance" cores, so that you can
    bench performance on either type of core.

The [`test_cycle.c`](test_cycle.c) file is a demonstration application
that more-or-less expects to run on a Linux-like system; it uses the
cycle counter to benchmark the speed of integer multiplications. Compile
it and use it with a numerical parameter:

```
$ clang -W -Wextra -O2 -o test_cycle test_cycle.c
$ ./test_cycle 3
32x32->32 muls:    2.000
64x64->64 muls:    4.025
64x64->128 muls:   5.499
(225)
```

The parameter should be 0, 1 or 3; with 0 or 1, you mostly bench speed
of multiplications by 0 or 1, and with 3, you mostly bench speed of
multiplications by "large values". Here, on an ARM Cortex-A76 CPU, we
see that 32-bit multiplications complete in 2 cycles, 64-bit
multiplications complete in 4 cycles for the low output word, and 5
cycles for the high output word (the extra ".499" are some loop
overhead). On that particular CPU, these multiplications are
constant-time; this is not the case on, for instance, an ARM Cortex-A55:

```
$ ./test_cycle 1
32x32->32 muls:    3.001
64x64->64 muls:    4.001
64x64->128 muls:   6.127
(0)
$ ./test_cycle 3
32x32->32 muls:    3.001
64x64->64 muls:    5.001
64x64->128 muls:   6.127
(225)
```

We see here that on the A55, 64-bit multiplications return the low word
of the output earlier when the operands are mathematically small enough
(and this is a problem for cryptographic schemes; the early return may
allow secret-revealing timing attacks).

If you try this program on your machine, then chances are that it will
crash with an "illegal instruction" error, or something similar. This is
because access to the cycle counter must first be allowed, which requires
at least superuser access, as described below.

# x86

On x86 CPUs, the cycle counter can be read with the `rdpmc` opcode, using
the register `0x40000001`. A typical access would use this function:

```c
static inline uint64_t
core_cycles(void)
{
        _mm_lfence();
        return __rdpmc(0x40000001);
}
```

Note the use of a memory fence to ensure that the instruction executes in
a reasonably predictable position within the instruction sequence.

On a Linux system, to allow `rdpmc` to complete without crashing, the
access must first be authorized by root with:

```
echo 2 > /sys/bus/event_source/devices/cpu/rdpmc
```

Once this has been done, unpriviledged userland processes can read the
cycle counter without crashing. The setting "sticks" until the next
reboot. On my test/research system (running Ubuntu), I do that
automatically at boot time with an `@reboot` crontab.

I don't have any matching solution for Windows (sorry). On macOS this is
hardly better, you would need a kernel extension (aka macOS's notion of
a kernel module) and rumour has it that there is one somewhere in [Intel
Performance Counter Monitor](https://github.com/intel/pcm), but I have
not tried. Also, macOS makes it difficult to load custom kernel
extensions, you have to reboot to some recovery OS and disable some
checks. Also note that virtual machines won't save you here; regardless
of what you do in a VM, you won't be able to access the cycle counter if
the host does not agree.

# ARMv8

On ARMv8 (ARMv8-A in 64-bit mode, aka "aarch64" or "arm64"), the cycle
counter is read with a bit of inline assembly:

```c
static inline uint64_t
core_cycles(void)
{
        uint64_t x;
        __asm__ __volatile__ ("dsb sy\n\tmrs %0, pmccntr_el0" : "=r" (x) : : );
        return x;
}
```

The `dsb` opcode is a memory fence, just like in the x86 case. For the
cycle counter to be accessible, it must be enabled, and unprivileged read
must be authorized, both operations needing to be done in supervisor mode
(i.e. in the kernel). On Linux, you can use the [cyccnt](cyccnt) custom
module from this repository. Namely:

  - You need to install the kernel headers that match your kernel.
    Possibly this is already done; otherwise, your distribution may
    provide a convenient package that is kept in sync with the kernel
    itself. On Ubuntu, try the `linux-headers-generic` package.

  - Go to the `cyccnt` directory and type `make`. If all goes well it
    should produce some files, including the module itself which will
    be called `cyccnt.ko`.

  - Loading the module is done with `insmod cyccnt.ko`. This is where
    root is needed, so presumably type `sudo insmod cyccnt.ko`.

Once the module is loaded, you can see in the kernel message (`dmesg`
command) something like:

```
[  901.256454] enable pmccntr_el0 on CPU 0
[  901.256454] enable pmccntr_el0 on CPU 1
[  901.256454] enable pmccntr_el0 on CPU 2
[  901.256454] enable pmccntr_el0 on CPU 3
```

which means that the module was indeed loaded and enabled the cycle
counter on all four CPU cores. The access remains allowed until the
module is unloaded (with `rmmod`), or the next reboot.

**Idle states:** on my Raspberry Pi 5, running Ubuntu 24.04, this
is sufficient. On another system, an ODROID C4 running an older kernel
(version "4.9.337-13"), the CPU cores happen to "forget" their setting
whenever they go idle, which is common. On that system, the CPU idle
state must be disabled on each core (the sequence is: disable idle
state, *then* load the kernel module):

```
echo 1 > /sys/devices/system/cpu/cpu0/cpuidle/state1/disable
echo 1 > /sys/devices/system/cpu/cpu1/cpuidle/state1/disable
echo 1 > /sys/devices/system/cpu/cpu2/cpuidle/state1/disable
echo 1 > /sys/devices/system/cpu/cpu3/cpuidle/state1/disable
```

I don't know if that is a quirk of that particular hardware, or of the
older kernel version.

# RISC-V

On a 64-bit RISC-V system, the cycle counter is read with the `rdcycle`
instruction:

```c
static inline uint64_t
core_cycles(void)
{
        uint64_t x;
        __asm__ __volatile__ ("rdcycle %0" : "=r" (x));
        return x;
}
```

Note that there is no memory fence here; the [RISC-V instruction set
manuals](https://lf-riscv.atlassian.net/wiki/spaces/HOME/pages/16154769/RISC-V+Technical+Specifications)
sort of explain that the CPU is supposed to enforce proper synchronization
of these instructions with regard to the sequence of instructions that the
core executes, so that no memory fence should be needed on cores with
out-of-order execution. I am not entirely sure that I read it correctly
and I do not have a RISC-V with out-of-order execution to test it.

For accessing the cycle counter, the situation is similar to that of
ARMv8: the `cyccnt` kernel module should be used. The process actually
needs cooperation of both the supervisor (kernel) mode, and the machine
level (the hypervisor). The kernel can ask the hypervisor to enable some
performance counters through an API called
[SBI](https://lists.riscv.org/g/tech-brs/attachment/361/0/riscv-sbi.pdf).
In `cyccnt` this is done with the `sbi_ecall()` function and it has been
tested on a grand total of one (1) test board (a [StarFive VisionFive
2](https://www.starfivetech.com/en/site/boards) using Ubuntu 24.04).

There again, the access remains until the module is unloaded or the
system is rebooted.
