/* Copyright (c) Andrea Righi <andrea.righi@linux.dev> */
/*
 * scx_rustland_core: BPF backend for schedulers running in user-space.
 *
 * This BPF backend implements the low level sched-ext functionalities for a
 * user-space counterpart, that implements the actual scheduling policy.
 *
 * The BPF part collects total cputime and weight from the tasks that need to
 * run, then it sends all details to the user-space scheduler that decides the
 * best order of execution of the tasks (based on the collected metrics).
 *
 * The user-space scheduler then returns to the BPF component the list of tasks
 * to be dispatched in the proper order.
 *
 * Messages between the BPF component and the user-space scheduler are passed
 * using BPF_MAP_TYPE_RINGBUFFER / BPF_MAP_TYPE_USER_RINGBUF maps: @queued for
 * the messages sent by the BPF dispatcher to the user-space scheduler and
 * @dispatched for the messages sent by the user-space scheduler to the BPF
 * dispatcher.
 *
 * The BPF dispatcher is completely agnostic of the particular scheduling
 * policy implemented in user-space. For this reason developers that are
 * willing to use this scheduler to experiment scheduling policies should be
 * able to simply modify the Rust component, without having to deal with any
 * internal kernel / BPF details.
 *
 * MODIFIED: This version has been hardcoded to only use CPUs 0 and 20.
 * All other CPUs will be ignored and tasks will only be dispatched to these
 * two CPUs. This restriction is enforced throughout the scheduler.
 *
 * This software may be used and distributed according to the terms of the
 * GNU General Public License version 2.
 */
#ifdef LSP
#define __bpf__
#include "../../../../scheds/include/scx/common.bpf.h"
#else
#include <scx/common.bpf.h>
#endif

#include "intf.h"

char _license[] SEC("license") = "GPL";

UEI_DEFINE(uei);

/*
 * Introduce a custom DSQ shared across all the CPUs, where we can dispatch
 * tasks that will be executed on the first CPU available.
 *
 * Per-CPU DSQs are also provided, to allow the scheduler to run a task on a
 * specific CPU (see dsq_init()).
 */
#define SHARED_DSQ MAX_CPUS

/*
 * The user-space scheduler itself is dispatched using a separate DSQ, that
 * is consumed after all other DSQs.
 *
 * This ensures to work in bursts: tasks are queued, then the user-space
 * scheduler runs and dispatches them. Once all these tasks exhaust their
 * time slices, the scheduler is invoked again, repeating the cycle.
 */
#define SCHED_DSQ (MAX_CPUS + 1)

/*
 * Scheduler attributes and statistics.
 */
const volatile u32 usersched_pid; /* User-space scheduler PID */
const volatile u32 khugepaged_pid; /* khugepaged PID */
u64 usersched_last_run_at; /* Timestamp of the last user-space scheduler execution */
static u64 nr_cpu_ids; /* Maximum possible CPU number */

/*
 * Switch all tasks or SCHED_EXT tasks.
 */
const volatile bool switch_partial;

/*
 * Number of tasks that are queued for scheduling.
 *
 * This number is incremented by the BPF component when a task is queued to the
 * user-space scheduler and it must be decremented by the user-space scheduler
 * when a task is consumed.
 */
volatile u64 nr_queued;

/*
 * Number of tasks that are waiting for scheduling.
 *
 * This number must be updated by the user-space scheduler to keep track if
 * there is still some scheduling work to do.
 */
volatile u64 nr_scheduled;

/*
 * Amount of currently running tasks.
 */
volatile u64 nr_running, nr_online_cpus;

/* Dispatch statistics */
volatile u64 nr_user_dispatches, nr_kernel_dispatches,
	     nr_cancel_dispatches, nr_bounce_dispatches;

/* Failure statistics */
volatile u64 nr_failed_dispatches, nr_sched_congested;

/* Per-CPU scheduled tasks count */
volatile u64 nr_scheduled_per_cpu[MAX_CPUS];

 /* Report additional debugging information */
const volatile bool debug;

/* Rely on the in-kernel idle CPU selection policy */
const volatile bool builtin_idle;

/* Allow to use bpf_printk() only when @debug is set */
#define dbg_msg(_fmt, ...) do {						\
	if (debug)							\
		bpf_printk(_fmt, ##__VA_ARGS__);			\
} while(0)

/*
 * CPUs in the system have SMT is enabled.
 */
const volatile bool smt_enabled = true;

/*
 * Allocate/re-allocate a new cpumask.
 */
static int calloc_cpumask(struct bpf_cpumask **p_cpumask)
{
	struct bpf_cpumask *cpumask;

	cpumask = bpf_cpumask_create();
	if (!cpumask)
		return -ENOMEM;

	cpumask = bpf_kptr_xchg(p_cpumask, cpumask);
	if (cpumask)
		bpf_cpumask_release(cpumask);

	return 0;
}

/*
 * Maximum amount of tasks queued between kernel and user-space at a certain
 * time.
 *
 * The @queued and @dispatched lists are used in a producer/consumer fashion
 * between the BPF part and the user-space part.
 */
#define MAX_ENQUEUED_TASKS 4096

/*
 * Maximum amount of slots reserved to the tasks dispatched via shared queue.
 */
#define MAX_DISPATCH_SLOT (MAX_ENQUEUED_TASKS / 8)

