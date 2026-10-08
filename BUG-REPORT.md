# [SteamOS 3.9 / Neptune 7.2] Reproducible NULL dereference in downstream `dm_freesync_mccs_ddc_worker` on DP-to-HDMI PCON

> This is the full root-cause write-up. A shorter version is filed with Valve
> as [ValveSoftware/SteamOS#2786](https://github.com/ValveSoftware/SteamOS/issues/2786).

## Summary: reproducible fault in downstream amdgpu code

Every docked boot on this Legion Go S produces a kernel NULL dereference in
Valve's downstream `dm_freesync_mccs_ddc_worker()` approximately one second
after the HP G6 dock/display path is enumerated.

```text
BUG: kernel NULL pointer dereference, address: 0000000000000000
Workqueue: events dm_freesync_mccs_ddc_worker [amdgpu]
RIP: 0010:i2c_transfer+0x79/0x150
 dm_freesync_mccs_ddc_worker+0x16f/0x200 [amdgpu]
 process_one_work+0x19f/0x370
 worker_thread+0x1b1/0x310
...
RAX: 0000000000000000
note: kworker/2:1[132] exited with irqs disabled
```

This is not a speculative diagnosis. The faulting instruction sequence is:

```text
48 8b 43 20          mov 0x20(%rbx),%rax
48 8b 00             mov (%rax),%rax       # fault; RAX == 0
```

`RBX` is the `struct i2c_adapter *`. On this x86-64 build, offset `0x20` in
`struct i2c_adapter` is `lock_ops`. Therefore the immediate NULL dereference is
`adap->lock_ops`, in the bus-lock helper called at the start of
`i2c_transfer()`. `adap->algo` also appears to be uninitialized, but it is not
the member dereferenced by this particular instruction.

The downstream worker selects the embedded DP AUX adapter without checking
that it was registered:

```c
if (aconn->dc_link->aux_mode)
        adap = &aconn->dm_dp_aux.aux.ddc;
...
ret = i2c_transfer(adap, msgs, 1);
```

Runtime sysfs agrees with that condition:

```text
/sys/class/drm/card0-DP-11/ddc    absent
/sys/class/drm/card0-eDP-1/ddc    -> .../i2c-5
```

Relevant source in the exact `7.2.0-valve1` integration tree:

- [`amdgpu_dm.c:169-258` — downstream deferred MCCS worker and capability-unconditional `schedule_work()` once VRR info changes](https://gitlab.steamos.cloud/jupiter/linux-integration/-/blob/d39b4282853d0af58cf47fcde20ac6ff126a3f8a/drivers/gpu/drm/amd/display/amdgpu_dm/amdgpu_dm.c#L169-258)
- [`amdgpu_dm.c:10189-10205` — schedules the poke for every changed VRR info packet](https://gitlab.steamos.cloud/jupiter/linux-integration/-/blob/d39b4282853d0af58cf47fcde20ac6ff126a3f8a/drivers/gpu/drm/amd/display/amdgpu_dm/amdgpu_dm.c#L10189-10205)
- [`include/linux/i2c.h:733-768` — `struct i2c_adapter`, including `lock_ops`](https://gitlab.steamos.cloud/jupiter/linux-integration/-/blob/d39b4282853d0af58cf47fcde20ac6ff126a3f8a/include/linux/i2c.h#L733-768)
- [`drivers/i2c/i2c-core-base.c:2319-2348` — `i2c_transfer()` locks the adapter before transfer](https://gitlab.steamos.cloud/jupiter/linux-integration/-/blob/d39b4282853d0af58cf47fcde20ac6ff126a3f8a/drivers/i2c/i2c-core-base.c#L2319-2348)

Valve's own `steamos-log-submitter` has queued, and still holds,
17 `kdumpst-*.zip` archives in `/var/lib/steamos-log-submitter/pending/kdump`
(16 dated 2026-09-05, one dated 2026-09-06), plus one `gamescope-xwm`
minidump in `pending/minidump`. The current boot's oops is additional and
unarchived, because `panic_on_oops=0` suppresses the kdump capture path.
The 16 same-day captures were produced between 09:30 and 13:55 on 2026-09-05,
while the machine was reboot-looping under Valve's default panic-on-oops.

## Hardware and software

```text
Device:       Lenovo Legion Go S (83N6/LNVNB161216)
SteamOS:      3.9.0
Kernel:       7.2.0-valve1-1-neptune-72-gd39b4282853d
GPU driver:   amdgpu

Display path:
USB-C DP Alt Mode
  -> HP USB-C Dock G6
  -> active DisplayPort-to-HDMI PCON
  -> HDMI 1
  -> TCL C6K television

DRM connector: card0-DP-11
Active mode:   3840x2160 at 120.00 Hz, 1,188,000 kHz pixel clock
```

The active mode above was read from the CRTC with `modetest -M amdgpu -p`, not
inferred from connector preferred mode number 0.

## Reproduction

1. Boot the Legion Go S with the HP G6 dock/display chain connected, or connect
   it after boot.
2. Wait for dock and DP/HDMI enumeration.
3. Inspect `journalctl -k -b` or the resulting pstore/kdump capture.

Representative timing:

```text
[   28.851] usb ... Product: HP USB-C 100W G6 Dock
[   29.730] BUG: kernel NULL pointer dereference
```

On the most recently inspected boot, the oops was at T+26.138 seconds on CPU 2
in `kworker/2:1`, PID 132. The display nevertheless remained active at 4K120.

Expected: the driver should skip MCCS/DDC communication when the DP AUX I2C
adapter was not registered.

Actual: the worker passes the incomplete embedded adapter to `i2c_transfer()`,
which dereferences its NULL `lock_ops` pointer and kills the kworker.

## Suggested fix

The safest fix appears to be reverting/removing this extra deferred DDC poke.
The operation is not required for mode generation or link training, and the
integration tree already contains the capability-gated MCCS implementation in
[`amdgpu_dm_helpers.c:1693-1826`](https://gitlab.steamos.cloud/jupiter/linux-integration/-/blob/d39b4282853d0af58cf47fcde20ac6ff126a3f8a/drivers/gpu/drm/amd/display/amdgpu_dm/amdgpu_dm_helpers.c#L1693-1826).

If the worker must remain, it should at minimum:

1. verify that the selected adapter is actually registered and that both
   `lock_ops` and `algo` are valid before calling the I2C core;
2. restrict the operation to the sink/PCON combinations for which MCCS was
   positively detected; and
3. hold an appropriate connector/link lifetime reference until the deferred
   work completes.

The last point is a separate latent issue found by source inspection:
`dm_freesync_mccs_ddc_work` appears to store a borrowed `aconn` pointer and
queues asynchronous work without taking a connector reference. A disconnect
between queueing and execution therefore appears capable of turning this into
a use-after-free even after the NULL-adapter bug is fixed.

## Secondary impact: inference, not direct proof

The following describes a likely consequence of killing the kworker. It is
clearly secondary to the directly proven NULL dereference above.

The oops occurs after `process_one_work()` has inserted the worker in the
physical pool's busy hash and recorded the work as in flight, but before its
normal epilogue removes the busy entry, clears `current_work/current_pwq`, and
decrements the in-flight count. `make_task_dead()` exits the kthread without
returning through `worker_thread()`'s normal `WORKER_DIE` path. The affected
per-CPU physical worker pool can consequently retain stale concurrency state.

This does **not** imply that every workqueue or every SRCU domain is globally
corrupt. The observed cascade is better explained by CPU placement:

- Tree SRCU grace-period work uses a per-CPU `rcu_gp` workqueue which shares
  the CPU's normal physical worker pool.
- On the current boot the amdgpu worker oopsed on CPU 2. The first stuck
  `events_unbound` fsnotify worker also last ran on CPU 2 before blocking in
  `synchronize_srcu(&fsnotify_mark_srcu)`.
- `khugepaged` blocked in `lru_add_drain_all()`, which queues and flushes drain
  work on every relevant CPU; one unusable per-CPU pool is sufficient.
- Later fsnotify and HID teardown callers can then block behind those specific
  SRCU/work items.

This CPU-locality correlation is strong evidence for the mechanism, but it is
not presented as proof of every internal pool field without a vmcore.

Code paths underlying that inference in the same exact tree are
[`process_one_work()` at `kernel/workqueue.c:3220-3380`](https://gitlab.steamos.cloud/jupiter/linux-integration/-/blob/d39b4282853d0af58cf47fcde20ac6ff126a3f8a/kernel/workqueue.c#L3220-3380),
the normal [`WORKER_DIE` exit at `kernel/workqueue.c:3431-3452`](https://gitlab.steamos.cloud/jupiter/linux-integration/-/blob/d39b4282853d0af58cf47fcde20ac6ff126a3f8a/kernel/workqueue.c#L3431-3452),
and the oops path through
[`oops_end()` at `arch/x86/kernel/dumpstack.c:375-408`](https://gitlab.steamos.cloud/jupiter/linux-integration/-/blob/d39b4282853d0af58cf47fcde20ac6ff126a3f8a/arch/x86/kernel/dumpstack.c#L375-408)
to [`make_task_dead()` at `kernel/exit.c:1056`](https://gitlab.steamos.cloud/jupiter/linux-integration/-/blob/d39b4282853d0af58cf47fcde20ac6ff126a3f8a/kernel/exit.c#L1056).

Severity varies with which CPU executes each FreeSync transition and which CPU
later starts a required SRCU grace period. One archived uninterrupted boot
recorded this same oops on CPUs 2, 2, 6, 7, 12, 1, 2 and 9 over time, several
immediately after resume. That provides a plausible CPU-placement/severity
lottery rather than a global workqueue failure.

The absence of automatic hung-task warnings after the first oops is expected:
this kernel's hung-task detector returns without checking after `TAINT_DIE`.
The current live value was:

```text
/proc/sys/kernel/tainted = 128
```

## `panic_on_oops` disclosure

This machine currently has:

```text
kernel.panic_on_oops = 0
```

That is a deliberate, non-default diagnostic setting used to keep the machine
alive long enough to collect post-oops evidence. It does not cause the fault.
With Valve's default panic-on-oops behavior, the machine reboot-loops while
docked; 16 kdump archives were produced between 09:30 and 13:55 on
2026-09-05.

## Available evidence

The following can be attached or uploaded on request:

- 17 `steamos-log-submitter` kdump zip archives and one `gamescope-xwm`
  minidump, plus the current live-oops journal;
- complete oops including registers and instruction bytes;
- SysRq blocked-task and all-task/workqueue dumps;
- exact EDID binary and decoded EDID;
- `drm_info`, `modetest -p`, connector sysfs and `/proc/config.gz`;
- boot/wtmp timeline showing both fast cascades and long-running sessions;
- a minimal out-of-tree kprobe mitigation which redirects only the faulty
  worker and demonstrates that 4K120 remains operational without the DDC poke.
