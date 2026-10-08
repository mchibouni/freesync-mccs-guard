# FreeSync MCCS kprobe guard

This is a temporary mitigation for
`dm_freesync_mccs_ddc_worker()` in
`7.2.0-valve1-1-neptune-72-gd39b4282853d`. It redirects that callback to a
replacement which only frees the callback's allocation and returns. The DDC
transaction is skipped, but `process_one_work()` regains control normally and
performs all workqueue cleanup and accounting.

Do not test this on a boot that has already oopsed. The guard prevents the
fault; it cannot repair a worker pool already damaged by one. Start from a
fresh, undocked boot.

## What was verified

On the live Legion Go S:

- The running kernel is
  `7.2.0-valve1-1-neptune-72-gd39b4282853d`.
- `CONFIG_X86_64`, `CONFIG_X86_KERNEL_IBT`, `CONFIG_KPROBES`,
  `CONFIG_KPROBES_ON_FTRACE`, `CONFIG_DYNAMIC_FTRACE_WITH_REGS`,
  `CONFIG_KPROBE_EVENTS`, `CONFIG_FUNCTION_TRACER`, and `CONFIG_OPTPROBES` are
  enabled.
- `CONFIG_LIVEPATCH` is disabled. This is not a livepatch/kpatch module.
- `amdgpu` is a loadable module, not built in. On the inspected boot it began
  initialization at T+7.78 seconds, completed DRM initialization at T+8.53,
  and the bad worker faulted at T+26.14 seconds.
- `lsinitcpio` did not find amdgpu in the current initramfs. The supplied
  verifier unit explicitly orders `systemd-modules-load.service` before udev
  coldplug, making that load point early enough on this image.
- `/proc/kallsyms` contains the standalone local-text symbol
  `dm_freesync_mccs_ddc_worker [amdgpu]`. Its presence there, and in the oops
  call trace, proves that it was emitted and was not inlined away.
- Kprobe symbol lookup calls `kallsyms_lookup_name()`, whose fallback searches
  module kallsyms including local symbols. The guard uses the qualified name
  `amdgpu:dm_freesync_mccs_ddc_worker`; `kernel/module/kallsyms.c` explicitly
  supports that `module:symbol` form. A successful `register_kprobe()` proves
  lookup; `parameters/armed=Y` additionally proves the probe was armed.
- The module initially registers the probe with `KPROBE_FLAG_DISABLED` and
  then calls `enable_kprobe()`. This is deliberate: in this exact tree,
  `__register_kprobe()` removes a probe after an ftrace-arm error but returns
  zero at `kernel/kprobes.c:1695-1705`. The separate enable call propagates the
  arm error, and the module also rejects a probe left disabled by a globally
  disarmed kprobe subsystem. `armed=Y` therefore means registration and arming
  both succeeded, not merely that symbol lookup succeeded.
- Module signature enforcement and Secure Boot are disabled on this machine.
  Loading this out-of-tree unsigned module will nevertheless add the ordinary
  out-of-tree/unsigned-module taint bits. It must not add `TAINT_DIE` (bit 7,
  decimal 128).

The blacklist and `available_filter_functions` files were root-only and could
not be read during preparation. The commands below explicitly test both and
abort if the target is blacklisted. Do not treat this item as verified until
that check has run.

## Why the IP redirect is safe on this kernel

The commonly shown shortcut—load the return address from `regs->sp`, increment
`regs->sp`, put that address in `regs->ip`, and return 1—would work only if the
probe is exactly before the function prologue and if every relevant return
stack invariant is understood. This module does not use it.

Instead, the pre-handler changes only the instruction pointer to
`guard_worker_replacement()`. It leaves the original call's return address on
the stack. The replacement has the same signature, frees the same allocation
as the original `out_free` path, and executes a normal return to
`process_one_work()`.

There are two important Neptune 7.2 details:

1. `arch/x86/kernel/kprobes/core.c:371-385` recognizes ENDBR and adjusts a
   symbol-entry kprobe from offset 0 to offset 4. The probe therefore does not
   overwrite or resume in the middle of ENDBR. The replacement is compiled as
   part of a kernel module with the same IBT compiler flags and is a valid
   function entry.
