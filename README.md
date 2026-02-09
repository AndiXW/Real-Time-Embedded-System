## Real-Time Embedded Linux Scheduling Framework on Raspberry Pi Project
## Course: Theory of Real-Time Systems, Professor: Hyunjong Choi

![Platform](https://img.shields.io/badge/platform-Raspberry%20Pi%203%20(Model%20B)-informational)
![Domain](https://img.shields.io/badge/domain-Real--Time%20Systems-blue)
![Language](https://img.shields.io/badge/language-C%20%2F%20Kernel%20C-orange)
![Build](https://img.shields.io/badge/build-make-success)

This repository contains my complete work for **CS596: Theory of Real-Time Systems (Fall 2025)** at **San Diego State University**, focused on **Linux kernel programming for real-time mechanisms** on **Raspberry Pi 3**: toolchain setup, syscalls, kernel modules, resource reservation, real-time scheduling (RM + EDF), and end-to-end latency measurement for task chains.

> **Course theme:** from “kernel bring-up” → to “real-time reservations” → to “scheduler instrumentation” → to “multi-core EDF + chain latency.”

---

## Table of Contents
- [Repo Layout](#repo-layout)
- [Environment](#environment)
- [Quick Start](#quick-start)
- [Project 1 — Kernel Build & Versioning](#project-1--kernel-build--versioning)
- [Project 2 — Syscalls & Kernel Modules](#project-2--syscalls--kernel-modules)
- [Project 3 — Reservation Framework (RM) + Monitoring](#project-3--reservation-framework-rm--monitoring)
- [Project 4 — Partitioned EDF + End-to-End Latency of Chains](#project-4--partitioned-edf--end-to-end-latency-of-chains)
- [Debugging & Verification](#debugging--verification)
- [Notes](#notes)
- [References](#references)

---

## Repo Layout

Typical structure used across projects (may vary slightly by group/repo):
```
.
├─ proj1/
├─ proj2/
│  ├─ kernel/
│  ├─ modules/
│  └─ apps/
├─ proj3/
│  ├─ kernel/
│  ├─ modules/
│  └─ apps/
└─ proj4/
   ├─ kernel/
   └─ apps/
```

Each project follows the course build conventions:
- **Built-in kernel code**: compiled into the kernel image
- **Kernel modules**: built with kernel build system (Kbuild + Makefile)
- **User apps**: built with gcc / cross-compiler + Makefile  

---

## Environment

- **Target:** Raspberry Pi 3 Model B (2 cores used in Project 4)
- **Host:** x86 Ubuntu VM / native Ubuntu
- **Cross compile:** `arm-linux-gnueabihf-` toolchain on host (Project 1 setup)
- **Kernel:** course-provided Raspberry Pi kernel tree, modified per projects

---

## Quick Start

### 1) Build the kernel (host)
From the kernel repo root:
```bash
export KERNEL=kernel7
make ARCH=arm CROSS_COMPILE=arm-linux-gnueabihf- zImage modules dtbs -j"$(nproc)"
```

### 2) Install to SD card (host)
Mount your boot + root partitions (example uses `/dev/sdb1` and `/dev/sdb2`), then:
```bash
sudo env PATH=$PATH make ARCH=arm CROSS_COMPILE=arm-linux-gnueabihf-   INSTALL_MOD_PATH=mnt/root modules_install -j"$(nproc)"

sudo cp mnt/boot/$KERNEL.img mnt/boot/$KERNEL-backup.img
sudo cp arch/arm/boot/zImage mnt/boot/$KERNEL.img
sudo cp arch/arm/boot/dts/*.dtb mnt/boot/
sudo cp arch/arm/boot/dts/overlays/*.dtb* mnt/boot/overlays/
sudo cp arch/arm/boot/dts/overlays/README mnt/boot/overlays/

sudo umount mnt/boot mnt/root
```

### 3) Run tests on Raspberry Pi
```bash
uname -a
dmesg -w
```

---

## Project 1 — Kernel Build & Versioning

**Goal:** setup a reliable kernel build + boot pipeline and rename kernel version string.

### Key deliverable
- Update kernel version so `uname -a` shows:
  - `... CS596-RT-GroupXX ...` (Group number embedded)

### What this project proves
- Cross-compilation works
- SD card flashing + module install works
- Kernel boots reliably after modifications

---

## Project 2 — Syscalls & Kernel Modules

**Goal:** learn Linux kernel programming fundamentals:
- user-space app
- loadable kernel module (LKM)
- new syscall + runtime syscall behavior modification

### Implemented components
1) **User-space “Hello World”** app  
Prints: `Hello world! GroupXX in user space`

2) **Kernel module “Hello World”**  
On `insmod`, logs: `Hello world! GroupXX in kernel space` (check with `dmesg`)

3) **Calculator syscall (`sys_calc`) — syscall #397**  
Signature:
```c
sys_calc(int param1, int param2, char operation, int* result)
```
- supports `+ - * /`
- returns `-1` on invalid inputs / NaN conditions

4) **User app test harness** (`test_calc`)  
Calls the syscall and prints integer result or `NaN`.

5) **Module that hooks/modifies syscall at runtime** (`mod_calc`)  
When loaded, forces calculator behavior to **modulo** regardless of requested op; restores normal behavior on unload.

---

## Project 3 — Reservation Framework (RM) + Monitoring

**Goal:** implement a kernel-level **resource reservation framework** using the periodic task model `(C, T)` and enforce budget with monitoring.

### Main features
1) **CPU-independent dummy task calibration**
- `dummy_load(execution_time_ms)` calibrated so runtime error is within ±0.5 ms using `gettimeofday()`

2) **Reservation management syscalls**
- `set_rsv(pid, C, T)` — syscall **#397**
- `cancel_rsv(pid)` — syscall **#398**  
Includes validation, error handling, and cleanup.

3) **RT priority assignment (Rate Monotonic)**
- Assign **SCHED_FIFO** priority based on period: shorter `T` ⇒ higher priority  
- Recompute priorities when reservations are added (and/or canceled)

4) **Periodic task support**
- `wait_until_next_period()` — syscall **#399**
- Uses **hrtimer** to wake tasks precisely at the next period boundary

5) **Computation time tracking + budget enforcement**
- Add custom accumulator field to `task_struct`
- Update on context switch in/out (scheduler hook)
- Reset each period in timer callback
- Log overruns to kernel log:
  ```
  Task xxxx: budget overrun (util: xx %)
  ```

6) **(Bonus) SIGUSR1 overrun notification**
- Signal task when it exceeds budget (checked at end of period)

---

## Project 4 — Partitioned EDF + End-to-End Latency of Chains

**Goal:** move from single-core RM concepts to **multi-core partitioned EDF**, plus **kernel-level end-to-end (E2E) latency measurement** for task chains.

### 1) Partitioned EDF scheduler (multi-processor)
New/modified syscall:
```c
int set_edf_task(pid_t pid,
                 struct timespec *C,
                 struct timespec *T,
                 struct timespec *D,
                 int cpu_id,
                 int chain_id,
                 int chain_pos);
```
- syscall **#397**
- registers a task as EDF task
- sets **SCHED_FIFO** and updates priority via EDF rules
- **does NOT** use `SCHED_DEADLINE`

### 2) User-space loader + CPU partitioning
- Reads provided `taskset.txt` with rows:
  ```
  < C, T, D, chain_id, chain_pos >
  ```
- Implements a bin-packing partition strategy (e.g., FFD/WFD/BFD) so per-core utilization ≤ 1.0
- Pins tasks to **CPU0/CPU1**, prints assignment, runs for ≥ 2 minutes, then cancels all tasks

### 3) End-to-End latency measurement for chains
- Measures latency from release of first task in chain → completion of last task
- After `cancel_rsv`, prints per-chain stats:
  - max / min / average latency (ms)

### 4) E2E syscall
```c
int get_e2e_latency(int chain_id, struct timespec *latency_buf);
```
- syscall **#400**
- returns the most recent E2E latency for the chain via `copy_to_user`

---

## Debugging & Verification

Recommended tools/commands used across projects:
- Kernel logs:
  ```bash
  dmesg -w
  ```
- Check RT scheduling/priority:
  ```bash
  chrt -p <pid>
  ```
- CPU pinning:
  ```bash
  taskset -p <mask> <pid>
  ```

---

## Notes

- All kernel changes are designed to be **stable** (no crashes/freezes), per course requirement.
- System call numbers used in these projects:
  - #397: `sys_calc` (Proj2) / `set_rsv` (Proj3) / `set_edf_task` (Proj4)
  - #398: `cancel_rsv`
  - #399: `wait_until_next_period`
  - #400: `get_e2e_latency`  
(Each project uses its own kernel build/branch context as specified by the course.)

---

## References

- Project 1 handout: environment setup + kernel build/boot workflow  
- Project 2 handout: LKMs + syscall creation + Makefile/Kbuild structure  
- Project 3 handout: reservations, hrtimers, RM priorities, budget enforcement  
- Project 4 handout: partitioned EDF, loader partitioning, E2E chain latency  

---

### Team Member
- Anh Huy Nguyen
- Thy Nguyen
>>>>>>> 58c583c3d8675e7305b2938996412c620e3d23c1
