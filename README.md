# OS-Jackfruit

Lightweight multi-container runtime in C with a long-running supervisor, bounded-buffer log pipeline, and kernel-space soft/hard memory enforcement.

## 1. Team Information

- Name: Nishitha Sathya
- SRN: PES1UG24AM180
- Name: Nayana S M
- SRN: PES1UG24AM174
## 2. Repository Contents

- `engine.c`: root-level wrapper for the user-space runtime implementation in `boilerplate/engine.c`
- `monitor.c`: root-level wrapper for the kernel module implementation in `boilerplate/monitor.c`
- `monitor_ioctl.h`: shared ioctl definitions
- `memory_hog.c`: memory pressure workload
- `cpu_hog.c`: CPU-bound workload
- `io_pulse.c`: I/O-oriented workload
- `Makefile`: top-level build entrypoint for grading
- `boilerplate/`: inherited CI-safe path kept intact for `make -C boilerplate ci`

## 3. Build, Load, and Run Instructions

This project is intended for an Ubuntu 22.04/24.04 VM with Secure Boot disabled.

### 3.1 Install Dependencies

```bash
sudo apt update
sudo apt install -y build-essential linux-headers-$(uname -r) wget
```

### 3.2 Build the Project

```bash
make
```

For the inherited smoke-check path:

```bash
make -C boilerplate ci
```

### 3.3 Prepare the Alpine Root Filesystem

```bash
mkdir -p rootfs-base
wget https://dl-cdn.alpinelinux.org/alpine/v3.20/releases/x86_64/alpine-minirootfs-3.20.3-x86_64.tar.gz
tar -xzf alpine-minirootfs-3.20.3-x86_64.tar.gz -C rootfs-base
```

Copy the workload binaries into the base rootfs before making per-container writable copies:

```bash
cp cpu_hog io_pulse memory_hog rootfs-base/
```

Create one writable rootfs per container:

```bash
cp -a rootfs-base rootfs-alpha
cp -a rootfs-base rootfs-beta
cp -a rootfs-base rootfs-gamma
```

### 3.4 Load the Kernel Module

```bash
sudo insmod monitor.ko
ls -l /dev/container_monitor
```

### 3.5 Start the Supervisor

```bash
sudo ./engine supervisor ./rootfs-base
```

The supervisor creates a UNIX domain control socket at `/tmp/mini_runtime.sock` and stores persistent logs in `./logs/`.

### 3.6 Launch Containers

Background container:

```bash
sudo ./engine start alpha ./rootfs-alpha /bin/sh --soft-mib 48 --hard-mib 80 --nice 0
```

Foreground container:

```bash
sudo ./engine run beta ./rootfs-beta "/cpu_hog 10" --soft-mib 40 --hard-mib 64 --nice 5
```

List tracked metadata:

```bash
sudo ./engine ps
```

View a container log:

```bash
sudo ./engine logs alpha
```

Stop a running container:

```bash
sudo ./engine stop alpha
```

### 3.7 Memory Limit Demonstration

Run a memory-intensive workload with a low soft and hard limit:

```bash
sudo ./engine start mem1 ./rootfs-gamma "/memory_hog 8 500" --soft-mib 24 --hard-mib 40
```

Watch kernel messages:

```bash
dmesg | tail -n 50
```

Expected behavior:

- The first soft-limit crossing emits a warning in `dmesg`
- Exceeding the hard limit triggers `SIGKILL`
- `./engine ps` should later show the container as `killed`

### 3.8 Scheduler Experiment Commands

Experiment A: two CPU-bound workloads with different `nice` values

```bash
cp -a rootfs-base rootfs-cpu-a
cp -a rootfs-base rootfs-cpu-b

sudo ./engine start cpua ./rootfs-cpu-a "/cpu_hog 15" --nice -5
sudo ./engine start cpub ./rootfs-cpu-b "/cpu_hog 15" --nice 10
sudo ./engine ps
sudo ./engine logs cpua
sudo ./engine logs cpub
```

Experiment B: CPU-bound vs I/O-oriented workload

```bash
cp -a rootfs-base rootfs-cpu
cp -a rootfs-base rootfs-io

sudo ./engine start cpu1 ./rootfs-cpu "/cpu_hog 15" --nice 0
sudo ./engine start io1 ./rootfs-io "/io_pulse 25 150" --nice 0
sudo ./engine logs cpu1
sudo ./engine logs io1
```

### 3.9 Cleanup

```bash
sudo ./engine stop alpha
sudo ./engine stop beta
sudo ./engine stop mem1
sudo rmmod monitor
rm -f /tmp/mini_runtime.sock
```

If the supervisor is still running, stop it with `Ctrl+C` or `SIGTERM`. It will mark active containers for shutdown, reap children, and drain the logger before exiting.

## 4. Implementation Overview