/*
 * The map containing tasks that are queued to user space from the kernel.
 *
 * This map is drained by the user space scheduler.
 */
struct {
	__uint(type, BPF_MAP_TYPE_RINGBUF);
	__uint(max_entries, MAX_ENQUEUED_TASKS *
				sizeof(struct queued_task_ctx));
} queued SEC(".maps");

/*
 * The user ring buffer containing pids that are dispatched from user space to
 * the kernel.
 *
 * Drained by the kernel in .dispatch().
 */
struct {
        __uint(type, BPF_MAP_TYPE_USER_RINGBUF);
	__uint(max_entries, MAX_ENQUEUED_TASKS *
				sizeof(struct dispatched_task_ctx));
} dispatched SEC(".maps");

/*
 * Per-CPU context.
 */
struct cpu_ctx {
	struct bpf_cpumask __kptr *l2_cpumask;
	struct bpf_cpumask __kptr *l3_cpumask;
};

struct {
	__uint(type, BPF_MAP_TYPE_PERCPU_ARRAY);
	__type(key, u32);
	__type(value, struct cpu_ctx);
	__uint(max_entries, 1);
} cpu_ctx_stor SEC(".maps");

/*
 * Return a CPU context.
 */
struct cpu_ctx *try_lookup_cpu_ctx(s32 cpu)
{
	const u32 idx = 0;
	return bpf_map_lookup_percpu_elem(&cpu_ctx_stor, &idx, cpu);
}

/*
 * Per-task local storage.
 *
 * This contain all the per-task information used internally by the BPF code.
 */
struct task_ctx {
	/*
	 * Temporary cpumask for calculating scheduling domains.
	 */
	struct bpf_cpumask __kptr *l2_cpumask;
	struct bpf_cpumask __kptr *l3_cpumask;

	/*
	 * Timestamp since last time the task ran on a CPU.
	 */
	u64 start_ts;

	/*
	 * Timestamp since last time the task released a CPU.
	 */
	u64 stop_ts;

	/*
	 * Execution time (in nanoseconds) since the last sleep event.
	 */
	u64 exec_runtime;

	/*
	 * CPU this task was dispatched to (for tracking strict affinity violations)
	 */
	s32 dispatched_cpu;
};

/* Map that contains task-local storage. */
struct {
	__uint(type, BPF_MAP_TYPE_TASK_STORAGE);
	__uint(map_flags, BPF_F_NO_PREALLOC);
	__type(key, int);
	__type(value, struct task_ctx);
} task_ctx_stor SEC(".maps");

/*
 * Return a local task context from a generic task or NULL if the context
 * doesn't exist.
 */
struct task_ctx *try_lookup_task_ctx(const struct task_struct *p)
{
	struct task_ctx *tctx = bpf_task_storage_get(&task_ctx_stor,
						(struct task_struct *)p, 0, 0);
	if (!tctx)
		dbg_msg("warning: failed to get task context for pid=%d (%s)",
			p->pid, p->comm);
	return tctx;
}

/*
 * Heartbeat timer used to periodically trigger the check to run the user-space
 * scheduler.
 *
 * Without this timer we may starve the scheduler if the system is completely
 * idle and hit the watchdog that would auto-kill this scheduler.
 */
struct usersched_timer {
	struct bpf_timer timer;
};

struct {
	__uint(type, BPF_MAP_TYPE_ARRAY);
	__uint(max_entries, 1);
	__type(key, u32);
	__type(value, struct usersched_timer);
} usersched_timer SEC(".maps");

/*
 * Time period of the scheduler heartbeat, used to periodically kick the
 * user-space scheduler and check if there is any pending activity.
 */
#define USERSCHED_TIMER_NS (NSEC_PER_SEC / 10)

/*
 * Return true if the target task @p is the user-space scheduler.
 */
static inline bool is_usersched_task(const struct task_struct *p)
{
	return p->pid == usersched_pid;
}

/*
 * Return true if the target task @p is a kernel thread.
 */
static inline bool is_kthread(const struct task_struct *p)
{
	return p->flags & PF_KTHREAD;
}

/*
 * Return true if the CPU is allowed by our hardcoded CPU mask (only CPUs 0 and 20).
 */
static inline bool is_cpu_allowed(s32 cpu)
{
	return cpu == 0 || cpu == 20;
}

/*
 * Return true if the target task @p is kswapd.
 */
static inline bool is_kswapd(const struct task_struct *p)
{
        return p->flags & (PF_KSWAPD | PF_KCOMPACTD);
}

/*
 * Return true if the target task @p is khugepaged, false otherwise.
 */
static inline bool is_khugepaged(const struct task_struct *p)
{
	return khugepaged_pid && p->pid == khugepaged_pid;
}

/*
 * Return true if @p still wants to run, false otherwise.
 */
static bool is_queued(const struct task_struct *p)
{
	return p->scx.flags & SCX_TASK_QUEUED;
}

/*
 * Flag used to wake-up the user-space scheduler.
 */
static volatile u32 usersched_needed;

/*
 * Set user-space scheduler wake-up flag (equivalent to an atomic release
 * operation).
 */
static void set_usersched_needed(void)
{
	__sync_fetch_and_or(&usersched_needed, 1);
}

/*
 * Check and clear user-space scheduler wake-up flag (equivalent to an atomic
 * acquire operation).
 */
