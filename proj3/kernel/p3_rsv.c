// proj3/kernel/p3_rsv.c — Project 3: 4.2 + 4.4 with exit-cleanup
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
#include <linux/hrtimer.h>
#include <linux/wait.h>
#include <linux/atomic.h>
#include <linux/init.h>
#include <trace/events/sched.h>   // register_trace_sched_process_exit()

#define P3_MAX_RSV 50

struct p3_rsv_entry {
	struct list_head node;
	pid_t pid;
	struct task_struct *task; /* ref held */

	/* (C,T) in nanoseconds */
	ktime_t C_ns;
	ktime_t T_ns;

	/* RM-assigned priority */
	int prio;

	/* 4.4: periodic wake-up machinery */
	struct hrtimer timer;
	ktime_t next_release;
	wait_queue_head_t wq;
	atomic64_t period_seq;  /* increments each period to wake sleepers */
	bool canceled;
};

static LIST_HEAD(p3_rsv_list);
static DEFINE_SPINLOCK(p3_rsv_lock);

/* ---------- helpers ---------- */

static struct p3_rsv_entry *p3_find_locked(pid_t pid)
{
	struct p3_rsv_entry *it;
	list_for_each_entry(it, &p3_rsv_list, node) {
		if (it->pid == pid)
			return it;
	}
	return NULL;
}

/* Rate-Monotonic: shorter T -> higher prio */
static void p3_reassign_rm_prios_locked(void)
{
	struct p3_rsv_entry *it, *arr[P3_MAX_RSV];
	int n = 0, i, j;

	list_for_each_entry(it, &p3_rsv_list, node) {
		if (n < P3_MAX_RSV) arr[n++] = it;
	}
	for (i = 0; i < n; i++)
		for (j = i + 1; j < n; j++)
			if (ktime_to_ns(arr[j]->T_ns) < ktime_to_ns(arr[i]->T_ns)) {
				struct p3_rsv_entry *tmp = arr[i]; arr[i] = arr[j]; arr[j] = tmp;
			}

	for (i = 0; i < n; i++) {
		int prio = (MAX_RT_PRIO - 1) - i; if (prio < 1) prio = 1;
		arr[i]->prio = prio;
		if (arr[i]->task) {
			struct sched_param sp = { .sched_priority = prio };
			sched_setscheduler_nocheck(arr[i]->task, SCHED_FIFO, &sp);
		}
	}
}

/* hrtimer callback: mark new period, wake waiters, re-arm */
static enum hrtimer_restart p3_timer_cb(struct hrtimer *t)
{
	struct p3_rsv_entry *e = container_of(t, struct p3_rsv_entry, timer);

	if (READ_ONCE(e->canceled))
		return HRTIMER_NORESTART;

	atomic64_inc(&e->period_seq);
	hrtimer_forward_now(&e->timer, e->T_ns);
	wake_up_all(&e->wq);
	return HRTIMER_RESTART;
}

/* Free one reservation entry (common path for cancel + exit) */
static void p3_free_entry(struct p3_rsv_entry *e)
{
	if (!e) return;

	WRITE_ONCE(e->canceled, true);
	hrtimer_cancel(&e->timer);
	wake_up_all(&e->wq);

	if (e->task) {
		/* Move task back to normal if it's still alive */
		if (!(e->task->flags & PF_EXITING)) {
			struct sched_param sp = { .sched_priority = 0 };
			sched_setscheduler_nocheck(e->task, SCHED_NORMAL, &sp);
		}
		put_task_struct(e->task);
	}
	kfree(e);
}

/* ---------- syscalls ---------- */

SYSCALL_DEFINE3(set_rsv, pid_t, pid,
		struct timespec __user *, C,
		struct timespec __user *, T)
{
	struct timespec c_us, t_us;
	struct p3_rsv_entry *e, *tmp;
	struct task_struct *p;
	unsigned long flags;
	ktime_t Cns, Tns;
	int count = 0;

	if (pid == 0) pid = current->pid;

	if (copy_from_user(&c_us, C, sizeof(c_us)) ||
	    copy_from_user(&t_us, T, sizeof(t_us)))
		return -EFAULT;

	if (c_us.tv_sec < 0 || c_us.tv_nsec < 0 || t_us.tv_sec < 0 || t_us.tv_nsec < 0)
		return -EINVAL;

