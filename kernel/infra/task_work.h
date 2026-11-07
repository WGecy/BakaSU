/* SPDX-License-Identifier: GPL-2.0 */
#ifndef __KSU_TASK_WORK_H
#define __KSU_TASK_WORK_H

#include <linux/module.h>
#include <linux/task_work.h>

#include "compat/kernel_compat.h"

/*
 * Queue task work on @task.
 *
 * A successful queue owns a module reference until the callback finishes, so
 * that unloading the LKM while a callback is still pending cannot tear the
 * code out from under it. In built-in builds there is nothing to pin.
 */
static inline int ksu_task_work_add(struct task_struct *task, struct callback_head *work)
{
#ifdef MODULE
    int ret;

    if (!try_module_get(THIS_MODULE))
        return -ENODEV;
    ret = task_work_add(task, work, TWA_RESUME);
    if (ret)
        module_put(THIS_MODULE);
    return ret;
#else
    return task_work_add(task, work, TWA_RESUME);
#endif
}

#endif /* __KSU_TASK_WORK_H */