2. `kernel/kprobes.c:746-758` documents in code that an optimized kprobe can
   ignore pre-handler IP changes. The empty post-handler deliberately prevents
   optprobe optimization. For an ftrace-backed kprobe,
   `kernel/kprobes.c:1124-1173` chooses the ftrace operations carrying
   `FTRACE_OPS_FL_IPMODIFY` when a post-handler exists, and
   `arch/x86/kernel/kprobes/ftrace.c:40-63` preserves a nonzero pre-handler's
   modified IP.

`CONFIG_X86_USER_SHADOW_STACK` affects userspace. This kernel enables kernel
IBT, not a kernel shadow stack. In any case the replacement uses the existing
regular return address and an ordinary `ret` rather than synthesizing a return.

## Load ordering

A kprobe named by `symbol_name` cannot resolve a module-local symbol before the
target module is live. Registering while amdgpu is `MODULE_STATE_COMING` does
not solve this: `register_kprobe()` tries to acquire a target-module reference,
and `try_module_get()` rejects a module that is not yet live.

The modules-load entry invokes `modprobe freesync_mccs_guard`. Both the embedded
module metadata and the modprobe.d file declare `amdgpu` as a **pre-dependency
of the guard**. Modprobe therefore waits for amdgpu initialization to finish,
then loads and arms the guard. This direction is intentional; declaring the
guard as a pre-dependency of amdgpu would load it too soon to resolve the local
symbol.

On the inspected boot this leaves about 17.6 seconds between completed amdgpu
DRM initialization and the fault. The verifier service is ordered after
`systemd-modules-load.service` and before `systemd-udev-trigger.service`; it
tests `parameters/armed=Y` synchronously before udev coldplug can enumerate the
dock. This explicit edge is needed because the stock modules-load and udev
trigger units are both merely ordered before `sysinit.target`, not relative to
one another.

If a future SteamOS image moves amdgpu into the initramfs, amdgpu will already
be live when modules-load starts and direct probe registration should still
work. Nevertheless, re-audit the symbol and timing before docking after a
kernel change. The current initramfs was checked and does not contain amdgpu.

The soft dependency controls load order but does not pin amdgpu. The kprobe
core will kill probes in a module that is unloaded; this small guard does not
automatically re-arm across an amdgpu unload/reload. Its `armed` parameter
reads the kprobe's live `DISABLED`/`GONE` flags and will change to `N`, but a
fresh reboot is needed to restore this boot-time mitigation. Do not unload
amdgpu while using the guard. The undo sequence correctly removes the guard
first.

## Get the files onto the Legion

The scripts expect this tree at `/home/deck/kprobe`. On the Legion (Desktop
Mode terminal, or ssh as `deck`):

```bash
git clone https://github.com/mchibouni/freesync-mccs-guard.git /home/deck/kprobe
```

## Install build prerequisites

Boot the Legion undocked. If you work over ssh, use Wi-Fi or a network adapter
that is not part of the dock, since the dock is unplugged for this step.

On the Legion:

```bash
sudo steamos-readonly disable
sudo pacman -S --needed dkms linux-neptune-72-headers

test "$(uname -r)" = "7.2.0-valve1-1-neptune-72-gd39b4282853d"
test "$(pacman -Q linux-neptune-72-headers)" = \
     "linux-neptune-72-headers 7.2.0.valve1-1"
test -e "/usr/lib/modules/$(uname -r)/build/Makefile"
```

Expected header package for this kernel:

```text
linux-neptune-72-headers 7.2.0.valve1-1
```

Stop if the running kernel or build tree does not match.

## Preflight symbol, blacklist, ftrace and IBT checks

Still undocked:

