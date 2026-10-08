// SPDX-License-Identifier: GPL-2.0-only
/*
 * freesync_mccs_guard.c
 *
 * Temporary runtime mitigation for SteamOS/Neptune 7.2.0-valve1:
 * dm_freesync_mccs_ddc_worker() passes an unregistered DP AUX I2C adapter to
 * i2c_transfer(), which dereferences a NULL i2c_adapter::lock_ops pointer.
 *
 * Redirect the worker at function entry to a replacement which performs only
 * the allocation cleanup.  The replacement then returns normally, allowing
 * process_one_work() to run its busy-hash and in-flight-accounting epilogue.
 *
 * This is deliberately a kprobe rather than a source fix.  Remove it once the
 * downstream worker has been fixed or reverted in the installed kernel.
 */

#include <linux/atomic.h>
#include <linux/errno.h>
#include <linux/kprobes.h>
#include <linux/module.h>
#include <linux/moduleparam.h>
#include <linux/ptrace.h>
#include <linux/slab.h>
#include <linux/sysfs.h>
#include <linux/workqueue.h>

#if !defined(CONFIG_X86_64)
#error "freesync_mccs_guard is implemented only for x86-64"
#endif

#define GUARD_TARGET_SYMBOL "amdgpu:dm_freesync_mccs_ddc_worker"

static atomic64_t skipped_count = ATOMIC64_INIT(0);

/*
 * In the affected source, struct dm_freesync_mccs_ddc_work has struct
 * work_struct as its first member.  The original callback's out_free path does
 * kfree(container_of(work, ..., work)); therefore kfree(work) is equivalent.
 * process_one_work() permits a callback to free its own work item and retains
 * the accounting value it needs before invoking the callback.
 *
 * Keep this as a real same-signature function.  Redirecting execution here and
 * leaving the caller's return address on the stack is safer than manually
 * popping regs->sp in a kprobe handler.  A normal RET returns to
 * process_one_work().
 */
static noinline notrace void guard_worker_replacement(struct work_struct *work)
{
	kfree(work);
}
NOKPROBE_SYMBOL(guard_worker_replacement);

static int guard_pre_handler(struct kprobe *p, struct pt_regs *regs)
{
	atomic64_inc(&skipped_count);
	instruction_pointer_set(regs,
				(unsigned long)guard_worker_replacement);

	/* Non-zero tells kprobes that the handler changed the execution path. */
	return 1;
}
NOKPROBE_SYMBOL(guard_pre_handler);

/*
 * This handler is intentionally present even though guard_pre_handler() skips
 * it.  On the affected kernel it has two safety effects:
 *
 *  - ordinary x86 optprobes are disabled, because optimized probes ignore an
 *    instruction-pointer change made by a pre-handler; and
 *  - KPROBES_ON_FTRACE selects kprobe_ipmodify_ops, whose ftrace operations
 *    carry FTRACE_OPS_FL_IPMODIFY.
 */
static void guard_post_handler(struct kprobe *p, struct pt_regs *regs,
			       unsigned long flags)
{
}
NOKPROBE_SYMBOL(guard_post_handler);

static struct kprobe guard_kprobe = {
	.symbol_name = GUARD_TARGET_SYMBOL,
	.pre_handler = guard_pre_handler,
	.post_handler = guard_post_handler,
	/* Register disabled, then explicitly enable so arming errors propagate. */
	.flags = KPROBE_FLAG_DISABLED,
};

static int skipped_get(char *buf, const struct kernel_param *kp)
{
	return sysfs_emit(buf, "%lld\n",
			  (long long)atomic64_read(&skipped_count));
}

static int armed_get(char *buf, const struct kernel_param *kp)
{
	u32 flags = READ_ONCE(guard_kprobe.flags);
	bool armed = !(flags & (KPROBE_FLAG_DISABLED | KPROBE_FLAG_GONE));

	return sysfs_emit(buf, "%c\n", armed ? 'Y' : 'N');
}

static int nmissed_get(char *buf, const struct kernel_param *kp)
{
	return sysfs_emit(buf, "%lu\n", READ_ONCE(guard_kprobe.nmissed));
}

static const struct kernel_param_ops skipped_ops = {
	.get = skipped_get,
};

static const struct kernel_param_ops armed_ops = {
	.get = armed_get,
};

static const struct kernel_param_ops nmissed_ops = {
	.get = nmissed_get,
};

module_param_cb(armed, &armed_ops, NULL, 0444);
MODULE_PARM_DESC(armed, "Whether the target kprobe is currently armed");
module_param_cb(skipped, &skipped_ops, NULL, 0444);
MODULE_PARM_DESC(skipped, "Number of worker invocations redirected");
module_param_cb(nmissed, &nmissed_ops, NULL, 0444);
MODULE_PARM_DESC(nmissed, "Number of target invocations missed by kprobes");

/*
 * A symbol_name kprobe cannot resolve a module-local symbol until that module
 * is live.  The embedded and modprobe.d soft dependencies make modprobe load
 * amdgpu synchronously before this module.  Direct insmod bypasses softdeps and
 * is supported only after the caller has verified that amdgpu is already live.
 */
static int __init guard_init(void)
{
	int ret;

	ret = register_kprobe(&guard_kprobe);
	if (ret) {
		pr_err("cannot register probe for %s: %d (is amdgpu live, and is the symbol probeable?)\n",
		       GUARD_TARGET_SYMBOL, ret);
		return ret;
	}

	/*
	 * Neptune 7.2's __register_kprobe() does not propagate an ftrace-arm
	 * failure from its ordinary enabled-registration path.  Registering
	 * disabled and calling enable_kprobe() separately makes that error visible.
	 * Also reject a globally disarmed kprobe subsystem: enable_kprobe() returns
	 * zero in that case while leaving KPROBE_FLAG_DISABLED set.
	 */
	ret = enable_kprobe(&guard_kprobe);
	if (ret || kprobe_disabled(&guard_kprobe)) {
		if (!ret)
			ret = -EAGAIN;
		pr_err("cannot arm probe for %s: %d\n",
		       GUARD_TARGET_SYMBOL, ret);
		unregister_kprobe(&guard_kprobe);
		return ret;
	}

	pr_info("armed at %ps (%px)\n", guard_kprobe.addr,
		guard_kprobe.addr);
	return 0;
}

static void __exit guard_exit(void)
{
	unsigned long nmissed = READ_ONCE(guard_kprobe.nmissed);

	unregister_kprobe(&guard_kprobe);
	pr_info("disarmed after redirecting %lld invocation(s), nmissed=%lu\n",
		(long long)atomic64_read(&skipped_count), nmissed);
}

module_init(guard_init);
module_exit(guard_exit);

MODULE_AUTHOR("Local SteamOS mitigation");
MODULE_DESCRIPTION("Neutralize the faulty amdgpu FreeSync MCCS DDC worker");
MODULE_LICENSE("GPL");
MODULE_VERSION("1.0.0");
MODULE_SOFTDEP("pre: amdgpu");
