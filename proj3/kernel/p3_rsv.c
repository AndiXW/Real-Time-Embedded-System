#include <linux/kernel.h>
#include <linux/syscalls.h>
#include <linux/sched.h>
#include <linux/sched/prio.h>
#include <linux/uaccess.h>
#include <linux/slab.h>
#include <linux/spinlock.h>
#include <linux/rcupdate.h>
#include <linux/list.h>
#include <linux/time.h>
#include <linux/ktime.h>

#define P3_MAX_RSV 50

struct p3_rsv_entry {
	struct list_head node;
	pid_t pid;
	struct task_struct *task; /* ref held */
	ktime_t C_ns;
	ktime_t T_ns;
	int prio; /* SCHED_FIFO prio we assigned */
};

static LIST_HEAD(p3_rsv_list);
static DEFINE_SPINLOCK(p3_rsv_lock);

/* Helper: assign SCHED_FIFO priorities by Rate-Monotonic (shorter T -> higher prio) */
static void p3_reassign_rm_prios_locked(void)
{
	struct p3_rsv_entry *it;
	/* Collect pointers in a temporary array to sort by T */
	struct p3_rsv_entry *arr[P3_MAX_RSV];
	int n = 0, i;

	list_for_each_entry(it, &p3_rsv_list, node) {
		if (n < P3_MAX_RSV)
			arr[n++] = it;
	}
	/* Simple O(n^2) sort (n <= 50) by ascending T */
	for (i = 0; i < n; i++) {
		int j;
		for (j = i + 1; j < n; j++) {
			if (ktime_to_ns(arr[j]->T_ns) < ktime_to_ns(arr[i]->T_ns)) {
				struct p3_rsv_entry *tmp = arr[i];
				arr[i] = arr[j]; arr[j] = tmp;
			}
		}
	}
	/* Highest RT prio is MAX_RT_PRIO-1 (typically 99), lowest is 1 */
	for (i = 0; i < n; i++) {
		int prio = (MAX_RT_PRIO - 1) - i;
		struct sched_param sp = { .sched_priority = prio };
		if (prio < 1) prio = 1; /* clamp just in case */
		arr[i]->prio = prio;
		/* Change scheduling class of the task */
		if (arr[i]->task) {
			sched_setscheduler_nocheck(arr[i]->task, SCHED_FIFO, &sp);
		}
	}
}

/* Find reservation entry by pid (caller holds p3_rsv_lock) */
static struct p3_rsv_entry *p3_find_locked(pid_t pid)
{
	struct p3_rsv_entry *it;
	list_for_each_entry(it, &p3_rsv_list, node) {
		if (it->pid == pid)
			return it;
	}
	return NULL;
}

SYSCALL_DEFINE3(set_rsv, pid_t, pid,
		struct timespec __user *, C,
		struct timespec __user *, T)
{
	struct timespec c_us, t_us;
	struct p3_rsv_entry *e;
	struct task_struct *p;
	unsigned long flags;
	ktime_t Cns, Tns;

	/* If pid==0, apply to current */
	if (pid == 0)
		pid = current->pid;

	/* Copy C/T from userspace */
	if (copy_from_user(&c_us, C, sizeof(c_us)) ||
	    copy_from_user(&t_us, T, sizeof(t_us)))
		return -EFAULT;

	/* Basic validation: strictly positive, and C <= T */
	if (c_us.tv_sec < 0 || c_us.tv_nsec < 0 || t_us.tv_sec < 0 || t_us.tv_nsec < 0)
		return -EINVAL;
	Cns = ktime_set(c_us.tv_sec, c_us.tv_nsec);
	Tns = ktime_set(t_us.tv_sec, t_us.tv_nsec);
	if (ktime_to_ns(Cns) <= 0 || ktime_to_ns(Tns) <= 0)
		return -EINVAL;
	if (ktime_to_ns(Cns) > ktime_to_ns(Tns))
		return -EINVAL;

	/* Resolve task */
	rcu_read_lock();
	p = pid_task(find_vpid(pid), PIDTYPE_PID);
	if (p)
		get_task_struct(p); /* hold a ref; released on cancel */
	rcu_read_unlock();
	if (!p)
		return -ESRCH;

	spin_lock_irqsave(&p3_rsv_lock, flags);

	/* Reject if already has a reservation */
	if (p3_find_locked(pid)) {
		spin_unlock_irqrestore(&p3_rsv_lock, flags);
		put_task_struct(p);
		return -EBUSY;
	}

	/* Limit total reservations */
	{
		int count = 0;
		list_for_each_entry(e, &p3_rsv_list, node) count++;
		if (count >= P3_MAX_RSV) {
			spin_unlock_irqrestore(&p3_rsv_lock, flags);
			put_task_struct(p);
			return -ENOSPC;
		}
	}

	/* Allocate and insert */
	e = kzalloc(sizeof(*e), GFP_KERNEL);
	if (!e) {
		spin_unlock_irqrestore(&p3_rsv_lock, flags);
		put_task_struct(p);
		return -ENOMEM;
	}
	e->pid  = pid;
	e->task = p;
	e->C_ns = Cns;
	e->T_ns = Tns;
	list_add_tail(&e->node, &p3_rsv_list);

	/* Assign SCHED_FIFO priority by RM across all reservations */
	p3_reassign_rm_prios_locked();

	spin_unlock_irqrestore(&p3_rsv_lock, flags);
	return 0;
}

SYSCALL_DEFINE1(cancel_rsv, pid_t, pid)
{
	struct p3_rsv_entry *e;
	unsigned long flags;

	/* If pid==0, apply to current */
	if (pid == 0)
		pid = current->pid;

	spin_lock_irqsave(&p3_rsv_lock, flags);
	e = p3_find_locked(pid);
	if (!e) {
		spin_unlock_irqrestore(&p3_rsv_lock, flags);
		return -ENOENT;
	}

	list_del(&e->node);
	/* Demote task to SCHED_NORMAL (normal) */
	if (e->task) {
		struct sched_param sp = { .sched_priority = 0 };
		sched_setscheduler_nocheck(e->task, SCHED_NORMAL, &sp);
		put_task_struct(e->task);
	}
	kfree(e);

	/* Recompute RM priorities for remaining tasks */
	p3_reassign_rm_prios_locked();

	spin_unlock_irqrestore(&p3_rsv_lock, flags);
	return 0;
}