```bash
target=dm_freesync_mccs_ddc_worker

grep -w "$target" /proc/kallsyms

sudo test -r /sys/kernel/debug/kprobes/blacklist || {
    echo "ABORT: cannot read the kprobe blacklist" >&2
    exit 1
}

if sudo grep -qw "$target" /sys/kernel/debug/kprobes/blacklist; then
    echo "ABORT: target is on the kprobe blacklist" >&2
    exit 1
else
    echo "PASS: target is not on the kprobe blacklist"
fi

sudo test -r /sys/kernel/tracing/available_filter_functions || {
    echo "ABORT: cannot read available_filter_functions" >&2
    exit 1
}

if sudo grep -w "$target" /sys/kernel/tracing/available_filter_functions; then
    echo "PASS: target has an ftrace entry; expect a [FTRACE] kprobe"
else
    echo "INFO: target is not ftrace-backed; the non-optimized int3 backend will be used"
fi

zgrep -E '^CONFIG_(X86_KERNEL_IBT|KPROBES_ON_FTRACE|DYNAMIC_FTRACE_WITH_REGS|OPTPROBES)=' /proc/config.gz
```

Expected symbol type is lower-case `t` with `[amdgpu]`. A zero address when run
without root is only `kptr_restrict`, not a failed lookup.

## Build without loading

```bash
cd /home/deck/kprobe
make
modinfo ./freesync_mccs_guard.ko
```

Expected results are a successful module build and `version: 1.0.0`. This step
does not load or arm anything.

## Manual undocked test

The machine must still be undocked and must not have oopsed this boot.

```bash
die_taint_before=$(cat /proc/sys/kernel/tainted)
printf 'taint before: %s\n' "$die_taint_before"
test $((die_taint_before & 128)) -eq 0

cd /home/deck/kprobe
test "$(cat /sys/module/amdgpu/initstate)" = live
sudo insmod ./freesync_mccs_guard.ko

cat /sys/module/freesync_mccs_guard/parameters/armed
cat /sys/module/freesync_mccs_guard/parameters/skipped
cat /sys/module/freesync_mccs_guard/parameters/nmissed
sudo grep -w dm_freesync_mccs_ddc_worker /sys/kernel/debug/kprobes/list
sudo journalctl -k -b --no-pager | grep -E 'freesync_mccs_guard|dm_freesync_mccs_ddc_worker'
```

Required results:

- `armed` is `Y`.
- `skipped` and `nmissed` initially read `0`.
- The kprobes list contains the exact target. It should end in `[FTRACE]` if
  the earlier ftrace check found the symbol.
- The journal contains `freesync_mccs_guard: armed` and no registration error.

Do not dock if `armed` is not `Y` or `nmissed` is nonzero.

## First docked test

Record the starting counter, then connect the already-known dock/PCON/TV chain:

```bash
before=$(cat /sys/module/freesync_mccs_guard/parameters/skipped)
printf 'skipped before docking: %s\n' "$before"
```

After docking, wait at least 45 seconds and run:

```bash
armed=$(cat /sys/module/freesync_mccs_guard/parameters/armed)
after=$(cat /sys/module/freesync_mccs_guard/parameters/skipped)
nmissed=$(cat /sys/module/freesync_mccs_guard/parameters/nmissed)
tainted=$(cat /proc/sys/kernel/tainted)

printf 'armed=%s skipped_before=%s skipped_after=%s nmissed=%s tainted=%s\n' \
       "$armed" "$before" "$after" "$nmissed" "$tainted"

test "$armed" = Y
test "$after" -gt "$before"
test "$nmissed" -eq 0
test $((tainted & 128)) -eq 0

sudo journalctl -k -b --no-pager | grep -E \
  'BUG: kernel NULL pointer|dm_freesync_mccs_ddc_worker|exited with irqs disabled' || true

ps -eLo pid,tid,psr,stat,wchan:32,comm,args | awk '$4 ~ /^D/'
modetest -M amdgpu -p | sed -n '/^CRTCs:/,/^Planes:/p' | grep '3840x2160 120.00'
```

Success means:

- `skipped` increased, proving the worker entry was intercepted.
- `nmissed` remains zero.
- `TAINT_DIE` remains clear (`tainted & 128 == 0`). Other taint bits caused by
  an unsigned out-of-tree module are expected.
- No new NULL dereference or dead kworker appears.
- There are no persistent D-state tasks in the affected paths.
- The active CRTC, not merely connector mode number 0, remains 3840x2160 at
  120.00 Hz.

## Install through DKMS and install the boot wiring

Do this only after the manual undocked/docked test succeeds. It is safe to
leave the manually loaded instance in place while installing the same module
for the next boot.