static bool test_and_clear_usersched_needed(void)
{
	return __sync_fetch_and_and(&usersched_needed, 0) == 1;
}

/*
 * Return true if there's any pending activity to do for the scheduler, false
 * otherwise.
 *
 * NOTE: a task is sent to the user-space scheduler using the "queued"
 * ringbuffer, then the scheduler drains the queued tasks and adds them to
 * its internal data structures / state; at this point tasks become
 * "scheduled" and the user-space scheduler will take care of updating
 * nr_scheduled accordingly; lastly tasks will be dispatched and the
 * user-space scheduler will update nr_scheduled again.
 *
 * Checking nr_scheduled and the available data in the ringbuffer allows to
 * determine if there is still some pending work to do for the scheduler:
 * new tasks have been queued since last check, or there are still tasks
 * "queued" or "scheduled" since the previous user-space scheduler run.
 *
 * If there's no pending action, it is pointless to wake-up the scheduler
 * (even if a CPU becomes idle), because there is nothing to do.
 *
 * Also keep in mind that we don't need any protection here since this code
 * doesn't run concurrently with the user-space scheduler (that is single
 * threaded), therefore this check is also safe from a concurrency perspective.
 */
static bool usersched_has_pending_tasks(void)
{
	if (nr_scheduled)
		return true;

	return bpf_ringbuf_query(&queued, BPF_RB_AVAIL_DATA) > 0;
}

/*
 * Return the DSQ ID associated to a CPU, or SHARED_DSQ if the CPU is not
 * valid.
 */
static u64 cpu_to_dsq(s32 cpu)
{
	if (cpu < 0 || cpu >= MAX_CPUS) {
		scx_bpf_error("Invalid cpu: %d", cpu);
		return SHARED_DSQ;
	}
	return (u64)cpu;
}

/*
 * Update the per-CPU scheduled tasks count by querying the number of tasks
 * in the local DSQ for each CPU.
 */
static void update_scheduled_tasks_per_cpu(void)
{
	s32 cpu;

	bpf_for(cpu, 0, nr_cpu_ids) {
		if (cpu >= MAX_CPUS)
			break;
		/* Only update counters for allowed CPUs */
		if (!is_cpu_allowed(cpu)) {
			nr_scheduled_per_cpu[cpu] = 0;
			continue;
		}
		nr_scheduled_per_cpu[cpu] = scx_bpf_dsq_nr_queued(cpu_to_dsq(cpu));
	}
}

/*
 * Find an idle CPU in the system for the task.
 *
 * NOTE: the idle CPU selection doesn't need to be formally perfect, it is
 * totally fine to accept racy conditions and potentially make mistakes, by
 * picking CPUs that are not idle or even offline, the logic has been designed
 * to handle these mistakes in favor of a more efficient response and a reduced
 * scheduling overhead.
 */
static s32 pick_idle_cpu(const struct task_struct *p, s32 prev_cpu)
{
	/*
	 * For tasks that can run only on a single CPU, we can simply verify if
	 * their only allowed CPU is still idle, but only if it's in our allowed set.
	 */
	if (is_cpu_allowed(prev_cpu) && scx_bpf_test_and_clear_cpu_idle(prev_cpu))
		return prev_cpu;

	return -EBUSY;
}

/*
 * Wake-up a target @cpu for the dispatched task @p. If @cpu can't be used
 * wakeup another valid CPU from our allowed set (0 or 20).
 */
static void kick_task_cpu(const struct task_struct *p, s32 cpu)
{
	/* Enforce our CPU restriction */
	if (!is_cpu_allowed(cpu)) {
		cpu = 0; /* Default to CPU 0 */
	}

	if (!bpf_cpumask_test_cpu(cpu, p->cpus_ptr) || !is_cpu_allowed(cpu)) {
		/*
		 * Kick the target CPU anyway, since it may be locked and
		 * needs to go back to idle to reset its state.
		 */
		if (is_cpu_allowed(cpu))
			scx_bpf_kick_cpu(cpu, SCX_KICK_IDLE);
		return;
	}
	scx_bpf_kick_cpu(cpu, SCX_KICK_IDLE);
}

/*
 * Dispatch a task to a target per-CPU DSQ, waking up the corresponding CPU, if
 * needed.
 * 
 * STRICT CPU AFFINITY: This function will ONLY dispatch to the exact CPU specified.
 * No fallback mechanisms. If the target CPU cannot accept the task, the dispatch
 * will be cancelled to maintain strict CPU affinity.
 */
