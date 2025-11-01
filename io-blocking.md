# Linux Kernel Blocking I/O: How Threads Block and Resume

This document explains the intuition behind how a thread gets blocked during I/O operations and how it gets resumed, using the page cache read path as an example.

## Table of Contents
- [High-Level Overview](#high-level-overview)
- [Key Data Structures](#key-data-structures)
- [The Blocking Path (Sleep)](#the-blocking-path-sleep)
- [The Wake-Up Path (Resume)](#the-wake-up-path-resume)
- [Wait Queues: Architecture](#wait-queues-architecture)
- [Complete Example Flow](#complete-example-flow)
- [Key Takeaways](#key-takeaways)

---

## High-Level Overview

When a userspace program performs a blocking I/O operation (like reading a file), the following happens:

1. **Syscall Entry**: Userspace calls `read(fd, buf, count)`
2. **VFS Layer**: Kernel translates fd to `struct file*` and routes to filesystem
3. **Page Cache Check**: Kernel checks if data is in memory (page cache)
4. **Blocking Decision**: If data not ready, thread must wait
5. **Add to Wait Queue**: Thread adds itself to a wait queue for that page
6. **Sleep**: Thread calls scheduler, gives up CPU, state becomes TASK_KILLABLE
7. **Wait**: Thread is off CPU, waiting for I/O to complete
8. **I/O Completes**: DMA transfer finishes, interrupt handler runs
9. **Wake-Up**: Interrupt handler wakes threads waiting on that page
10. **Resume**: Thread added back to runqueue, becomes TASK_RUNNING
11. **Return to Userspace**: Thread returns with data

**Key Insight**: Blocking is accomplished by:
- Adding thread to a wait queue
- Changing thread state to sleeping (TASK_INTERRUPTIBLE/TASK_KILLABLE/TASK_UNINTERRUPTIBLE)
- Calling the scheduler to give up the CPU

---

## Key Data Structures

### 1. File Descriptor Translation

```c
// Task has array of open files
struct task_struct {
    struct files_struct *files;  // Per-process file table
    ...
};

struct files_struct {
    struct fdtable *fdt;
    ...
};

struct fdtable {
    unsigned int max_fds;
    struct file __rcu **fd;  // Array of file pointers (indexed by fd number)
    ...
};
```

**Translation**: `fd` (integer) → `files->fdt->fd[fd]` → `struct file*`

**Location**: `include/linux/fdtable.h:72-86`, `fs/file.c:1138-1163`

---

### 2. Wait Queue Structures

```c
// Each wait queue has a head with a spinlock and list of waiters
struct wait_queue_head {
    spinlock_t      lock;   // Protects the list
    struct list_head head;  // Linked list of waiters
};

// Each waiter has an entry in the linked list
struct wait_queue_entry {
    unsigned int        flags;      // WQ_FLAG_WOKEN, WQ_FLAG_EXCLUSIVE, etc.
    void               *private;    // Pointer to task_struct
    wait_queue_func_t   func;       // Function to call when waking
    struct list_head    entry;      // Linked list node
};
```

**Location**: `include/linux/wait.h:28-38`

---

### 3. Task States

```c
#define TASK_RUNNING            0x00000000  // On runqueue, can get CPU
#define TASK_INTERRUPTIBLE      0x00000001  // Sleeping, can be woken by signals
#define TASK_UNINTERRUPTIBLE    0x00000002  // Sleeping, cannot be interrupted
#define TASK_KILLABLE           (TASK_WAKEKILL | TASK_UNINTERRUPTIBLE)  // Sleep, only SIGKILL wakes

struct task_struct {
    volatile long __state;  // Current task state
    ...
};
```

**Location**: `include/linux/sched.h:99-129`

---

## The Blocking Path (Sleep)

### Step-by-Step: read() Syscall to Blocking

#### 1. Syscall Entry (`fs/read_write.c:720-747`)

```c
SYSCALL_DEFINE3(read, unsigned int, fd, char __user *, buf, size_t, count)
{
    return ksys_read(fd, buf, count);
}

ssize_t ksys_read(unsigned int fd, char __user *buf, size_t count)
{
    CLASS(fd_pos, f)(fd);  // Translates fd to struct file*

    if (!fd_empty(f)) {
        ret = vfs_read(fd_file(f), buf, count, ppos);
    }
    return ret;
}
```

**What happens**: Integer fd → array lookup → `struct file*`

---

#### 2. VFS Read (`fs/read_write.c:476-501`)

```c
ssize_t vfs_read(struct file *file, char __user *buf, size_t count, loff_t *pos)
{
    // Security checks, position validation...

    if (file->f_op->read_iter) {
        ret = new_sync_read(file, buf, count, pos);  // Modern path
    }
    // else legacy read()...

    return ret;
}
```

**What happens**: Routes to filesystem-specific read, most use `read_iter`

---

#### 3. Page Cache Read (`mm/filemap.c:2664-2700`)

```c
ssize_t filemap_read(struct kiocb *iocb, struct iov_iter *iter, ...)
{
    struct file *filp = iocb->ki_filp;
    struct address_space *mapping = filp->f_mapping;
    struct folio *folio;

    // Loop through pages needed for this read
    do {
        // Find page in page cache
        folio = filemap_get_read_folio(mapping, index);

        if (IS_ERR(folio)) {
            // Page not in cache, need to read from disk
            error = filemap_read_folio(filp, mapping, folio);
            // ↑ THIS BLOCKS if page not ready
        }

        // Copy data to userspace buffer
        copy_folio_to_iter(folio, offset, bytes, iter);

    } while (iov_iter_count(iter) && iocb->ki_pos < isize);
}
```

**What happens**: Checks page cache, if page not ready → block

---

#### 4. Wait for Page (`mm/filemap.c:2390-2414`)

```c
static int filemap_read_folio(struct file *file, struct address_space *mapping,
                              struct folio *folio)
{
    // Start async read from disk
    error = filemap_read_folio_async(file, mapping, folio, sync);

    if (error == AOP_TRUNCATED_PAGE)
        return error;

    // Wait for the page to be unlocked (I/O to complete)
    return folio_wait_locked_killable(folio);  // ← BLOCKS HERE
}
```

**What happens**: Starts disk I/O, then waits for page lock to be released

---

#### 5. The Core Blocking Function (`mm/filemap.c:1238-1350`)

This is where the **actual blocking** happens:

```c
static inline int folio_wait_bit_common(struct folio *folio, int bit_nr,
                                        int state, enum behavior behavior)
{
    // STEP 1: Find the wait queue for this page
    wait_queue_head_t *q = folio_waitqueue(folio);  // Hash page → queue
    struct wait_page_queue wait_page;
    wait_queue_entry_t *wait = &wait_page.wait;

    // STEP 2: Initialize wait entry
    init_wait(wait);
    wait->func = wake_page_function;  // Custom wake function
    wait->private = current;          // Store current task_struct

    // STEP 3: Add ourselves to the wait queue (CRITICAL!)
    spin_lock_irq(&q->lock);
    folio_set_waiters(folio);  // Mark page as having waiters

    if (!folio_trylock_flag(folio, bit_nr, wait)) {
        __add_wait_queue_entry_tail(q, wait);  // Add to tail of linked list
    }
    spin_unlock_irq(&q->lock);

    // STEP 4: Sleep loop
    for (;;) {
        // Set task state to KILLABLE (can be woken by SIGKILL)
        set_current_state(state);  // state = TASK_KILLABLE

        // Check if we've been woken up
        flags = smp_load_acquire(&wait->flags);
        if (!(flags & WQ_FLAG_WOKEN)) {
            // Check for signals
            if (signal_pending_state(state, current))
                break;

            // STEP 5: Give up CPU and BLOCK
            io_schedule();  // ← THREAD BLOCKS HERE, OFF CPU
            continue;
        }

        // We've been woken up!
        break;
    }

    // STEP 6: Cleanup
    finish_wait(q, wait);  // Remove from wait queue if still there

    return ret;
}
```

**What happens**:
1. **Hash to wait queue**: Uses `folio_waitqueue(folio)` to find which wait queue to use
2. **Add to queue**: Under spinlock, adds wait entry to linked list
3. **Set state**: Changes task state from TASK_RUNNING → TASK_KILLABLE
4. **Call scheduler**: `io_schedule()` invokes scheduler, thread gives up CPU
5. **Thread is now OFF CPU**: Not scheduled until woken

---

#### 6. The Scheduler Takes Over (`kernel/sched/core.c:7672-7693`)

```c
void __sched io_schedule(void)
{
    int token;

    token = io_schedule_prepare();  // Mark as I/O wait for accounting
    schedule();                     // Call scheduler
    io_schedule_finish(token);
}
```

**schedule()** does:
1. Picks next task to run from runqueue
2. Context switch to that task
3. Current task remains in wait queue, state = TASK_KILLABLE
4. Current task is **not on runqueue**, so it won't get CPU time

**Location**: `kernel/sched/core.c:6792-6935`

---

## The Wake-Up Path (Resume)

### Step-by-Step: I/O Completion to Thread Resumption

#### 1. I/O Completes

When disk I/O finishes:
- DMA transfer completes
- Disk controller raises interrupt
- Interrupt handler runs
- Handler updates page state and unlocks page

---

#### 2. Page Unlock (`mm/filemap.c:1493-1502`)

```c
void folio_unlock(struct folio *folio)
{
    // Check if there are waiters
    if (folio_xor_flags_has_waiters(folio, 1 << PG_locked))
        folio_wake_bit(folio, PG_locked);  // Wake them up!
}
```

**What happens**: Clears PG_locked bit, checks for waiters, wakes them

---

#### 3. Wake All Waiters (`mm/filemap.c:1175-1201`)

```c
static void folio_wake_bit(struct folio *folio, int bit_nr)
{
    // STEP 1: Find the SAME wait queue (same hash function!)
    wait_queue_head_t *q = folio_waitqueue(folio);
    struct wait_page_key key;
    unsigned long flags;

    // STEP 2: Prepare wake key (identifies which page/bit)
    key.folio = folio;
    key.bit_nr = bit_nr;
    key.page_match = 0;

    // STEP 3: Wake all threads waiting on this page
    spin_lock_irqsave(&q->lock, flags);
    __wake_up_locked_key(q, TASK_NORMAL, &key);  // Walk list, wake matching threads

    // STEP 4: Clear waiters flag if queue is empty
    if (!waitqueue_active(q) || !key.page_match)
        folio_clear_waiters(folio);

    spin_unlock_irqrestore(&q->lock, flags);
}
```

**What happens**: Hashes to same queue, walks linked list, wakes matching threads

---

#### 4. Walk Wait Queue (`kernel/sched/wait.c:73-97`)

```c
static int __wake_up_common(struct wait_queue_head *wq_head, unsigned int mode,
                            int nr_exclusive, int wake_flags, void *key)
{
    wait_queue_entry_t *curr, *next;

    // Must be called with lock held
    lockdep_assert_held(&wq_head->lock);

    // Get first waiter
    curr = list_first_entry(&wq_head->head, wait_queue_entry_t, entry);

    // Empty queue?
    if (&curr->entry == &wq_head->head)
        return nr_exclusive;

    // Walk the linked list of waiters
    list_for_each_entry_safe_from(curr, next, &wq_head->head, entry) {
        unsigned flags = curr->flags;
        int ret;

        // Call the wake function for this waiter
        ret = curr->func(curr, mode, wake_flags, key);

        if (ret < 0)
            break;  // Stop waking

        // For exclusive waiters, only wake one
        if (ret && (flags & WQ_FLAG_EXCLUSIVE) && !--nr_exclusive)
            break;
    }

    return nr_exclusive;
}
```

**What happens**: For each entry in linked list, calls `curr->func()` (the wake function)

---

#### 5. Per-Waiter Wake Function (`mm/filemap.c:1124-1172`)

This function is called **for each waiter** in the queue:

```c
static int wake_page_function(wait_queue_entry_t *wait, unsigned mode, int sync, void *arg)
{
    struct wait_page_key *key = arg;
    struct wait_page_queue *wait_page = container_of(wait, struct wait_page_queue, wait);

    // STEP 1: Check if this waiter is waiting for THIS page
    if (!wake_page_match(wait_page, key))
        return 0;  // Different page, skip this waiter

    // STEP 2: Handle exclusive waiters (like lock handoff)
    flags = wait->flags;
    if (flags & WQ_FLAG_EXCLUSIVE) {
        if (test_bit(key->bit_nr, &key->folio->flags))
            return -1;  // Bit still set, stop
        // Try to grab the lock for this waiter
        if (flags & WQ_FLAG_CUSTOM) {
            if (test_and_set_bit(key->bit_nr, &key->folio->flags))
                return -1;  // Someone else got it
            flags |= WQ_FLAG_DONE;
        }
    }

    // STEP 3: Mark waiter as WOKEN (sleeper will see this flag)
    smp_store_release(&wait->flags, flags | WQ_FLAG_WOKEN);

    // STEP 4: Actually wake the task!
    wake_up_state(wait->private, mode);  // wait->private = task_struct

    // STEP 5: Remove from wait queue
    list_del_init_careful(&wait->entry);

    return (flags & WQ_FLAG_EXCLUSIVE) != 0;
}
```

**What happens**:
1. Checks if this waiter wants THIS specific page (handles hash collisions)
2. If yes, sets WQ_FLAG_WOKEN flag
3. Wakes the task via `wake_up_state()`
4. Removes waiter from linked list

---

#### 6. Wake the Task (`kernel/sched/core.c:7095-7100`)

```c
int default_wake_function(wait_queue_entry_t *curr, unsigned mode, int wake_flags, void *key)
{
    return try_to_wake_up(curr->private, mode, wake_flags);
}
```

**try_to_wake_up()** (simplified):
1. Changes task state from TASK_KILLABLE → TASK_RUNNING
2. Adds task to runqueue
3. May preempt current task if priority is higher
4. Task will now be scheduled again

**Location**: `kernel/sched/core.c:4346-4576`

---

#### 7. Sleeping Thread Wakes Up

Back in **folio_wait_bit_common()**, the sleeping thread:

```c
for (;;) {
    set_current_state(state);

    // Check if we've been woken (WQ_FLAG_WOKEN was set by waker)
    flags = smp_load_acquire(&wait->flags);
    if (!(flags & WQ_FLAG_WOKEN)) {
        io_schedule();  // Was blocked here
        continue;
    }

    // Flag is set! We've been woken!
    break;  // Exit loop
}

finish_wait(q, wait);  // Cleanup
return 0;  // Success
```

**What happens**:
1. Scheduler gives CPU back to this thread
2. Thread resumes execution after `io_schedule()`
3. Checks `WQ_FLAG_WOKEN` flag (set by waker)
4. Sees flag is set, exits loop
5. Returns to caller

---

#### 8. Return to Userspace

The call chain unwinds:
- `folio_wait_bit_common()` → `filemap_read_folio()` → `filemap_read()` → `vfs_read()` → `ksys_read()` → userspace

Data is now in page cache, gets copied to userspace buffer, and `read()` syscall returns with byte count.

---

## Wait Queues: Architecture

### Not Global – Decentralized Design

Wait queues are **NOT global**. Instead, there are **thousands to millions** of wait queues throughout the kernel:

### 1. Global/Subsystem Wait Queues

Static, declared globally:

```c
DECLARE_WAIT_QUEUE_HEAD(pci_cfg_wait);      // PCI configuration
DECLARE_WAIT_QUEUE_HEAD(probe_waitqueue);   // Driver probing
```

### 2. Hash Table of Wait Queues (Page Cache)

To save memory, Linux uses a **hash table** instead of one queue per page:

```c
// mm/filemap.c:1071-1078
#define PAGE_WAIT_TABLE_BITS 8
#define PAGE_WAIT_TABLE_SIZE (1 << PAGE_WAIT_TABLE_BITS)  // 256 queues

static wait_queue_head_t folio_wait_table[PAGE_WAIT_TABLE_SIZE] __cacheline_aligned;

static wait_queue_head_t *folio_waitqueue(struct folio *folio)
{
    return &folio_wait_table[hash_ptr(folio, PAGE_WAIT_TABLE_BITS)];
}
```

**Why?** From the code comment:
> "By using a hash table of waitqueues... this **saves space** at a cost of 'thundering herd' phenomena during rare hash collisions."

Instead of millions of queues (one per page), we have **256 shared queues**.

**Hash collisions are OK** because:
- Each waiter stores which page it's waiting for
- Wake function checks if waiter wants **this specific page**
- Only matching waiters are woken

### 3. Per-Object Embedded Wait Queues

Many kernel objects embed their own wait queues:

```c
struct task_struct {
    wait_queue_head_t wait_chldexit;  // For wait() syscall
};

struct socket {
    wait_queue_head_t wait;  // Socket events
};

struct inode {
    wait_queue_head_t i_cap_wq;  // Capability events
};
```

### Visual: Same Queue for Sleep and Wake

```
┌────────────────────────────────────────────────────┐
│            folio_wait_table[256]                   │
│                                                    │
│  Hash(page_A) = 42                                 │
│                                                    │
│  Queue #42:                                        │
│  ┌──────────────────────────────────────────────┐  │
│  │ wait_queue_head_t                            │  │
│  │   .lock = spinlock                           │  │
│  │   .head = linked list:                       │  │
│  │                                              │  │
│  │   ┌─────────────────────┐                    │  │
│  │   │ wait_queue_entry    │                    │  │
│  │   │  .private = task1   │ ← Waiting for A    │  │
│  │   │  .func = wake_fn    │                    │  │
│  │   │  .flags = 0         │                    │  │
│  │   └─────────────────────┘                    │  │
│  │           ↓                                  │  │
│  │   ┌─────────────────────┐                    │  │
│  │   │ wait_queue_entry    │                    │  │
│  │   │  .private = task2   │ ← Waiting for A    │  │
│  │   │  .func = wake_fn    │                    │  │
│  │   │  .flags = 0         │                    │  │
│  │   └─────────────────────┘                    │  │
│  │           ↓                                  │  │
│  │   ┌─────────────────────┐                    │  │
│  │   │ wait_queue_entry    │                    │  │
│  │   │  .private = task3   │ ← Waiting for B!   │  │
│  │   │  .func = wake_fn    │   (hash collision) │  │
│  │   │  .flags = 0         │                    │  │
│  │   └─────────────────────┘                    │  │
│  └──────────────────────────────────────────────┘  │
└────────────────────────────────────────────────────┘

BLOCKING (task1):                   WAKING (I/O complete for A):
  1. Hash(page_A) → Queue #42         1. Hash(page_A) → Queue #42
  2. spin_lock(&Queue42.lock)         2. spin_lock(&Queue42.lock)
  3. Add task1 to list                3. For each entry in list:
  4. spin_unlock()                       - Call entry->func(entry, key)
  5. set_current_state(KILLABLE)         - wake_fn checks: waiting for A?
  6. io_schedule()                       - If yes: set WOKEN, wake task
  7. ← OFF CPU                        4. spin_unlock()

  ... TIME PASSES, I/O COMPLETES ...

  8. ← Gets CPU again                 Result: task1 and task2 woken
  9. Checks WQ_FLAG_WOKEN                     task3 still sleeping (wants B)
 10. Flag is set! Exit loop
```

---

## Complete Example Flow

Let's trace a read that blocks:

```
USER:    read(fd, buf, 1024)
           ↓
SYSCALL: SYSCALL_DEFINE3(read, ...) in fs/read_write.c:720
           ↓
         ksys_read(fd, buf, count)
           ↓
         CLASS(fd_pos, f)(fd)  ← Translate fd → struct file*
           ↓                      (files->fdt->fd[fd])
         vfs_read(file, buf, count, pos)
           ↓
VFS:     file->f_op->read_iter()
           ↓
         new_sync_read() in fs/read_write.c:389
           ↓
         call_read_iter(file, &kiocb, &iter)
           ↓
FS:      [filesystem's read_iter, e.g., ext4_file_read_iter]
           ↓
         generic_file_read_iter() in mm/filemap.c:2933
           ↓
PAGECACHE: filemap_read() in mm/filemap.c:2664
           ↓
         folio = filemap_get_read_folio()  ← Check page cache
           ↓
         [Page not in cache or not ready]
           ↓
         filemap_read_folio() in mm/filemap.c:2390
           ↓
         [Start async disk read]
           ↓
         folio_wait_locked_killable(folio) in mm/filemap.c:1469
           ↓
WAIT:    folio_wait_bit_common(folio, PG_locked, TASK_KILLABLE, ...)
           ↓
         q = folio_waitqueue(folio)  ← Hash to queue #42
           ↓
         spin_lock(&q->lock)
           ↓
         __add_wait_queue_entry_tail(q, &wait)  ← Add to list
           ↓
         spin_unlock(&q->lock)
           ↓
         set_current_state(TASK_KILLABLE)  ← Change state
           ↓
         io_schedule() in kernel/sched/core.c:7685
           ↓
SCHED:   schedule() in kernel/sched/core.c:6792
           ↓
         [Pick next task, context switch]
           ↓
         *** THREAD IS NOW OFF CPU, BLOCKED ***

... TIME PASSES, I/O COMPLETES ...

DISK:    [DMA transfer done, interrupt raised]
           ↓
IRQ:     [Interrupt handler runs]
           ↓
         [Update page state, mark as uptodate]
           ↓
         folio_unlock(folio) in mm/filemap.c:1493
           ↓
WAKE:    folio_wake_bit(folio, PG_locked) in mm/filemap.c:1175
           ↓
         q = folio_waitqueue(folio)  ← Same hash! Queue #42
           ↓
         spin_lock(&q->lock)
           ↓
         __wake_up_locked_key(q, TASK_NORMAL, &key)
           ↓
         __wake_up_common() in kernel/sched/wait.c:73
           ↓
         [Walk linked list of waiters]
           ↓
         for each entry: entry->func(entry, mode, key)
           ↓
         wake_page_function() in mm/filemap.c:1124
           ↓
         if (wake_page_match(wait_page, key))  ← Check: right page?
           ↓
         smp_store_release(&wait->flags, WQ_FLAG_WOKEN)  ← Set flag
           ↓
         wake_up_state(wait->private, mode)  ← Wake task!
           ↓
         try_to_wake_up() in kernel/sched/core.c:4346
           ↓
         [Change state: TASK_KILLABLE → TASK_RUNNING]
           ↓
         [Add to runqueue]
           ↓
         spin_unlock(&q->lock)
           ↓
SCHED:   [Scheduler picks our thread]
           ↓
         *** THREAD GETS CPU AGAIN ***
           ↓
RESUME:  [Resume after io_schedule()]
           ↓
         flags = smp_load_acquire(&wait->flags)
           ↓
         if (flags & WQ_FLAG_WOKEN) break;  ← Yes! Exit loop
           ↓
         finish_wait(q, wait)  ← Cleanup
           ↓
         return 0  ← Success
           ↓
         [Unwind: filemap_read → generic_file_read_iter → ...]
           ↓
         [Copy page data to user buffer]
           ↓
USER:    read() returns 1024  ← Success!
```

---

## Key Takeaways

### 1. **Blocking = State Change + Scheduler Call**

Blocking a thread is accomplished by:
1. Adding thread to a wait queue (linked list)
2. Changing task state (TASK_RUNNING → TASK_KILLABLE/INTERRUPTIBLE/UNINTERRUPTIBLE)
3. Calling `schedule()` to give up the CPU
4. Thread is removed from runqueue, won't get CPU time until woken

### 2. **Wait Queues are Decentralized**

- **Not global**: Thousands to millions of wait queues
- **Page cache uses hash table**: 256 queues for all pages (saves memory)
- **Hash collisions OK**: Wake function filters by specific resource
- **Same queue for sleep and wake**: Both use same hash function

### 3. **Symmetrical Sleep/Wake**

Both sleep and wake use the **exact same wait queue**:
- **Sleep**: Hash(resource) → queue → add to list → sleep
- **Wake**: Hash(resource) → **same queue** → walk list → wake

### 4. **Lock Ordering Prevents Races**

```c
// SLEEP side:
spin_lock(&q->lock);
add_to_list();
spin_unlock(&q->lock);
set_current_state(SLEEPING);  // Memory barrier here
schedule();

// WAKE side:
spin_lock(&q->lock);
for_each_waiter:
    set_flag(WOKEN);  // Memory barrier
    try_to_wake_up();
spin_unlock(&q->lock);
```

Memory barriers ensure:
- Waker sees waiter in list OR
- Waiter sees WOKEN flag
- No lost wakeups

### 5. **Task States Matter**

- **TASK_RUNNING**: On runqueue, can get CPU
- **TASK_INTERRUPTIBLE**: Sleeping, any signal wakes
- **TASK_KILLABLE**: Sleeping, only SIGKILL wakes (used for I/O)
- **TASK_UNINTERRUPTIBLE**: Sleeping, no signals wake (short critical sections only)

### 6. **I/O Scheduling is Special**

`io_schedule()` vs `schedule()`:
- `io_schedule()` marks task as waiting for I/O (accounting)
- Shows up in `iowait` CPU statistics
- Helps distinguish I/O-bound from CPU-bound workloads

### 7. **Multiple Waiters Handled**

- One resource (page) can have many waiters
- All added to same wait queue linked list
- Wake walks entire list, calling wake function for each
- Non-exclusive: wake all
- Exclusive: wake one (for locks)

### 8. **Hash Collisions are Fine**

Multiple pages can hash to same wait queue:
- Each waiter stores which resource it wants
- Wake function checks: `if (waiter.page == waking_page)`
- Only matching waiters are woken
- Others keep sleeping

### 9. **Complete Flow is Synchronous**

From userspace perspective:
```c
// Userspace
ssize_t bytes = read(fd, buf, count);  // Blocks here if data not ready
// Returns when I/O complete
```

Kernel transforms this into:
- Async I/O operation (submit to disk)
- Thread blocks (off CPU)
- I/O completes (interrupt)
- Thread wakes (back on CPU)
- Returns to userspace

### 10. **Context Switch is Hidden**

From thread's perspective:
```c
// Before io_schedule()
io_schedule();  // ← Blocks
// After io_schedule() - could be milliseconds later!
```

Thread doesn't know:
- How long it was blocked
- What ran on the CPU while it slept
- When exactly the I/O completed

It just continues execution as if `io_schedule()` was a long function call.

---

## Files and Line Numbers Reference

### Core Files

**Syscall Entry**:
- `fs/read_write.c:702-718` - `ksys_read()` with fd translation
- `fs/read_write.c:720-723` - `SYSCALL_DEFINE3(read)`

**FD Translation**:
- `include/linux/fdtable.h:72-86` - `files_lookup_fd_raw()`
- `fs/file.c:1138-1163` - `__fget_light()` with fast/slow path

**VFS Layer**:
- `fs/read_write.c:476-501` - `vfs_read()`
- `fs/read_write.c:389-434` - `new_sync_read()`

**Page Cache**:
- `mm/filemap.c:2664-2700` - `filemap_read()`
- `mm/filemap.c:2390-2414` - `filemap_read_folio()`
- `mm/filemap.c:1469-1471` - `folio_wait_locked_killable()`

**Wait Queue Core**:
- `mm/filemap.c:1238-1350` - `folio_wait_bit_common()` (THE blocking function)
- `mm/filemap.c:1124-1172` - `wake_page_function()` (per-waiter wake)
- `mm/filemap.c:1175-1201` - `folio_wake_bit()` (initiate wake)
- `mm/filemap.c:1493-1502` - `folio_unlock()` (trigger wake)

**Wait Queue Hash Table**:
- `mm/filemap.c:1071-1078` - Hash table definition and `folio_waitqueue()`

**Generic Wait Queue**:
- `include/linux/wait.h:28-38` - Data structure definitions
- `kernel/sched/wait.c:73-97` - `__wake_up_common()` (walk list)
- `kernel/sched/wait.c:145-149` - `__wake_up_locked_key()`
- `kernel/sched/wait.c:382-391` - `autoremove_wake_function()`

**Scheduler**:
- `kernel/sched/core.c:7685-7693` - `io_schedule()`
- `kernel/sched/core.c:6792-6935` - `schedule()` (context switch)
- `kernel/sched/core.c:4346-4576` - `try_to_wake_up()` (actual wake)
- `kernel/sched/core.c:7095-7100` - `default_wake_function()`

**Task States**:
- `include/linux/sched.h:99-129` - TASK_* definitions
- `include/linux/sched.h:226-236` - `set_current_state()`

---

## Conclusion

Linux kernel blocking is elegant in its simplicity:

1. **Find the right wait queue** (via hash or direct reference)
2. **Add yourself to the queue** (linked list)
3. **Change your state** (RUNNING → SLEEPING)
4. **Call the scheduler** (give up CPU)
5. **Wake-up reverses this**: Waker finds same queue, walks list, marks WOKEN, calls scheduler
6. **Sleeping thread resumes**: Sees WOKEN flag, exits loop, continues

The key insight is that **both sides use the same wait queue**, found via the **same hash function** (for page cache) or **same embedded queue** (for other resources). This ensures they always meet at the right synchronization point.

This mechanism is used throughout the kernel for any resource that threads might need to wait for: pages, locks, I/O completion, events, etc.