```bash
cd /home/deck/kprobe

sudo install -d -m 0755 /usr/src/freesync-mccs-guard-1.0.0
sudo install -m 0644 \
  freesync_mccs_guard.c Makefile dkms.conf INSTALL.md \
  /usr/src/freesync-mccs-guard-1.0.0/

sudo dkms add -m freesync-mccs-guard -v 1.0.0
sudo dkms build -m freesync-mccs-guard -v 1.0.0 -k "$(uname -r)"
sudo dkms install -m freesync-mccs-guard -v 1.0.0 -k "$(uname -r)"
sudo depmod -a "$(uname -r)"
dkms status -m freesync-mccs-guard -v 1.0.0
modinfo freesync_mccs_guard

sudo install -Dm0644 rootfs/etc/modules-load.d/freesync-mccs-guard.conf \
  /etc/modules-load.d/freesync-mccs-guard.conf
sudo install -Dm0644 rootfs/etc/modprobe.d/freesync-mccs-guard.conf \
  /etc/modprobe.d/freesync-mccs-guard.conf
sudo install -Dm0644 rootfs/etc/systemd/system/freesync-mccs-guard-verify.service \
  /etc/systemd/system/freesync-mccs-guard-verify.service
sudo install -Dm0644 rootfs/etc/atomic-update.conf.d/93-freesync-mccs-guard.conf \
  /etc/atomic-update.conf.d/93-freesync-mccs-guard.conf

sudo systemctl daemon-reload
sudo systemctl enable freesync-mccs-guard-verify.service
sudo steamos-readonly enable
```

Expected DKMS status contains `installed` for the exact running kernel.

For the boot-order test, undock, reboot, and inspect from the local terminal or
Wi-Fi session before reconnecting the dock:

```bash
systemctl status --no-pager freesync-mccs-guard-verify.service
cat /sys/module/freesync_mccs_guard/parameters/armed
sudo grep -w dm_freesync_mccs_ddc_worker /sys/kernel/debug/kprobes/list
sudo journalctl -k -b --no-pager | grep -E 'freesync_mccs_guard|amdgpu.*initializing kernel modesetting'
```

The service must be successful and `armed` must be `Y` before docking.

## SteamOS updates wipe the module

The supplied atomic-update drop-in preserves the `/etc` boot wiring. That part
was verified against `/usr/lib/rauc/atomic-update-keep.conf` and the existing
`99-panic-on-oops.conf` pattern.

The built module does **not** survive an OS update. SteamOS 3.x is A/B atomic:
an update writes a new rootfs and a fresh `/var` into the other slot, so
`/usr/src/freesync-mccs-guard-*`, the module under `/usr/lib/modules` and
`/var/lib/dkms` are all gone on the first boot after it. `/home` carries over,
and so do the `/etc` paths on the keep list: SteamOS's own
(`/usr/lib/rauc/atomic-update-keep.conf`, which already covers
`/etc/systemd/system/*.service` and `*.wants/**`) plus the `modules-load.d`
and `modprobe.d` files that `93-freesync-mccs-guard.conf` adds. The first docked boot after an update is
therefore unguarded and will bootloop.

### Rebuild automatically at boot

`ensure/ensure-guard.sh` runs from a systemd unit at every boot. If the module
is missing for the running kernel, it restores the source from
`/home/deck/kprobe`, installs the matching `linux-neptune-*-headers` package if
needed, and rebuilds it through DKMS. It cannot save the boot it runs on,
because amdgpu has already loaded by then, so after an update boot once
undocked and let it do its work.

It only builds for kernels listed in `verified-kernels`. The replacement
function calls `kfree(work)`, which is only correct while `struct work_struct
work` is the first member of `struct dm_freesync_mccs_ddc_work`. If Valve
reorders that struct, a rebuilt guard loads cleanly and corrupts the heap. An
unguarded bootloop is recoverable by booting undocked; heap corruption is not.
So for a kernel it has not seen, the script logs a refusal and exits.

Install it:

```bash
install -Dm0755 /home/deck/kprobe/ensure/ensure-guard.sh \
  /home/deck/.local/share/freesync-mccs-guard/ensure-guard.sh
sudo install -Dm0644 /home/deck/kprobe/ensure/freesync-mccs-guard-ensure.service \
  /etc/systemd/system/freesync-mccs-guard-ensure.service
sudo systemctl daemon-reload
sudo systemctl enable freesync-mccs-guard-ensure.service
```

