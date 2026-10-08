# freesync-mccs-guard

A small kernel module that stops a Legion Go S on SteamOS 3.9 from crashing,
and bootlooping, when it drives a TV through a USB-C dock's HDMI port.

The crash is in Valve's downstream amdgpu code. A deferred worker,
`dm_freesync_mccs_ddc_worker()`, sends a DDC/MCCS message to the display after
every FreeSync change. On a DisplayPort-to-HDMI converter (here, the HP USB-C
Dock G6) it picks an I2C adapter that was never registered, and `i2c_transfer()`
dereferences a NULL pointer. SteamOS panics on oops, so the machine reboots,
and because the dock is still connected it crashes again about 26 seconds into
the next boot.

This module puts a kprobe on that worker and redirects it to a replacement
that only frees the work item. The display keeps working at 4K 120 Hz; the
skipped MCCS message is not needed for mode setting or link training.

Reported to Valve as
[ValveSoftware/SteamOS#2786](https://github.com/ValveSoftware/SteamOS/issues/2786).
The full root-cause analysis is in [BUG-REPORT.md](BUG-REPORT.md).

## Status

Not fixed as of 2026-10-08. Every SteamOS build and kernel Valve has
published since still has the same worker:

| Branch | Newest build | SteamOS | Kernel |
|---|---|---|---|
| stable | 20260922.1 | 3.8.28 | 6.18.50-valve2 |
| beta / preview | 20260925.101 | 3.9.2 | 7.2.7-valve1 |
| main | 20260924.1000 | 3.10.0 | 7.2.4-valve1 |

`dm_freesync_mccs_ddc_worker()` is identical in all of them and in the newest
kernel tag, 7.2.7-valve2, which no image ships yet. It still takes
`&aconn->dm_dp_aux.aux.ddc` without checking that the adapter is registered
([amdgpu_dm.c#L181-L236 at 7.2.7-valve2](https://github.com/evlaV/linux-integration/blob/7.2.7-valve2/drivers/gpu/drm/amd/display/amdgpu_dm/amdgpu_dm.c#L181-L236),
via the evlaV mirror of Valve's kernel tree). `struct work_struct work` is
still the first member of `struct dm_freesync_mccs_ddc_work` in those kernels,
so the guard's `kfree(work)` remains correct there. Each new kernel still has
to be checked and added to `verified-kernels` before the guard is rebuilt for it.

## Are you affected?

To see the crash without the reboot, boot undocked, turn off panic-on-oops for
this boot only, plug the dock in, wait a minute and search the kernel log:

```bash
sudo sysctl kernel.panic_on_oops=0
# plug the dock in, wait ~60 s
journalctl -k -b | grep -B2 -A8 'dm_freesync_mccs_ddc_worker'
```

The machine stays up, but a worker thread has died, so reboot undocked
afterwards. The signature is:

```text
BUG: kernel NULL pointer dereference, address: 0000000000000000
Workqueue: events dm_freesync_mccs_ddc_worker [amdgpu]
RIP: 0010:i2c_transfer+0x79/0x150
 dm_freesync_mccs_ddc_worker+0x16f/0x200 [amdgpu]
```

SteamOS also queues crash dumps in
`/var/lib/steamos-log-submitter/pending/kdump/`; a pile of `kdumpst-*.zip`
files from the same day is a good hint.

## Tested on

| | |
|---|---|
| Device | Lenovo Legion Go S (83N6) |
| SteamOS | 3.9.0, build 20260828.100 (preview) |
| Kernel | `7.2.0-valve1-1-neptune-72-gd39b4282853d` |
| Dock | HP USB-C 100W G6 (DP MST, DP-to-HDMI PCON) |
| Display | TCL C6K over HDMI, 3840x2160 at 120 Hz |

The same crash also happened on the earlier `6.16.12-valve` kernel. Other
SteamOS devices on these kernels with a DP-to-HDMI converter may hit it too,
but only the setup above has been tested.

On the tested kernel a docked boot arms the guard at about T+7 s, the faulty
worker fires at about T+26 s and is skipped (`skipped=1`, `nmissed=0`), and
no oops is logged.

## Install

[INSTALL.md](INSTALL.md) has every step with the checks to run between them.
In short, with the Legion **undocked**:

```bash
git clone https://github.com/mchibouni/freesync-mccs-guard.git ~/kprobe
cd ~/kprobe
sudo steamos-readonly disable
sudo pacman -S --needed dkms base-devel linux-neptune-72-headers
make && sudo insmod ./freesync_mccs_guard.ko     # try it once by hand
cat /sys/module/freesync_mccs_guard/parameters/armed   # must print Y
```

Then dock, wait a minute, and check that
`/sys/module/freesync_mccs_guard/parameters/skipped` went up and the kernel
log has no new oops. If so, follow INSTALL.md to install it through DKMS with
the boot wiring in `rootfs/etc/`, so it loads on every boot before udev
enumerates the dock.

The paths in the scripts assume the repo lives at `/home/deck/kprobe`.

## Check that it is working

```bash
~/kprobe/guard          # read-only health check
sudo ~/kprobe/guard     # also checks the kprobe is registered
```

A healthy result shows `armed = Y`, `nmissed = 0` and `TAINT_DIE clear`.
`skipped = 1` or more means the faulty worker fired and was intercepted.

## SteamOS updates

An OS update replaces `/usr` and `/var`, which removes the built module. The
first docked boot after an update is unguarded and will bootloop, so:

1. Boot **undocked** after an update.
2. Check that `struct work_struct work` is still the first member of
   `struct dm_freesync_mccs_ddc_work` in the new kernel's `amdgpu_dm.c`. The
   replacement function calls `kfree(work)` and would corrupt the heap if that
   changed.
3. Add the new kernel to the list and rebuild:

   ```bash
   uname -r >> ~/kprobe/verified-kernels
   sudo ~/kprobe/guard rebuild
   ```

4. Reboot undocked, run `~/kprobe/guard`, and dock only once it shows
   `armed = Y`.

`ensure/ensure-guard.sh` can do step 3's rebuild automatically at boot, but
only for kernels already in `verified-kernels`. `check-updates` lets you query
for a new SteamOS build while automatic updates are masked. Both are explained
in INSTALL.md.

If a future kernel drops or fixes `dm_freesync_mccs_ddc_worker`, remove the
guard (see "Undo" in INSTALL.md) instead of adapting it.

## Files

| Path | What it is |
|---|---|
| `freesync_mccs_guard.c`, `Makefile`, `dkms.conf` | The module and its DKMS packaging |
| `rootfs/etc/` | Boot wiring: load order, a unit that checks the guard is armed before udev coldplug, and the atomic-update keep list |
| `guard` | Health check, `rebuild` after an update, `vanilla` to restore panic-on-oops |
| `ensure/` | Boot-time rebuild script and its systemd unit |
| `verified-kernels` | Kernels whose struct layout has been checked by hand |
| `check-updates` | Query or apply a SteamOS update while the updater is masked |
| `INSTALL.md` | Step-by-step install, test, update and removal |
| `BUG-REPORT.md` | Root-cause analysis of the crash |

## License

GPL-2.0, same as the kernel. See [LICENSE](LICENSE).