### 4.1 Supervisor and Control Plane

- The supervisor is a long-running process started with `./engine supervisor <base-rootfs>`
- CLI clients connect to the supervisor over a UNIX domain socket at `/tmp/mini_runtime.sock`
- Each accepted client connection is handled in its own thread so a blocking `run` request does not prevent concurrent `ps`, `logs`, or `stop` commands
- Metadata is stored in an in-memory linked list protected by `metadata_lock`

### 4.2 Container Lifecycle

- Containers are started with `clone()` on Linux using `CLONE_NEWPID`, `CLONE_NEWUTS`, and `CLONE_NEWNS`
- The child process sets a container hostname, changes root into the provided writable rootfs, mounts `/proc`, applies the requested `nice` value, and executes `/bin/sh -c <command>`
- Each live container must have a unique writable rootfs path
- The supervisor reaps children with `waitpid(..., WNOHANG)` when `SIGCHLD` is observed

### 4.3 Logging Pipeline

- Each container's `stdout` and `stderr` are redirected to a pipe owned by the supervisor
- A producer thread per container reads pipe output and pushes fixed-size chunks into a bounded shared buffer
- A dedicated consumer thread drains the buffer and appends data to `logs/<container-id>.log`
- Shutdown logic wakes both producers and consumers and ensures buffered data is drained before exit

### 4.4 Kernel Monitor

- The supervisor registers the host PID, container ID, soft limit, and hard limit through `ioctl`
- The kernel module tracks entries in a linked list protected by a spinlock
- A periodic timer checks resident memory usage using RSS
- Soft-limit behavior logs one warning event per container
- Hard-limit behavior sends `SIGKILL` and removes the entry from the monitored list

## 5. Demo with Screenshots

The assignment requires annotated screenshots. Capture these on the Ubuntu VM after running the commands above and place them in `docs/screenshots/`.

| # | Required Screenshot | What to Capture |
| --- | --- | --- |
| 1 | Multi-container supervision | Supervisor terminal plus at least two running containers |
| 2 | Metadata tracking | `sudo ./engine ps` output showing IDs, PIDs, states, limits, and rootfs paths |
| 3 | Bounded-buffer logging | `logs/<id>.log` content plus active producer/consumer behavior |
| 4 | CLI and IPC | A client command and supervisor response using the control socket |
| 5 | Soft-limit warning | `dmesg` output showing the soft-limit warning |
| 6 | Hard-limit enforcement | `dmesg` hard-limit kill plus `./engine ps` showing `killed` |
| 7 | Scheduling experiment | Measured output from at least one scheduling comparison |
| 8 | Clean teardown | Evidence of child reaping and no zombie processes after shutdown |

Suggested filenames:

- `docs/screenshots/01-multi-container.png`
- `docs/screenshots/02-ps-metadata.png`
- `docs/screenshots/03-logging.png`
- `docs/screenshots/04-cli-ipc.png`
- `docs/screenshots/05-soft-limit.png`
- `docs/screenshots/06-hard-limit.png`
- `docs/screenshots/07-scheduler.png`
- `docs/screenshots/08-clean-teardown.png`

## 6. Engineering Analysis

### 6.1 Isolation Mechanisms

The runtime isolates processes with PID, UTS, and mount namespaces. PID namespaces give each container its own process numbering view, so PID 1 inside a container is not PID 1 on the host. UTS namespaces isolate the hostname, which makes it easy to label each container distinctly. Mount namespaces let the child process build a filesystem view that is independent of the host's mount table. The `chroot()` step changes the visible root directory to the container's writable rootfs copy, while the `/proc` mount exposes process information that is consistent with the container's namespace view.

Containers still share the same host kernel. This means they do not have separate kernels, schedulers, or physical memory managers. Namespace isolation narrows visibility and resource interaction, but system calls still enter the same kernel and compete for the same CPUs and memory.

### 6.2 Supervisor and Process Lifecycle

A long-running supervisor is useful because container execution is not a single fire-and-forget action. The parent must create children, remember metadata, receive log output, handle `stop` requests, and reap exited processes so zombies do not accumulate. Keeping that coordination in one persistent parent makes signal routing and lifecycle accounting much simpler than trying to reconstruct state from short-lived CLI invocations.

The supervisor also acts as the single source of truth for container state. A client can ask for `ps`, `logs`, or `stop`, but the authoritative lifecycle state lives in the parent because it is the process that owns the children and receives `SIGCHLD`.

### 6.3 IPC, Threads, and Synchronization

The project uses two different IPC paths. The control plane uses a UNIX domain socket because it supports request/response semantics cleanly and lets multiple CLI clients talk to one supervisor. The logging plane uses file-descriptor pipes because container `stdout` and `stderr` naturally map onto byte streams.