### After an update

1. Boot **undocked**.
2. Check `struct dm_freesync_mccs_ddc_work` in
   `drivers/gpu/drm/amd/display/amdgpu_dm/amdgpu_dm.c` of the matching tree in
   [linux-integration](https://gitlab.steamos.cloud/jupiter/linux-integration):
   `struct work_struct work` must still be the first member. If the
   `dm_freesync_mccs_ddc_worker` symbol is gone, read the new source before
   deciding whether the guard is still needed; do not blindly rename the probe.
3. `echo "$(uname -r)" >> /home/deck/kprobe/verified-kernels`
4. `sudo /home/deck/kprobe/guard rebuild`, reboot undocked, run
   `/home/deck/kprobe/guard` and confirm `armed = Y`. Only then dock.

### Optional: stop automatic OS updates

To pick the moment an update lands instead of finding out at the next docked
boot, mask the updater:

```bash
sudo systemctl mask atomupd.service
```

`atomupd.service` is a static, D-Bus-activated unit, so `systemctl disable`
does nothing; masking also blocks the D-Bus activation the Steam client uses.
Steam client and game updates are unaffected. The cost: Gaming Mode shows an
error when it checks for system updates.

`sudo /home/deck/kprobe/check-updates` unmasks the updater for a single check,
prints the offered build and masks it again, including on Ctrl-C. A check
never installs anything. `sudo /home/deck/kprobe/check-updates apply` installs
the offered build only if it is newer than the running one. It refuses
downgrades, which a branch switch (for example preview to stable) can offer.

## Undo

Undock first. Removing the guard while the faulty display path is active can
make the next VRR transition oops.

Use a local terminal or an ssh session over Wi-Fi. Then run on the Legion:

```bash
sudo steamos-readonly disable

sudo systemctl disable --now freesync-mccs-guard-verify.service
sudo systemctl disable freesync-mccs-guard-ensure.service
sudo rm -f /etc/systemd/system/freesync-mccs-guard-ensure.service
sudo modprobe -r freesync_mccs_guard
sudo dkms remove -m freesync-mccs-guard -v 1.0.0 --all

sudo rm -f /etc/modules-load.d/freesync-mccs-guard.conf
sudo rm -f /etc/modprobe.d/freesync-mccs-guard.conf
sudo rm -f /etc/systemd/system/freesync-mccs-guard-verify.service
sudo rm -f /etc/atomic-update.conf.d/93-freesync-mccs-guard.conf
sudo rm -rf /usr/src/freesync-mccs-guard-1.0.0

sudo systemctl daemon-reload
sudo depmod -a "$(uname -r)"
sudo steamos-readonly enable
```

Expected result:

```bash
test ! -e /sys/module/freesync_mccs_guard
modinfo freesync_mccs_guard >/dev/null 2>&1 && echo UNEXPECTED || echo REMOVED
dkms status -m freesync-mccs-guard -v 1.0.0
```

Leave `dkms`, the compiler, and headers installed unless they were installed
solely for this experiment and the owner explicitly wants to remove those
packages too.

## Side-effect audit

Suppressing the worker cannot make it continuously requeue itself:

- `amdgpu_dm_crtc_duplicate_state()` allocates the new state with
  `kzalloc_obj()` and copies selected members, but does not copy
  `freesync_vrr_info_changed` (`amdgpu_dm_crtc.c:415-448`). It therefore starts
  false for each new atomic state.
- `update_freesync_state_on_stream()` ORs in the result of comparing the old
  and newly built VRR info packets and schedules one allocation/work item only
  when that comparison indicates a change (`amdgpu_dm.c:10189-10205`).
- Neither the original callback nor the replacement requeues itself.
- The replacement calls `kfree(work)`. This is valid for the exact affected
  source because `struct work_struct work` is the first member of the allocated
  private object, and the original callback also frees that object before
  returning (`amdgpu_dm.c:181-185, 241-257`).

Thus there is one tiny no-op callback per genuine VRR packet transition, no
spin, no persistent flag, and no allocation leak.