static void dispatch_task(const struct dispatched_task_ctx *task)
{
	struct task_struct *p;

	/* Ignore entry if the task doesn't exist anymore */
	p = bpf_task_from_pid(task->pid);
	if (!p)
		return;

	/*
	 * Handle RL_CPU_ANY case - dispatch to SHARED_DSQ where any allowed CPU can pick it up.
	 */
	if (task->cpu == RL_CPU_ANY) {
		scx_bpf_dsq_insert_vtime(p, SHARED_DSQ,
					 task->slice_ns, task->vtime, task->flags);
		/* Don't kick any specific CPU for RL_CPU_ANY, let any allowed CPU pick it up */
		goto out_release;
	}

	/*
	 * STRICT CPU VALIDATION: Ensure the target CPU is allowed and valid for this task.
	 * If not, cancel the dispatch entirely to maintain strict CPU affinity.
	 */
	if (!is_cpu_allowed(task->cpu)) {
		dbg_msg("dispatch_task: CPU %d not allowed for pid=%d, cancelling", task->cpu, task->pid);
		scx_bpf_dispatch_cancel();
		__sync_fetch_and_add(&nr_failed_dispatches, 1);
		goto out_release;
	}

	if (!bpf_cpumask_test_cpu(task->cpu, p->cpus_ptr)) {
		dbg_msg("dispatch_task: CPU %d not in cpumask for pid=%d, cancelling", task->cpu, task->pid);
		scx_bpf_dispatch_cancel();
		__sync_fetch_and_add(&nr_failed_dispatches, 1);
		goto out_release;
	}

	/*
	 * Dispatch task to the exact target CPU specified.
	 * STRICT AFFINITY: No fallback mechanisms - task goes exactly where requested.
	 */
	scx_bpf_dsq_insert_vtime(p, cpu_to_dsq(task->cpu),
				 task->slice_ns, task->vtime, task->flags);
	__sync_fetch_and_add(&nr_user_dispatches, 1);
	if (task->cpu >= 0 && task->cpu < MAX_CPUS) {
		__sync_fetch_and_add(&nr_scheduled_per_cpu[task->cpu], 1);
	}

	/* Record which CPU this task was dispatched to for affinity tracking */
	struct task_ctx *tctx = try_lookup_task_ctx(p);
	if (tctx) {
		tctx->dispatched_cpu = task->cpu;
	}

	/* Wake up the exact target CPU */
	scx_bpf_kick_cpu(task->cpu, SCX_KICK_IDLE);

	dbg_msg("dispatch_task: dispatched pid=%d to CPU %d", task->pid, task->cpu);

out_release:
	bpf_task_release(p);
}

/*
 * Return true if the waker commits to release the CPU after waking up @p,
 * false otherwise.
 */
static bool is_wake_sync(u64 wake_flags)
{
	const struct task_struct *current = (void *)bpf_get_current_task_btf();
	return (wake_flags & SCX_WAKE_SYNC) && !(current->flags & PF_EXITING);
}

/*
 * Try to dispatch a task directly on an idle CPU.
 *
 * On success, return the idle CPU where the task has been dispatched, or
 * return a negative value otherwise.
 */
static s32 try_direct_dispatch(struct task_struct *p,
			       s32 prev_cpu, u64 enq_flags, bool *dispatched)
{
	s32 cpu;

	*dispatched = false;

	/*
	 * If built-in idle CPU policy is not enabled completely delegate
	 * the idle selection policy to user-space and keep re-using the
	 * same CPU here.
	 */
	if (!builtin_idle)
		return -EBUSY;

	/*
	 * Pick the idle CPU closest to prev_cpu usable by the task.
	 */
	cpu = pick_idle_cpu(p, prev_cpu);
	if (cpu < 0)
		return cpu;

	/*
	 * Enforce our CPU restriction: only allow CPUs 0 and 20
	 */
	if (!is_cpu_allowed(cpu)) {
		return -EBUSY;
	}

	/*
	 * If we picked an invalid CPU for the task, give up and ignore
	 * direct dispatch.
	 */
	if (!bpf_cpumask_test_cpu(cpu, p->cpus_ptr)) {
		/*
		 * We selected a CPU that the task cannot run on, which
		 * causes it to become idle-locked, so it can't be chosen
		 * by other tasks and may remain unused for a long time.
		 *
		 * To avoid wasting CPU time, we kick the CPU to let it
		 * pull a task from the SHARED_DSQ. If a task is available,
		 * it runs for a slice, then returns idle again, refreshing
		 * its state and becoming eligible again as an idle CPU. If
		 * no task is available, it will continue running the idle
		 * task, refreshing its idle state and rejoining the pool
		 * of idle CPUs.
		 */
		if (is_cpu_allowed(cpu))
			scx_bpf_kick_cpu(cpu, SCX_KICK_IDLE);
		return -EBUSY;
	}

	/*
	 * Perform direct dispatch only if the SHAREQ_DSQ is empty,
	 * otherwise we may risk to starve the tasks waiting in the
	 * SHARED_DSQ.
	 */
	if (!scx_bpf_dsq_nr_queued(SHARED_DSQ) && !scx_bpf_dsq_nr_queued(cpu_to_dsq(cpu))) {
		scx_bpf_dsq_insert_vtime(p, cpu_to_dsq(cpu),
					 SCX_SLICE_DFL, p->scx.dsq_vtime, enq_flags);
		__sync_fetch_and_add(&nr_kernel_dispatches, 1);
		// Atomically increment the counter with a bounds check for the verifier
		if (cpu >= 0 && cpu < MAX_CPUS) {
			__sync_fetch_and_add(&nr_scheduled_per_cpu[cpu], 1);
		}
		*dispatched = true;
	}

	return cpu;
}

