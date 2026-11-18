/* SPDX-License-Identifier: GPL-2.0 */
#include <linux/arm_mpam.h>

#include <asm/mpam.h>

static inline void resctrl_arch_task_fork(struct task_struct *child)
{
	mpam_fork_sync(child, current);
}