Without synchronization, the container metadata list could be corrupted by concurrent `start`, `stop`, `ps`, and reap operations. A `pthread_mutex_t` protects that linked list and a condition variable lets `run` requests sleep until the corresponding container reaches a terminal state.

The logging buffer is a producer-consumer queue with a fixed capacity. A mutex plus `not_full` and `not_empty` condition variables prevent races around the queue indices and count. The bounded design intentionally applies backpressure when the buffer is full instead of silently dropping data. During shutdown, a `shutting_down` flag wakes blocked threads so they can exit cleanly after the remaining log chunks are drained.

### 6.4 Memory Management and Enforcement

RSS measures resident set size: the portion of a process's address space currently backed by physical memory. It captures how much RAM a process is actively occupying, but it does not directly describe swapped-out pages, total virtual address space reservations, or every kernel-side memory cost associated with the process.

Soft and hard limits are different because they serve different policy goals. A soft limit is advisory and useful for visibility: it tells the operator that a workload is beginning to exceed expectations without immediately destroying useful work. A hard limit is an enforcement threshold intended to protect the rest of the system from one container consuming too much memory.

The enforcement mechanism belongs in kernel space because the kernel owns authoritative memory accounting and signal delivery for processes. A pure user-space monitor can be delayed, can miss fast spikes, and cannot see process memory state as directly as the kernel can.

### 6.5 Scheduling Behavior

The scheduler experiments are meant to show how Linux balances fairness, throughput, and responsiveness across different workloads. A CPU-bound workload competes continuously for timeslices, so `nice` differences should visibly affect its progress and completion time. An I/O-oriented workload frequently sleeps, so it tends to regain CPU quickly when it wakes up, which often makes it feel more responsive even when running next to CPU-heavy work.

By running these workloads inside the runtime, the project turns the container system into an experimental platform. The interesting result is not just which job finishes first, but how scheduler policy affects observed progress, interactivity, and CPU distribution under contention.

## 7. Design Decisions and Tradeoffs

### 7.1 Namespace Isolation

- Choice: `clone()` with PID, UTS, and mount namespaces plus `chroot()`
- Tradeoff: `chroot()` is simpler to implement than `pivot_root()`, but it provides less thorough containment against escape patterns if the environment is misconfigured
- Justification: it matches the course scope while still demonstrating real namespace and filesystem isolation

### 7.2 Supervisor Architecture

- Choice: one long-running daemon with per-client request threads
- Tradeoff: threaded request handling is more complex than a single blocking loop
- Justification: it allows `run` to wait for a specific container without freezing the entire control plane

### 7.3 IPC and Logging

- Choice: UNIX socket for commands and pipes for container output
- Tradeoff: two IPC paths mean more code and more cleanup cases
- Justification: each mechanism matches its traffic pattern well, keeping commands structured and logs streaming-oriented

### 7.4 Kernel Monitor

- Choice: kernel linked list plus periodic timer checks and spinlock protection
- Tradeoff: timer-based polling is simpler than event-driven accounting, but it only enforces at the polling interval
- Justification: it is straightforward, auditable, and sufficient for demonstrating soft-limit warnings and hard kills

### 7.5 Scheduling Experiments

- Choice: bundled `cpu_hog`, `io_pulse`, and `memory_hog` workloads
- Tradeoff: synthetic workloads are less realistic than large applications
- Justification: they make scheduler and memory behavior repeatable and easy to explain in a report

## 8. Scheduler Experiment Results

Record the actual values from your Ubuntu VM here after running the experiments.

### 8.1 Experiment A: CPU vs CPU with Different Priorities

| Container | Workload | Nice | Observed completion/progress |
| --- | --- | --- | --- |
| `cpua` | `/cpu_hog 15` | `-5` | Fill from VM run |
| `cpub` | `/cpu_hog 15` | `10` | Fill from VM run |

Expected interpretation:

- The lower `nice` value should receive a larger CPU share
- Its progress lines should appear more frequently or it should finish sooner under contention

### 8.2 Experiment B: CPU vs I/O

| Container | Workload | Nice | Observed behavior |
| --- | --- | --- | --- |
| `cpu1` | `/cpu_hog 15` | `0` | Fill from VM run |
| `io1` | `/io_pulse 25 150` | `0` | Fill from VM run |

Expected interpretation:

- The I/O-oriented workload should remain responsive because it regularly sleeps and re-enters the run queue
- The CPU-bound task should dominate sustained CPU consumption, but not prevent the sleeping task from making progress

## 9. Verification Notes

Completed locally in this workspace:

- `make ci`
- `./engine` prints usage and exits non-zero

Still required on the Ubuntu VM before final submission:

- `make` including kernel module build
- `sudo insmod monitor.ko`
- end-to-end container runs using Alpine rootfs copies
- screenshot capture
- scheduler result measurement table completion