s32 BPF_STRUCT_OPS(rustland_select_cpu, struct task_struct *p, s32 prev_cpu,
		   u64 wake_flags)
{
	bool dispatched = false;
	s32 cpu;

	/*
	 * Scheduler is dispatched directly in .dispatch() when needed, so
	 * we can skip it here.
	 */
	if (is_usersched_task(p))
		return prev_cpu;

	/* Enforce our CPU restriction */
	if (!is_cpu_allowed(prev_cpu)) {
		prev_cpu = 0; /* Default to CPU 0 */
	}

	cpu = try_direct_dispatch(p, prev_cpu, 0, &dispatched);
	if (cpu >= 0 && is_cpu_allowed(cpu))
		return cpu;

	/*
	 * If we couldn't find an allowed idle CPU, return an allowed CPU.
	 * In case of a sync wakeup prioritize the waker's CPU if allowed.
	 */
	s32 waker_cpu = bpf_get_smp_processor_id();
	if (is_wake_sync(wake_flags) && is_cpu_allowed(waker_cpu)) {
		return waker_cpu;
	} else if (is_cpu_allowed(prev_cpu)) {
		return prev_cpu;
	} else {
		return 0; /* Default to CPU 0 */
	}
}

/*
 * Select and wake-up an idle CPU for a specific task from the user-space
 * scheduler.
 */
SEC("syscall")
int rs_select_cpu(struct task_cpu_arg *input)
{
	struct task_struct *p;
	int cpu;

	p = bpf_task_from_pid(input->pid);
	if (!p)
		return -EINVAL;

	bpf_rcu_read_lock();
	cpu = pick_idle_cpu(p, input->cpu);
	bpf_rcu_read_unlock();

	bpf_task_release(p);

	return cpu;
}

/*
 * Fill @task with all the information that need to be sent to the user-space
 * scheduler.
 */
static void get_task_info(struct queued_task_ctx *task,
			  const struct task_struct *p, u64 enq_flags)
{
	struct task_ctx *tctx = try_lookup_task_ctx(p);

	task->pid = p->pid;
	task->cpu = scx_bpf_task_cpu(p);
	task->nr_cpus_allowed = p->nr_cpus_allowed;
	task->flags = enq_flags;
	task->start_ts = tctx ? tctx->start_ts : 0;
	task->stop_ts = tctx ? tctx->stop_ts : 0;
	task->exec_runtime = tctx ? tctx->exec_runtime : 0;
	task->weight = p->scx.weight;
	task->vtime = p->scx.dsq_vtime;
}

/*
 * User-space scheduler is congested: log that and increment congested counter.
 */
static void sched_congested(struct task_struct *p)
{
	dbg_msg("congested: pid=%d (%s)", p->pid, p->comm);
	__sync_fetch_and_add(&nr_sched_congested, 1);
}

/*
 * Return true if a task has been enqueued as a remote wakeup, false
 * otherwise.
 */
static bool is_queued_wakeup(const struct task_struct *p, u64 enq_flags)
{
	return !__COMPAT_is_enq_cpu_selected(enq_flags) && !scx_bpf_task_running(p);
}

/*
 * Task @p becomes ready to run. We can dispatch the task directly here if the
 * user-space scheduler is not required, or enqueue it to be processed by the
 * scheduler.
 */
void BPF_STRUCT_OPS(rustland_enqueue, struct task_struct *p, u64 enq_flags)
{
	struct queued_task_ctx *task;
	s32 cpu = -EBUSY;

	/*
	 * Scheduler is dispatched directly in .dispatch() when needed, so
	 * we can skip it here.
	 */
	if (is_usersched_task(p))
		return;

	/*
	 * Always dispatch per-CPU kthreads directly on their target CPU.
	 *
	 * This allows to prioritize critical kernel threads that may
	 * potentially stall the entire system if they are blocked for too long
	 * (i.e., ksoftirqd/N, rcuop/N, etc.).
	 * 
	 * However, enforce our CPU restriction: only allow CPUs 0 and 20.
	 */
	if ((is_kthread(p) && p->nr_cpus_allowed == 1) || is_kswapd(p) || is_khugepaged(p)) {
		cpu = scx_bpf_task_cpu(p);
		
		/* Enforce CPU restriction for kernel threads */
		if (!is_cpu_allowed(cpu)) {
			/* Redirect to an allowed CPU, prefer CPU 0 */
			cpu = 0;
		}
		
                scx_bpf_dsq_insert_vtime(p, cpu_to_dsq(cpu),
					 SCX_SLICE_DFL, p->scx.dsq_vtime, enq_flags);
		__sync_fetch_and_add(&nr_kernel_dispatches, 1);
		if (cpu >= 0 && cpu < MAX_CPUS) {
			__sync_fetch_and_add(&nr_scheduled_per_cpu[cpu], 1);
		}
		return;
	}

	/*
	 * DISABLE direct dispatch to ensure ALL tasks go through user-space scheduler.
	 * This prevents any bypass that could cause CPU migration.
	 */
	/* Direct dispatch disabled for strict CPU affinity
	if (builtin_idle && is_queued_wakeup(p, enq_flags)) {
		bool dispatched = false;

		cpu = try_direct_dispatch(p, scx_bpf_task_cpu(p), enq_flags, &dispatched);
		if (dispatched) {
			scx_bpf_kick_cpu(cpu, SCX_KICK_IDLE);
			return;
		}
	}
	*/

	/*
	 * Add tasks to the @queued list, they will be processed by the
	 * user-space scheduler.
	 *
	 * If @queued list is full (user-space scheduler is congested) tasks
	 * will be dispatched directly from the kernel (using the first CPU
	 * available in this case).
	 */
	task = bpf_ringbuf_reserve(&queued, sizeof(*task), 0);
	if (!task) {
		sched_congested(p);
		scx_bpf_dsq_insert_vtime(p, SHARED_DSQ, SCX_SLICE_DFL, p->scx.dsq_vtime, enq_flags);
		__sync_fetch_and_add(&nr_kernel_dispatches, 1);
		goto out_kick;
	}
	get_task_info(task, p, enq_flags);
	dbg_msg("enqueue: pid=%d (%s)", p->pid, p->comm);
	bpf_ringbuf_submit(task, 0);

	__sync_fetch_and_add(&nr_queued, 1);

out_kick:
	if (cpu < 0)
		cpu = scx_bpf_task_cpu(p);
	kick_task_cpu(p, cpu);
}