	Cns = ktime_set(c_us.tv_sec, c_us.tv_nsec);
	Tns = ktime_set(t_us.tv_sec, t_us.tv_nsec);
	if (ktime_to_ns(Cns) <= 0 || ktime_to_ns(Tns) <= 0)
		return -EINVAL;
	if (ktime_to_ns(Cns) > ktime_to_ns(Tns))
		return -EINVAL;

	rcu_read_lock();
	p = pid_task(find_vpid(pid), PIDTYPE_PID);
	if (p) get_task_struct(p);
	rcu_read_unlock();
	if (!p) return -ESRCH;

	spin_lock_irqsave(&p3_rsv_lock, flags);

	if (p3_find_locked(pid)) {
		spin_unlock_irqrestore(&p3_rsv_lock, flags);
		put_task_struct(p);
		return -EBUSY;
	}

	list_for_each_entry(tmp, &p3_rsv_list, node) count++;
	if (count >= P3_MAX_RSV) {
		spin_unlock_irqrestore(&p3_rsv_lock, flags);
		put_task_struct(p);
		return -ENOSPC;
	}

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
	atomic64_set(&e->period_seq, 0);
	e->canceled = false;

	/* 4.4: init periodic wake machinery */
	init_waitqueue_head(&e->wq);
	hrtimer_init(&e->timer, CLOCK_MONOTONIC, HRTIMER_MODE_PINNED);
	e->timer.function = p3_timer_cb;
	hrtimer_start(&e->timer, e->T_ns, HRTIMER_MODE_REL_PINNED);

	list_add_tail(&e->node, &p3_rsv_list);
	p3_reassign_rm_prios_locked();

	spin_unlock_irqrestore(&p3_rsv_lock, flags);
	return 0;
}

SYSCALL_DEFINE1(cancel_rsv, pid_t, pid)
{
	struct p3_rsv_entry *e;
	unsigned long flags;

	if (pid == 0) pid = current->pid;

	spin_lock_irqsave(&p3_rsv_lock, flags);
	e = p3_find_locked(pid);
	if (!e) {
		spin_unlock_irqrestore(&p3_rsv_lock, flags);
		return -ENOENT;
	}
	list_del(&e->node);
	spin_unlock_irqrestore(&p3_rsv_lock, flags);

	p3_free_entry(e);

	/* recompute remaining priorities */
	spin_lock_irqsave(&p3_rsv_lock, flags);
	p3_reassign_rm_prios_locked();
	spin_unlock_irqrestore(&p3_rsv_lock, flags);

	return 0;
}

/* 4.4.1: wait until next period boundary for the caller's reservation */
SYSCALL_DEFINE0(wait_until_next_period)
{
	struct p3_rsv_entry *e;
	unsigned long flags;
	u64 seen;

	spin_lock_irqsave(&p3_rsv_lock, flags);
	e = p3_find_locked(current->pid);
	if (!e) {
		spin_unlock_irqrestore(&p3_rsv_lock, flags);
		return -ENOENT;
	}
	seen = atomic64_read(&e->period_seq);
	spin_unlock_irqrestore(&p3_rsv_lock, flags);

	if (atomic64_read(&e->period_seq) != seen)
		return 0;

	if (wait_event_interruptible(e->wq,
		atomic64_read(&e->period_seq) != seen || READ_ONCE(e->canceled)))
		return -EINTR;

	if (READ_ONCE(e->canceled))
		return -ENOENT;

	return 0;
}

/* ---------- cleanup on task exit (4.4.2) ---------- */
/* Triggered when any task exits; if it has a reservation, free it. */
static void p3_sched_exit_cb(void *ignore, struct task_struct *p)
{
	struct p3_rsv_entry *e;
	unsigned long flags;

	if (!p)
		return;

	spin_lock_irqsave(&p3_rsv_lock, flags);
	e = p3_find_locked(p->pid);
	if (e)
		list_del(&e->node);
	spin_unlock_irqrestore(&p3_rsv_lock, flags);

	if (e)
		p3_free_entry(e);

	/* reprioritize survivors */
	spin_lock_irqsave(&p3_rsv_lock, flags);
	p3_reassign_rm_prios_locked();
	spin_unlock_irqrestore(&p3_rsv_lock, flags);
}

/* Register our exit callback at late init */
static int __init p3_init(void)
{
	register_trace_sched_process_exit(p3_sched_exit_cb, NULL);
	return 0;
}
late_initcall(p3_init);
