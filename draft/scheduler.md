# Linux Scheduler Queues and Timer Interrupts Explained

This document provides an overview of the queue structures in the Linux kernel, focusing on the scheduler's runqueues, wait queues, and how timer interrupts drive scheduling decisions.

## Table of Contents
- [Overview: Types of Queues](#overview-types-of-queues)
- [Runqueues: The Scheduler's Core](#runqueues-the-schedulers-core)
- [Wait Queues: Blocking Synchronization](#wait-queues-blocking-synchronization)
- [Other Important Queues](#other-important-queues)
- [Timer Interrupts and Scheduling](#timer-interrupts-and-scheduling)
- [How It All Fits Together](#how-it-all-fits-together)
- [Visual Overview](#visual-overview)
- [Key Takeaways](#key-takeaways)

---

## Overview: Types of Queues

The Linux kernel uses **many different types of queues** for different purposes:

| Queue Type | Count | Purpose | Managed By |
|------------|-------|---------|------------|
| **Runqueues** | One per CPU | Tasks ready to run | Scheduler |
| **Wait Queues** | Thousands+ | Tasks waiting for events | Resource owners |
| **Work Queues** | Several | Deferred work execution | Workqueue subsystem |
| **IO Queues** | Per device | Pending I/O requests | Block layer |
| **Network Queues** | Per interface | Packets to send/receive | Network stack |
| **Timer Queues** | Per CPU | Pending timers | Timer subsystem |

This document focuses on **runqueues** and **wait queues** as they're central to understanding blocking I/O.

---

## Runqueues: The Scheduler's Core

### How Many Runqueues?

**Answer**: **One runqueue per CPU**

```c
// kernel/sched/core.c:120
DEFINE_PER_CPU_SHARED_ALIGNED(struct rq, runqueues);
```

On a **4-core system**: 4 runqueues
On a **64-core system**: 64 runqueues
On a **256-core system**: 256 runqueues

Each CPU has its **own independent runqueue** to avoid lock contention.

### The Runqueue Structure

**Location**: `kernel/sched/sched.h:1103-1280`

```c
/*
 * This is the main, per-CPU runqueue data structure.
 */
struct rq {
    /* Runqueue lock */
    raw_spinlock_t __lock;

    /* Number of runnable tasks */
    unsigned int nr_running;

    /* Per-scheduling-class runqueues */
    struct cfs_rq  cfs;   // CFS (normal tasks)
    struct rt_rq   rt;    // Real-time tasks
    struct dl_rq   dl;    // Deadline tasks

    /* Currently running task */
    struct task_struct *curr;

    /* Idle task for this CPU */
    struct task_struct *idle;

    /* CPU this runqueue belongs to */
    int cpu;

    /* Clock for this runqueue */
    u64 clock;

    /* Load balancing */
    unsigned long cpu_capacity;
    struct root_domain *rd;

    /* Number of tasks in uninterruptible sleep */
    unsigned int nr_uninterruptible;

    /* I/O wait accounting */
    atomic_t nr_iowait;

    /* ... many more fields ... */
};
```

### Multiple Sub-Runqueues per CPU

Each CPU's runqueue contains **multiple sub-runqueues**, one for each **scheduling class**:

```
CPU 0 Runqueue
├── CFS Runqueue (SCHED_NORMAL, SCHED_BATCH)
│   └── Red-black tree of tasks sorted by vruntime
├── RT Runqueue (SCHED_FIFO, SCHED_RR)
│   └── Priority arrays (0-99)
└── Deadline Runqueue (SCHED_DEADLINE)
    └── Red-black tree sorted by deadline

CPU 1 Runqueue
├── CFS Runqueue
├── RT Runqueue
└── Deadline Runqueue

... (one per CPU)
```

### Scheduling Classes (Priority Order)

Tasks are picked in this order:

1. **Stop** (`stop_sched_class`) - Highest priority, per-CPU tasks
2. **Deadline** (`dl_sched_class`) - SCHED_DEADLINE real-time
3. **Real-Time** (`rt_sched_class`) - SCHED_FIFO, SCHED_RR
4. **CFS** (`fair_sched_class`) - SCHED_NORMAL, SCHED_BATCH (most tasks)
5. **Idle** (`idle_sched_class`) - Runs when nothing else to do

### CFS Runqueue: The Common Case

Most tasks use the **Completely Fair Scheduler (CFS)**:

```c
struct cfs_rq {
    /* Load (weight) of all tasks */
    struct load_weight load;

    /* Number of runnable tasks */
    unsigned int nr_running;

    /* Red-black tree of runnable tasks */
    struct rb_root_cached tasks_timeline;

    /* Leftmost (earliest vruntime) task */
    struct rb_node *rb_leftmost;

    /* Minimum vruntime (virtual runtime) */
    u64 min_vruntime;

    /* Currently running task */
    struct sched_entity *curr;

    /* ... */
};
```

**Data structure**: Red-black tree ordered by **vruntime** (virtual runtime)

- **vruntime** = virtual time a task has run
- **Lower vruntime** = task hasn't run much, should run next
- **Higher vruntime** = task has run a lot, should wait

**Pick next task**: Leftmost node in the red-black tree (O(1) operation)

### Accessing the Runqueue

```c
// Get current CPU's runqueue
struct rq *rq = this_rq();

// Get specific CPU's runqueue
struct rq *rq = cpu_rq(cpu_id);

// Get runqueue for a specific task
struct rq *rq = task_rq(task);
```

**Location**: `kernel/sched/core.c:120`

---

## Wait Queues: Blocking Synchronization

### How Many Wait Queues?

**Answer**: **Thousands to millions**, dynamically created

Wait queues are **NOT centralized**. They are:

1. **Embedded in kernel objects** (inodes, sockets, devices)
2. **Allocated dynamically** when needed
3. **Hash tables** for shared resources (e.g., page cache uses 256 queues)

### Examples of Wait Queues

#### 1. Page Cache Wait Queues (Hash Table)

```c
// mm/filemap.c:1071-1078
#define PAGE_WAIT_TABLE_SIZE 256

static wait_queue_head_t folio_wait_table[PAGE_WAIT_TABLE_SIZE];

static wait_queue_head_t *folio_waitqueue(struct folio *folio)
{
    return &folio_wait_table[hash_ptr(folio, PAGE_WAIT_TABLE_BITS)];
}
```

**Count**: 256 wait queues (shared via hashing)
**Purpose**: Threads waiting for pages to be unlocked/loaded

#### 2. Per-Object Wait Queues

Many kernel objects have embedded wait queues:

```c
// Task exiting
struct task_struct {
    wait_queue_head_t wait_chldexit;  // For wait() syscall
};

// Socket events
struct socket {
    wait_queue_head_t wait;  // For poll/select/epoll
};

// Device operations
struct tty_struct {
    wait_queue_head_t read_wait;
    wait_queue_head_t write_wait;
};

// Pipe operations
struct pipe_inode_info {
    wait_queue_head_t rd_wait;
    wait_queue_head_t wr_wait;
};

// File lease events
struct file_lock {
    wait_queue_head_t fl_wait;
};
```

**Count**: As many as there are objects (potentially millions)

#### 3. Global/Subsystem Wait Queues

Some wait queues are static and global:

```c
// PCI configuration access
DECLARE_WAIT_QUEUE_HEAD(pci_cfg_wait);

// Driver probing
DECLARE_WAIT_QUEUE_HEAD(probe_waitqueue);

// VGA arbitration
DECLARE_WAIT_QUEUE_HEAD(vga_wait_queue);

// Balloon driver
DECLARE_WAIT_QUEUE_HEAD(balloon_wq);
```

**Count**: Dozens to hundreds
**Purpose**: System-wide events

### Wait Queue Structure

```c
// include/linux/wait.h:35-38
struct wait_queue_head {
    spinlock_t      lock;   // Protects the list
    struct list_head head;  // Linked list of waiters
};

// Each waiter in the list:
struct wait_queue_entry {
    unsigned int        flags;      // WQ_FLAG_WOKEN, etc.
    void               *private;    // Points to task_struct
    wait_queue_func_t   func;       // Function to call when waking
    struct list_head    entry;      // List linkage
};
```

### Wait Queue Operations

```c
// Initialize a wait queue
DECLARE_WAIT_QUEUE_HEAD(my_wq);
// or
wait_queue_head_t my_wq;
init_waitqueue_head(&my_wq);

// Add to wait queue and sleep
wait_event(my_wq, condition);                    // Uninterruptible
wait_event_interruptible(my_wq, condition);      // Interruptible
wait_event_timeout(my_wq, condition, timeout);   // With timeout

// Wake up waiters
wake_up(&my_wq);                    // Wake one (or all non-exclusive)
wake_up_all(&my_wq);                // Wake all
wake_up_interruptible(&my_wq);      // Wake interruptible waiters
```

### Total Wait Queue Count

**Approximate count** on a busy system:

- **Page cache**: 256 (hash table)
- **Sockets**: 1,000 - 100,000+ (depends on connections)
- **Pipes/FIFOs**: 100 - 10,000+
- **TTYs**: 10 - 100
- **Block devices**: 10 - 1,000
- **Files with locks**: Variable
- **Global/subsystem**: ~100

**Total**: **Thousands to hundreds of thousands** of wait queues

---

## Other Important Queues

### 1. Work Queues

**Purpose**: Deferred work execution in process context

```c
struct workqueue_struct *system_wq;        // System workqueue
struct workqueue_struct *my_wq;            // Custom workqueue

DECLARE_WORK(my_work, my_work_func);
queue_work(system_wq, &my_work);           // Queue work
```

**Count**: Dozens (one per subsystem that needs deferred work)

### 2. I/O Scheduler Queues

**Purpose**: Pending I/O requests to block devices

```c
struct request_queue {
    struct list_head queue_head;           // Pending requests
    struct elevator_queue *elevator;       // I/O scheduler
    // ...
};
```

**Count**: One per block device (disks, SSDs, etc.)

### 3. Network Transmit Queues

**Purpose**: Packets waiting to be sent

```c
struct net_device {
    struct netdev_queue *_tx;              // TX queues
    unsigned int num_tx_queues;            // Number of TX queues
    // ...
};
```

**Count**: Multiple per network interface (multi-queue)

### 4. Timer Queues

**Purpose**: Pending timers (timeouts, periodic events)

```c
// Per-CPU timer wheel
struct timer_base {
    raw_spinlock_t lock;
    struct timer_list *running_timer;
    unsigned long clk;
    // Timer wheel buckets
};
```

**Count**: One per CPU (for low-resolution timers)

### 5. High-Resolution Timer Queues

```c
struct hrtimer_cpu_base {
    struct hrtimer_clock_base clock_base[HRTIMER_MAX_CLOCK_BASES];
    // Red-black tree of timers
};
```

**Count**: One per CPU

---

## Timer Interrupts and Scheduling

### The Timer Tick

The kernel receives **periodic timer interrupts** (the "tick"):

**Frequency**: Configurable, typically:
- **HZ=100**: 100 interrupts/second (10ms period) - servers
- **HZ=250**: 250 interrupts/second (4ms period) - default
- **HZ=1000**: 1000 interrupts/second (1ms period) - desktop

**Configuration**: `CONFIG_HZ` in kernel config

### What Happens on Each Tick

**Location**: `kernel/sched/core.c:5634-5685`

```c
/*
 * This function gets called by the timer code, with HZ frequency.
 * We call it with interrupts disabled.
 */
void sched_tick(void)
{
    int cpu = smp_processor_id();
    struct rq *rq = cpu_rq(cpu);           // Get this CPU's runqueue
    struct task_struct *curr = rq->curr;    // Current task
    struct rq_flags rf;

    // Update scheduler clock
    rq_lock(rq, &rf);
    update_rq_clock(rq);

    // Let scheduling class handle the tick
    curr->sched_class->task_tick(rq, curr, 0);

    // Update load statistics
    calc_global_load_tick(rq);

    // Check if we need to rebalance (move tasks between CPUs)
    sched_balance_trigger(rq);

    rq_unlock(rq, &rf);

    // Update performance counters
    perf_event_task_tick();
}
```

### Scheduling Class Tick Handlers

Each scheduling class has a `task_tick()` function:

#### CFS (Normal Tasks)

```c
// kernel/sched/fair.c
static void task_tick_fair(struct rq *rq, struct task_struct *curr, int queued)
{
    struct cfs_rq *cfs_rq;
    struct sched_entity *se = &curr->se;

    // Update runtime statistics
    update_curr(cfs_rq);

    // Check if task has used its timeslice
    if (cfs_rq->nr_running > 1) {
        // Multiple tasks, check preemption
        check_preempt_tick(cfs_rq, curr);
    }

    // Update task's vruntime
    // If vruntime is too high → set TIF_NEED_RESCHED
}
```

**Key actions**:
1. Update how long the task has run
2. Increase task's vruntime
3. Check if task should be preempted (vruntime too high)
4. If yes, set `TIF_NEED_RESCHED` flag

#### Real-Time Tasks

```c
// kernel/sched/rt.c
static void task_tick_rt(struct rq *rq, struct task_struct *p, int queued)
{
    struct sched_rt_entity *rt_se = &p->rt;

    update_curr_rt(rq);

    // For SCHED_RR: decrement timeslice
    if (p->policy == SCHED_RR) {
        if (--p->rt.time_slice)
            return;

        // Timeslice expired, move to end of queue
        p->rt.time_slice = sched_rr_timeslice;
        requeue_task_rt(rq, p, 0);
        resched_curr(rq);
    }

    // SCHED_FIFO: runs until it blocks or yields
}
```

### The TIF_NEED_RESCHED Flag

When a task should be preempted:

```c
// Set the flag
set_tsk_need_resched(task);
set_preempt_need_resched();

// Later, when returning to userspace or enabling preemption:
if (need_resched()) {
    schedule();  // Call the scheduler
}
```

**Check points**:
1. **Return from interrupt** (including timer interrupt)
2. **Return from syscall**
3. **Enabling preemption** (preempt_enable())
4. **Explicitly** (cond_resched())

### Timer Interrupt Flow

```
HARDWARE
  ↓
[Timer interrupt fires every 1/HZ seconds]
  ↓
CPU jumps to interrupt handler
  ↓
KERNEL (arch-specific interrupt entry)
  ↓
do_IRQ() or equivalent
  ↓
TIMER SUBSYSTEM
  ↓
tick_periodic() or tick_sched_timer()
  ↓
update_process_times()
  ↓
SCHEDULER
  ↓
sched_tick()                              ← kernel/sched/core.c:5634
  ↓
├─ update_rq_clock(rq)                    Update runqueue clock
├─ curr->sched_class->task_tick(...)      Call scheduling class handler
│    ↓
│    For CFS: task_tick_fair()
│    ├─ update_curr(cfs_rq)               Update vruntime
│    ├─ check_preempt_tick(cfs_rq, curr)  Should we preempt?
│    └─ if (should_preempt)
│          set_tsk_need_resched(curr)     ← SET FLAG
│
├─ calc_global_load_tick(rq)              Update load average
└─ sched_balance_trigger(rq)              Trigger load balancing
  ↓
RETURN FROM INTERRUPT
  ↓
Check TIF_NEED_RESCHED flag
  ↓
if (need_resched()) {
    preempt_schedule_irq()                ← Might call schedule()
      ↓
    schedule()                            ← THE SCHEDULER
      ↓
    pick_next_task(rq, prev)              Pick highest priority task
      ↓
    context_switch(prev, next)            Switch to next task
}
  ↓
Return to userspace or continue kernel code
```

### Tickless Mode (NO_HZ)

Modern kernels support **dynamic ticks** (tickless mode):

```c
CONFIG_NO_HZ_IDLE     // Stop tick when CPU idle
CONFIG_NO_HZ_FULL     // Stop tick even when CPU busy (single task)
```

**Benefits**:
- Lower power consumption (fewer interrupts)
- Better performance (less overhead)
- Longer battery life on laptops

**When enabled**:
- Timer interrupt stops when CPU idle
- Or stops when only one task running (NO_HZ_FULL)
- Interrupts restart when needed (I/O event, new task, etc.)

---

## How It All Fits Together

### Task State Transitions

```
┌─────────────────────────────────────────────────────────────────┐
│                    TASK STATE MACHINE                           │
└─────────────────────────────────────────────────────────────────┘

   TASK_RUNNING                    On runqueue, ready to run
        │                          (or currently running)
        │
        ├──[scheduler]──────────→ Gets CPU
        │
        │
   TASK_INTERRUPTIBLE              Sleeping, can be woken by signals
   TASK_UNINTERRUPTIBLE            Sleeping, cannot be interrupted
   TASK_KILLABLE                   Sleeping, only SIGKILL wakes
        │
        │ [Added to wait queue]
        │ [Waiting for event...]
        │
        ├──[wake_up()]──────────→ Back to TASK_RUNNING
        │                          Added to runqueue
        │
        └──[scheduler]──────────→ Gets CPU again
```

### Blocking I/O Flow with Queues

```
THREAD BLOCKS (read not ready)
  ↓
1. Find wait queue for resource
   q = folio_waitqueue(folio);              ← Hash to one of 256 queues
  ↓
2. Add to wait queue
   spin_lock(&q->lock);
   __add_wait_queue_entry_tail(q, &wait);   ← Linked list operation
   spin_unlock(&q->lock);
  ↓
3. Set task state
   set_current_state(TASK_KILLABLE);        ← Not runnable
  ↓
4. Call scheduler
   schedule();
     ↓
   pick_next_task(rq, prev)                 ← Pick from runqueue
     ↓
   Remove current from runqueue              ← No longer in runqueue
   Context switch to next task               ← Give up CPU
  ↓
*** THREAD BLOCKED, NOT ON ANY RUNQUEUE ***

... TIME PASSES ...

I/O COMPLETES
  ↓
1. Interrupt handler runs
   folio_unlock(folio);
     ↓
   folio_wake_bit(folio, PG_locked);
     ↓
   q = folio_waitqueue(folio);              ← Same hash, same queue!
  ↓
2. Walk wait queue
   spin_lock(&q->lock);
   __wake_up_common(q, ...);
     ↓
   for each entry in q->head:
       entry->func(entry, ...);             ← wake_page_function()
         ↓
       try_to_wake_up(task);
         ↓
         Set task state to TASK_RUNNING
         Add task to runqueue                ← Back on runqueue!
   spin_unlock(&q->lock);
  ↓
*** THREAD NOW ON RUNQUEUE, READY TO RUN ***

... WAIT FOR SCHEDULER ...

TIMER INTERRUPT
  ↓
sched_tick()
  ↓
Current task's timeslice expired
set_tsk_need_resched(current);
  ↓
RETURN FROM INTERRUPT
  ↓
schedule()
  ↓
pick_next_task(rq, prev)
  ↓
  Check RT queue → empty
  Check CFS queue → pick leftmost task
    ↓
    *** MIGHT BE OUR WOKEN THREAD! ***
  ↓
context_switch(prev, next)
  ↓
*** THREAD GETS CPU, RESUMES EXECUTION ***
```

### Queue Interaction Summary

| Queue Type | When Task Added | When Task Removed | Who Manages |
|------------|----------------|-------------------|-------------|
| **Runqueue** | Task becomes runnable | Task runs or blocks | Scheduler |
| **Wait Queue** | Task blocks waiting for event | Event occurs, task woken | Resource owner |
| **Work Queue** | Work scheduled | Work executes | Workqueue subsystem |
| **Timer Queue** | Timer started | Timer fires | Timer subsystem |

**Key Insight**: A task is **either on a runqueue OR a wait queue**, never both:

- **On runqueue** → TASK_RUNNING → waiting for CPU
- **On wait queue** → TASK_INTERRUPTIBLE/UNINTERRUPTIBLE/KILLABLE → waiting for event
- **Running** → not on any queue, has the CPU

---

## Visual Overview

### System-Wide Queue Architecture

```
┌────────────────────────────────────────────────────────────────┐
│                        4-CPU SYSTEM                            │
└────────────────────────────────────────────────────────────────┘

CPU 0 Runqueue                   CPU 1 Runqueue
├── CFS RQ                       ├── CFS RQ
│   └── [task1, task5, task9]   │   └── [task2, task6]
├── RT RQ                        ├── RT RQ
│   └── [rt_task1]              │   └── []
└── DL RQ                        └── DL RQ
    └── []                           └── []

CPU 2 Runqueue                   CPU 3 Runqueue
├── CFS RQ                       ├── CFS RQ
│   └── [task3, task7]          │   └── [task4, task8]
├── RT RQ                        ├── RT RQ
│   └── []                       │   └── [rt_task2]
└── DL RQ                        └── DL RQ
    └── []                           └── []

═══════════════════════════════════════════════════════════════

WAIT QUEUES (resource-specific, thousands of them)

Page Wait Queues (256 queues, hash table)
├── Queue #0:  [task10, task11]     ← Waiting for page A
├── Queue #1:  []
├── Queue #2:  [task12]             ← Waiting for page B
├── ...
└── Queue #255: [task13, task14]    ← Waiting for page C

Socket Wait Queues (per socket)
├── Socket 1: [task15]              ← Waiting for data
├── Socket 2: []
├── Socket 3: [task16, task17]      ← Multiple waiters
└── ...

TTY Wait Queues (per terminal)
├── TTY 0: [task18]                 ← Waiting for input
├── TTY 1: []
└── ...

Global Wait Queues
├── pci_cfg_wait: []
├── probe_waitqueue: [task19]
└── ...

═══════════════════════════════════════════════════════════════

TIMER INTERRUPT (every 1/HZ seconds)
  ↓
For each CPU:
  sched_tick()
    ↓
  Update current task's runtime
    ↓
  Check if timeslice expired
    ↓
  If yes: set_tsk_need_resched()
    ↓
  On return from interrupt:
    if (need_resched())
        schedule() → pick from runqueue
```

### Task Migration Between Queues

```
NEW TASK CREATED
  ↓
fork() / clone()
  ↓
wake_up_new_task()
  ↓
Add to CPU's runqueue
  ↓
┌──────────────┐
│  RUNQUEUE    │  ← TASK_RUNNING, waiting for CPU
│  [new_task]  │
└──────────────┘
  ↓
[Gets scheduled, runs on CPU]
  ↓
[Calls read(), data not ready]
  ↓
Remove from runqueue
  ↓
Add to wait queue
  ↓
┌──────────────┐
│  WAIT QUEUE  │  ← TASK_INTERRUPTIBLE, waiting for I/O
│  [new_task]  │
└──────────────┘
  ↓
[I/O completes]
  ↓
Remove from wait queue
  ↓
Add to runqueue
  ↓
┌──────────────┐
│  RUNQUEUE    │  ← TASK_RUNNING, waiting for CPU
│  [new_task]  │
└──────────────┘
  ↓
[Gets scheduled again]
  ↓
[Returns from read() with data]
  ↓
[Exits]
  ↓
TASK_DEAD
```

---

## Key Takeaways

### 1. **Runqueues: One Per CPU**

- **Count**: Exactly one runqueue per CPU core
- **Purpose**: Tasks ready to run on that CPU
- **Data structure**: Per-class sub-queues (CFS uses red-black tree)
- **Lock**: Per-runqueue spinlock (independent locking)

### 2. **Wait Queues: Thousands+**

- **Count**: Thousands to millions, dynamically created
- **Purpose**: Tasks waiting for specific events/resources
- **Location**: Embedded in objects, hash tables, global
- **Examples**: Page cache (256), sockets (thousands), devices (hundreds)

### 3. **Task is on Runqueue XOR Wait Queue**

A task is **never on both**:
- **Runnable** → on runqueue, state = TASK_RUNNING
- **Blocking** → on wait queue, state = TASK_INTERRUPTIBLE/UNINTERRUPTIBLE/KILLABLE
- **Running** → on no queue, has CPU

### 4. **Timer Interrupts Drive Scheduling**

- **Frequency**: HZ times per second (typically 250 or 1000)
- **Handler**: `sched_tick()` in `kernel/sched/core.c:5634`
- **Actions**:
  - Update task runtime
  - Check if timeslice expired
  - Set `TIF_NEED_RESCHED` if should preempt
  - Trigger load balancing

### 5. **Scheduling Happens at Specific Points**

```c
// Scheduler is invoked at:
1. Return from interrupt (including timer)
2. Return from syscall
3. Explicit schedule() call (blocking I/O, sleep, etc.)
4. Preemption point (preempt_enable(), cond_resched())
```

### 6. **Multiple Scheduling Classes**

Priority order (high to low):
1. Stop (per-CPU tasks)
2. Deadline (SCHED_DEADLINE)
3. Real-Time (SCHED_FIFO, SCHED_RR)
4. CFS (SCHED_NORMAL, SCHED_BATCH) ← Most tasks
5. Idle (runs when nothing else)

### 7. **CFS: The Default Scheduler**

- **Algorithm**: Completely Fair Scheduler
- **Goal**: Fair CPU time distribution
- **Data structure**: Red-black tree ordered by vruntime
- **Pick next**: O(1) - leftmost node
- **Timeslice**: Dynamic, based on number of tasks

### 8. **Wake-Up is Fast**

Waking a task:
```c
try_to_wake_up(task)
  ↓
Set state to TASK_RUNNING
  ↓
select_task_rq(task)  ← Pick CPU
  ↓
enqueue_task(rq, task)  ← Add to runqueue
  ↓
check_preempt_curr(rq, task)  ← Should preempt current?
```

**Complexity**: O(1) to O(log n) depending on scheduling class

### 9. **Tickless Mode Reduces Overhead**

- **CONFIG_NO_HZ_IDLE**: Stop timer when CPU idle
- **CONFIG_NO_HZ_FULL**: Stop timer even with one task
- **Benefit**: Lower power, less overhead
- **Trade-off**: Slightly more complex

### 10. **Load Balancing Moves Tasks**

Periodically (and on idle):
```c
// Move tasks from busy CPUs to idle CPUs
load_balance(rq)
  ↓
Find busiest CPU
  ↓
Pull tasks to this CPU
  ↓
Rebalance load across system
```

**Goal**: Keep all CPUs busy, minimize migrations

---

## Summary

The Linux scheduler manages **multiple queue types**:

1. **Runqueues** (one per CPU):
   - Tasks ready to run
   - Organized by scheduling class
   - CFS uses red-black tree by vruntime
   - Managed by scheduler

2. **Wait Queues** (thousands+):
   - Tasks waiting for events
   - Per-resource or hash tables
   - Managed by resource owners
   - Linked lists of waiters

3. **Timer interrupts** (HZ per second):
   - Update task runtime
   - Check for preemption
   - Trigger scheduling decisions
   - Can be stopped (tickless mode)

**Key flow**:
- Task runs → blocks → added to wait queue → removed from runqueue
- Event occurs → wake_up() → added to runqueue → gets CPU
- Timer tick → update vruntime → check preemption → maybe reschedule

This architecture enables:
- **Scalability**: Per-CPU runqueues avoid lock contention
- **Fairness**: CFS ensures fair CPU distribution
- **Responsiveness**: Timer preemption prevents monopolization
- **Efficiency**: Tickless mode reduces overhead

The scheduler is constantly balancing **fairness**, **latency**, and **throughput** while managing these queues!