/*
 * Dispatch the user-space scheduler.
 */
static void dispatch_user_scheduler(void)
{
	struct task_struct *p;

	p = bpf_task_from_pid(usersched_pid);
	if (!p) {
		scx_bpf_error("Failed to find usersched task %d", usersched_pid);
		return;
	}

	/*
	 * Assign an infinite time slice to the user-space scheduler, so
	 * that it can completely drain all the pending tasks.
	 *
	 * The user-space scheduler will voluntarily yield the CPU upon
	 * completion through BpfScheduler->notify_complete().
	 */
	scx_bpf_dsq_insert(p, SCHED_DSQ, SCX_SLICE_INF, 0);

	bpf_task_release(p);
}

/*
 * Handle a task dispatched from user-space, performing the actual low-level
 * BPF dispatch.
 */
static long handle_dispatched_task(struct bpf_dynptr *dynptr, void *context)
{
	const struct dispatched_task_ctx *task;

	task = bpf_dynptr_data(dynptr, 0, sizeof(*task));
	if (!task)
		return 0;

	dispatch_task(task);

	return !!scx_bpf_dispatch_nr_slots();
}

/*
 * Dispatch tasks that are ready to run.
 *
 * This function is called when a CPU's local DSQ is empty and ready to accept
 * new dispatched tasks.
 *
 * STRICT CPU AFFINITY: Each CPU only consumes tasks from its own per-CPU DSQ.
 * Tasks dispatched to specific CPUs will ONLY run on those CPUs.
 */
void BPF_STRUCT_OPS(rustland_dispatch, s32 cpu, struct task_struct *prev)
{
	/*
	 * Only dispatch on allowed CPUs (0 and 20). For disallowed CPUs,
	 * just return and let them stay idle.
	 */
	if (!is_cpu_allowed(cpu))
		return;

	/*
	 * Fire up the user-space scheduler: it will run only if no other
	 * task needs to run.
	 */
	if (test_and_clear_usersched_needed())
		dispatch_user_scheduler();

	/*
	 * Consume all tasks from the @dispatched list and immediately
	 * dispatch them on the target CPU decided by the user-space
	 * scheduler.
	 */
	bpf_user_ringbuf_drain(&dispatched, handle_dispatched_task, NULL, BPF_RB_NO_WAKEUP);

	/*
	 * STRICT AFFINITY: Only consume tasks from this CPU's own DSQ.
	 * This ensures tasks dispatched to specific CPUs stay on those CPUs.
	 */
	if (scx_bpf_dsq_move_to_local(cpu_to_dsq(cpu))) {
		if (cpu >= 0 && cpu < MAX_CPUS) {
			__sync_fetch_and_sub(&nr_scheduled_per_cpu[cpu], 1);
		}
		return;
	}

	/*
	 * Only consume from the shared DSQ if dispatched with RL_CPU_ANY.
	 * This allows flexibility for tasks that don't require strict CPU affinity.
	 */
	if (scx_bpf_dsq_move_to_local(SHARED_DSQ))
		return;

		
	/*
	 * Lastly, consume and dispatch the user-space scheduler.
	 */
	if (scx_bpf_dsq_move_to_local(SCHED_DSQ))
		return;

	/*
	 * If there are still pending task, notify the user-space scheduler
	 * and prevent the CPU from going idle.
	 */
	if (usersched_has_pending_tasks()) {
		set_usersched_needed();
		scx_bpf_kick_cpu(cpu, SCX_KICK_IDLE);
		return;
	}

	// Also periodically ensure scheduled tasks is up to date, prevent drifting
	update_scheduled_tasks_per_cpu();

	/*
	 * If the current task expired its time slice and no other task
	 * wants to run, simply replenish its time slice and let it run for
	 * another round on the same CPU.
	 */
	// if (prev && is_queued(prev) && !is_usersched_task(prev))
	// 	prev->scx.slice = SCX_SLICE_DFL;
}

void BPF_STRUCT_OPS(rustland_runnable, struct task_struct *p, u64 enq_flags)
{
	struct task_ctx *tctx;

	if (is_usersched_task(p))
		return;

	tctx = try_lookup_task_ctx(p);
	if (!tctx)
		return;

	tctx->exec_runtime = 0;
}

/*
 * Task @p starts on its selected CPU (update CPU ownership map).
 */
void BPF_STRUCT_OPS(rustland_running, struct task_struct *p)
{
	s32 cpu = scx_bpf_task_cpu(p);
	struct task_ctx *tctx;

	if (is_usersched_task(p)) {
		usersched_last_run_at = scx_bpf_now();
		return;
	}

	/*
	 * Mark the CPU as busy by setting the pid as owner (ignoring the
	 * user-space scheduler).
	 */
	__sync_fetch_and_add(&nr_running, 1);

	tctx = try_lookup_task_ctx(p);
	if (!tctx)
		return;
	tctx->start_ts = scx_bpf_now();
}

/*
 * Task @p stops running on its associated CPU (update CPU ownership map).
 */
void BPF_STRUCT_OPS(rustland_stopping, struct task_struct *p, bool runnable)
{
	u64 now = scx_bpf_now();
	s32 cpu = scx_bpf_task_cpu(p);
	struct task_ctx *tctx;

	if (is_usersched_task(p))
		return;

	dbg_msg("stop: pid=%d (%s) cpu=%ld", p->pid, p->comm, cpu);

	__sync_fetch_and_sub(&nr_running, 1);

	tctx = try_lookup_task_ctx(p);
	if (!tctx)
		return;
	tctx->stop_ts = now;

	/*
	 * Update the partial execution time since last sleep.
	 */
	tctx->exec_runtime += now - tctx->start_ts;
}

/*
 * A CPU is taken away from the scheduler, preempting the current task by
 * another one running in a higher priority sched_class.
 */
void BPF_STRUCT_OPS(rustland_cpu_release, s32 cpu,
				struct scx_cpu_release_args *args)
{
	struct task_struct *p = args->task;
	/*
	 * If the interrupted task is the user-space scheduler make sure to
	 * re-schedule it immediately.
	 */
	dbg_msg("cpu preemption: pid=%d (%s)", p->pid, p->comm);
	if (is_usersched_task(p))
		set_usersched_needed();
}

/*
 * A task joins the sched_ext scheduler.
 */
void BPF_STRUCT_OPS(rustland_enable, struct task_struct *p)
{
	p->scx.dsq_vtime = 0;
	p->scx.slice = SCX_SLICE_DFL;
}

/*
 * A new task @p is being created.
 *
 * Allocate and initialize all the internal structures for the task (this
 * function is allowed to block, so it can be used to preallocate memory).
 */
s32 BPF_STRUCT_OPS(rustland_init_task, struct task_struct *p,
		   struct scx_init_task_args *args)
{
	struct task_ctx *tctx;
	struct bpf_cpumask *cpumask;

	tctx = bpf_task_storage_get(&task_ctx_stor, p, 0,
				    BPF_LOCAL_STORAGE_GET_F_CREATE);
	if (!tctx)
		return -ENOMEM;

	/* Initialize dispatched_cpu to -1 (no specific CPU assigned yet) */
	tctx->dispatched_cpu = -1;

	/*
	 * Create task's L2 cache cpumask.
	 */
	cpumask = bpf_cpumask_create();
	if (!cpumask)
		return -ENOMEM;
	cpumask = bpf_kptr_xchg(&tctx->l2_cpumask, cpumask);
	if (cpumask)
		bpf_cpumask_release(cpumask);

	/*
	 * Create task's L3 cache cpumask.
	 */
	cpumask = bpf_cpumask_create();
	if (!cpumask)
		return -ENOMEM;
	cpumask = bpf_kptr_xchg(&tctx->l3_cpumask, cpumask);
	if (cpumask)
		bpf_cpumask_release(cpumask);

	return 0;
}

/*
 * Heartbeat scheduler timer callback.
 *
 * If the system is completely idle the sched-ext watchdog may incorrectly
 * detect that as a stall and automatically disable the scheduler. So, use this
 * timer to periodically wake-up the scheduler and avoid long inactivity.
 *
 * This can also help to prevent real "stalling" conditions in the scheduler.
 */
static int usersched_timer_fn(void *map, int *key, struct bpf_timer *timer)
{
	struct task_struct *p;
	int err = 0;

	/*
	 * Trigger the user-space scheduler if it has been inactive for
	 * more than USERSCHED_TIMER_NS.
	 */
	if (time_delta(scx_bpf_now(), usersched_last_run_at) >= USERSCHED_TIMER_NS) {
		bpf_rcu_read_lock();
		p = bpf_task_from_pid(usersched_pid);
		if (p) {
			s32 cpu;

			set_usersched_needed();
			cpu = scx_bpf_pick_idle_cpu(p->cpus_ptr, 0);
			if (cpu >= 0 && is_cpu_allowed(cpu))
				scx_bpf_kick_cpu(cpu, SCX_KICK_IDLE);
			else {
				/* Fallback to our allowed CPU */
				scx_bpf_kick_cpu(0, SCX_KICK_IDLE);
			}
			bpf_task_release(p);
		}
		bpf_rcu_read_unlock();
	}

	/* Re-arm the timer */
	err = bpf_timer_start(timer, USERSCHED_TIMER_NS, 0);
	if (err)
		scx_bpf_error("Failed to arm stats timer");

	return 0;
}

/*
 * Initialize the heartbeat scheduler timer.
 */
static int usersched_timer_init(void)
{
	struct bpf_timer *timer;
	u32 key = 0;
	int err;

	timer = bpf_map_lookup_elem(&usersched_timer, &key);
	if (!timer) {
		scx_bpf_error("Failed to lookup scheduler timer");
		return -ESRCH;
	}
	bpf_timer_init(timer, &usersched_timer, CLOCK_BOOTTIME);
	bpf_timer_set_callback(timer, usersched_timer_fn);
	err = bpf_timer_start(timer, USERSCHED_TIMER_NS, 0);
	if (err)
		scx_bpf_error("Failed to arm scheduler timer");

	return err;
}

/*
 * Evaluate the amount of online CPUs.
 */
static s32 get_nr_online_cpus(void)
{
	const struct cpumask *online_cpumask;
	int i, cpus = 0;

	online_cpumask = scx_bpf_get_online_cpumask();

	bpf_for(i, 0, nr_cpu_ids) {
		if (!bpf_cpumask_test_cpu(i, online_cpumask))
			continue;
		/* Only count allowed CPUs */
		if (!is_cpu_allowed(i))
			continue;
		cpus++;
	}

	scx_bpf_put_cpumask(online_cpumask);

	return cpus;
}

/*
 * Create a DSQ for each CPU available in the system and a global shared DSQ.
 *
 * All the tasks processed by the user-space scheduler can be dispatched either
 * to a specific CPU/DSQ or to the first CPU available (SHARED_DSQ).
 *
 * Custom DSQs are then consumed from the .dispatch() callback, that will
 * transfer all the enqueued tasks to the consuming CPU's local DSQ.
 */
static int dsq_init(void)
{
	int err;
	s32 cpu;

	/* Initialize amount of online CPUs */
	nr_online_cpus = get_nr_online_cpus();

	/* Create per-CPU DSQs only for allowed CPUs (0 and 20) */
	bpf_for(cpu, 0, nr_cpu_ids) {
		/* Only create DSQs for our allowed CPUs */
		if (!is_cpu_allowed(cpu))
			continue;
			
		err = scx_bpf_create_dsq(cpu_to_dsq(cpu), -1);
		if (err) {
			scx_bpf_error("failed to create pcpu DSQ %d: %d",
				      cpu, err);
			return err;
		}
	}

	/* Create the global shared DSQ */
	err = scx_bpf_create_dsq(SHARED_DSQ, -1);
	if (err) {
		scx_bpf_error("failed to create shared DSQ: %d", err);
		return err;
	}

	/* Create the scheduler's DSQ */
	err = scx_bpf_create_dsq(SCHED_DSQ, -1);
	if (err) {
		scx_bpf_error("failed to create scheduler DSQ: %d", err);
		return err;
	}

	return 0;
}

static int init_cpumask(struct bpf_cpumask **cpumask)
{
	struct bpf_cpumask *mask;
	int err = 0;

	/*
	 * Do nothing if the mask is already initialized.
	 */
	mask = *cpumask;
	if (mask)
		return 0;
	/*
	 * Create the CPU mask.
	 */
	err = calloc_cpumask(cpumask);
	if (!err)
		mask = *cpumask;
	if (!mask)
		err = -ENOMEM;

	return err;
}

SEC("syscall")
int enable_sibling_cpu(struct domain_arg *input)
{
	struct cpu_ctx *cctx;
	struct bpf_cpumask *mask, **pmask;
	int err = 0;

	cctx = try_lookup_cpu_ctx(input->cpu_id);
	if (!cctx)
		return -ENOENT;

	/* Make sure the target CPU mask is initialized */
	switch (input->lvl_id) {
	case 2:
		pmask = &cctx->l2_cpumask;
		break;
	case 3:
		pmask = &cctx->l3_cpumask;
		break;
	default:
		return -EINVAL;
	}
	err = init_cpumask(pmask);
	if (err)
		return err;

	bpf_rcu_read_lock();
	mask = *pmask;
	if (mask)
		bpf_cpumask_set_cpu(input->sibling_cpu_id, mask);
	bpf_rcu_read_unlock();

	return err;
}

/*
 * Initialize the scheduling class.
 */
s32 BPF_STRUCT_OPS_SLEEPABLE(rustland_init)
{
	int err;

	/* Compile-time checks */
	BUILD_BUG_ON((MAX_CPUS % 2));

	/* Initialize maximum possible CPU number */
	nr_cpu_ids = scx_bpf_nr_cpu_ids();

	/* Initialize rustland core */
	err = dsq_init();
	if (err)
		return err;
	err = usersched_timer_init();
	if (err)
		return err;

	/* Initialize per-CPU scheduled tasks count */
	update_scheduled_tasks_per_cpu();

	return 0;
}

/*
 * Unregister the scheduling class.
 */
void BPF_STRUCT_OPS(rustland_exit, struct scx_exit_info *ei)
{
	UEI_RECORD(uei, ei);
}

/*
 * Scheduling class declaration.
 */
SCX_OPS_DEFINE(rustland,
	       .select_cpu		= (void *)rustland_select_cpu,
	       .enqueue			= (void *)rustland_enqueue,
	       .dispatch		= (void *)rustland_dispatch,
	       .runnable		= (void *)rustland_runnable,
	       .running			= (void *)rustland_running,
	       .stopping		= (void *)rustland_stopping,
	       .cpu_release		= (void *)rustland_cpu_release,
	       .enable			= (void *)rustland_enable,
	       .init_task		= (void *)rustland_init_task,
	       .init			= (void *)rustland_init,
	       .exit			= (void *)rustland_exit,
	       .flags			= SCX_OPS_ENQ_LAST,
	       .timeout_ms		= 5000,
	       .dispatch_max_batch	= MAX_DISPATCH_SLOT,
	       .name			= "rustland");
