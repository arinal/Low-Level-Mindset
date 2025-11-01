# Linux Kernel Non-Blocking I/O Explained

This document explains how non-blocking I/O works in the Linux kernel, contrasting it with blocking I/O to show the fundamental differences in approach and implementation.

## Table of Contents
- [Overview](#overview)
- [Key Concept](#key-concept)
- [Setting Non-Blocking Mode](#setting-non-blocking-mode)
- [The Two Scenarios](#the-two-scenarios)
  - [Scenario 1: Data Ready (Fast Path)](#scenario-1-data-ready-fast-path)
  - [Scenario 2: Data Not Ready (Immediate Return)](#scenario-2-data-not-ready-immediate-return)
- [Blocking vs Non-Blocking Comparison](#blocking-vs-non-blocking-comparison)
- [Implementation Details](#implementation-details)
- [Use Cases and Patterns](#use-cases-and-patterns)
- [Performance Characteristics](#performance-characteristics)
- [Complete Code Traces](#complete-code-traces)
- [Key Takeaways](#key-takeaways)

---

## Overview

**Non-blocking I/O** is a mode where I/O operations **never block** the calling thread. Instead of waiting for data to become available, the syscall returns immediately with either:
- **Success**: Data was ready, here it is
- **Error (EAGAIN/EWOULDBLOCK)**: Data not ready, try again later

This is fundamentally different from blocking I/O, where the thread sleeps until data is available.

---

## Key Concept

### The Central Principle

**Blocking I/O**:
```
"Wait until ready, then return"
Thread gives up CPU, sleeps, wakes when ready
```

**Non-Blocking I/O**:
```
"Return immediately, ready or not"
Thread never gives up CPU, never sleeps
```

### The Trade-Off

| Aspect | Blocking I/O | Non-Blocking I/O |
|--------|--------------|------------------|
| **Simplicity** | ✅ Simple: just call read() | ❌ Complex: event loops, state machines |
| **Scalability** | ❌ One thread per connection | ✅ One thread, many connections |
| **Resources** | ❌ High: thread stacks, context switches | ✅ Low: single thread |
| **Latency** | ✅ Good: woken immediately when ready | ⚠️ Variable: depends on poll frequency |
| **CPU Usage** | ✅ Low: thread sleeps when waiting | ⚠️ Can be high if busy-polling |

---

## Setting Non-Blocking Mode

### Method 1: At open() Time

```c
#include <fcntl.h>

// Open file in non-blocking mode
int fd = open("/dev/ttyS0", O_RDONLY | O_NONBLOCK);

// All read() calls on this fd will be non-blocking
ssize_t n = read(fd, buf, sizeof(buf));
if (n == -1 && errno == EAGAIN) {
    // No data available, try again later
}
```

**Kernel flag**: `O_NONBLOCK` in `include/uapi/asm-generic/fcntl.h:38`
```c
#define O_NONBLOCK   00004000
```

### Method 2: Using fcntl() After Opening

```c
#include <fcntl.h>

// Open in blocking mode first
int fd = open("/dev/ttyS0", O_RDONLY);

// Get current flags
int flags = fcntl(fd, F_GETFL, 0);

// Add O_NONBLOCK flag
fcntl(fd, F_SETFL, flags | O_NONBLOCK);

// Now non-blocking
ssize_t n = read(fd, buf, sizeof(buf));
```

### Method 3: Per-Operation (preadv2/pwritev2 with RWF_NOWAIT)

```c
#include <sys/uio.h>

// File is blocking, but this specific read is non-blocking
struct iovec iov = {
    .iov_base = buf,
    .iov_len = sizeof(buf)
};

ssize_t n = preadv2(fd, &iov, 1, 0, RWF_NOWAIT);
if (n == -1 && errno == EAGAIN) {
    // This specific operation would block
}
```

**Kernel flag**: `IOCB_NOWAIT` in `include/linux/fs.h:355`
```c
#define IOCB_NOWAIT  (__force int) RWF_NOWAIT
```

### How Flags Propagate in Kernel

```c
// fs/read_write.c:389-434 - new_sync_read()
static ssize_t new_sync_read(struct file *filp, char __user *buf, ...)
{
    struct kiocb kiocb;

    init_sync_kiocb(&kiocb, filp);

    // Translate file flag to I/O control block flag
    if (filp->f_flags & O_NONBLOCK)
        kiocb.ki_flags |= IOCB_NOWAIT;  // ← File-level non-blocking

    // Or per-operation flag (from preadv2 with RWF_NOWAIT)
    // kiocb.ki_flags |= IOCB_NOWAIT;

    return call_read_iter(filp, &kiocb, &iter);
}
```

Throughout the I/O path, kernel checks:
```c
if (iocb->ki_flags & IOCB_NOWAIT) {
    // Use trylock, return -EAGAIN if would block
} else {
    // Use blocking lock, wait if needed
}
```

---

## The Two Scenarios

### Scenario 1: Data Ready (Fast Path)

When data is **already in page cache** and **ready**, both blocking and non-blocking I/O follow the **exact same path**.

#### Flow Diagram

```
USER:   read(fd, buf, 4096)  [O_NONBLOCK set, data ready]
          ↓
SYSCALL: SYSCALL_DEFINE3(read, fd, buf, count)
          ↓
        ksys_read(fd, buf, count)
          ↓
        CLASS(fd_pos, f)(fd)  ← fd → struct file*
          ↓
VFS:    vfs_read(file, buf, count, pos)
          ↓
        new_sync_read(file, buf, len, ppos)
          ↓
        kiocb.ki_flags |= IOCB_NOWAIT  ← From O_NONBLOCK
          ↓
        call_read_iter(file, &kiocb, &iter)
          ↓
FS:     generic_file_read_iter(&kiocb, &iter)
          ↓
        filemap_read(iocb, iter, 0)
          ↓
PAGE    filemap_get_pages(iocb, count, &fbatch, false)
CACHE:    ↓
        filemap_get_read_batch(mapping, index, max, &fbatch)
          ↓
        XA_STATE(xas, &mapping->i_pages, index)
          ↓
        folio = xas_load(&xas)  ← LOOKUP in page cache (XArray)
          ↓
        *** FOUND! ***
          ↓
        folio_try_get(folio)  ← Increment refcount
          ↓
        if (!folio_test_uptodate(folio))  ← Check if ready
            break;  // Not ready, would need to wait
          ↓
        *** UPTODATE! Data is ready! ***
          ↓
        folio_batch_add(fbatch, folio)
          ↓
        [Return to filemap_read()]
          ↓
COPY:   for each folio in batch:
          folio_mark_accessed(folio)  ← LRU management
          ↓
        copy_folio_to_iter(folio, offset, bytes, iter)
          ↓
        copy_to_user(user_buf, kernel_page, bytes)
          ↓
        *** DATA COPIED TO USERSPACE! ***
          ↓
        already_read += copied
          ↓
        folio_put(folio)  ← Release refcount
          ↓
RETURN: return already_read  (4096 bytes)
          ↓
        [Unwind call stack...]
          ↓
USER:   read() returns 4096  ← SUCCESS!

TIME: ~1-5 microseconds
NO BLOCKING, NO I/O, NO CONTEXT SWITCH
```

#### Key Code Paths

**Page cache lookup** (`mm/filemap.c:2354-2388`):
```c
static void filemap_get_read_batch(struct address_space *mapping,
                                   pgoff_t index, pgoff_t max,
                                   struct folio_batch *fbatch)
{
    XA_STATE(xas, &mapping->i_pages, index);  // XArray iterator
    struct folio *folio;

    rcu_read_lock();

    for (folio = xas_load(&xas); folio; folio = xas_next(&xas)) {
        // Skip retries, check bounds
        if (xas_retry(&xas, folio))
            continue;
        if (xas.xa_index > max || xa_is_value(folio))
            break;

        // Get reference to page
        if (!folio_try_get(folio))
            goto retry;

        // Add to batch
        if (!folio_batch_add(fbatch, folio))
            break;

        // CHECK: Is data ready?
        if (!folio_test_uptodate(folio))
            break;  // Not ready, stop here

        // Ready! Continue to get more pages if needed
    }

    rcu_read_unlock();
}
```

**Copy to userspace** (`mm/filemap.c:2732-2762`):
```c
// In filemap_read(), after getting pages:
for (i = 0; i < folio_batch_count(&fbatch); i++) {
    struct folio *folio = fbatch.folios[i];
    size_t offset = iocb->ki_pos & (folio_size(folio) - 1);
    size_t bytes = min_t(loff_t, end_offset - iocb->ki_pos,
                         folio_size(folio) - offset);
    size_t copied;

    // Mark as recently used (LRU)
    folio_mark_accessed(folio);

    // Flush cache if writably mapped
    if (writably_mapped)
        flush_dcache_folio(folio);

    // COPY DATA TO USERSPACE
    copied = copy_folio_to_iter(folio, offset, bytes, iter);

    already_read += copied;
    iocb->ki_pos += copied;

    if (copied < bytes) {
        error = -EFAULT;  // Bad userspace pointer
        break;
    }
}
```

#### What Gets Executed

```c
✅ XArray lookup:        xas_load(&mapping->i_pages, index)
✅ Check uptodate:       folio_test_uptodate(folio)
✅ Refcount ops:         folio_try_get(folio), folio_put(folio)
✅ Copy to user:         copy_folio_to_iter() → copy_to_user()
✅ LRU update:           folio_mark_accessed(folio)
✅ Return success:       return bytes_read

❌ NO wait queues
❌ NO task state changes
❌ NO scheduler calls
❌ NO context switches
❌ NO disk I/O
❌ NO blocking
```

---

### Scenario 2: Data Not Ready (Immediate Return)

When data is **not in page cache** or **not ready**, this is where blocking and non-blocking diverge dramatically.

#### Non-Blocking: Immediate -EAGAIN

```
USER:   read(fd, buf, 4096)  [O_NONBLOCK set, data NOT ready]
          ↓
        [Same syscall path as above...]
          ↓
PAGE    filemap_get_pages(iocb, count, &fbatch, false)
CACHE:    ↓
        filemap_get_read_batch(mapping, index, max, &fbatch)
          ↓
        folio = xas_load(&xas)  ← Try to find page
          ↓
        *** NOT FOUND or NOT UPTODATE! ***
          ↓
        if (!folio_batch_count(fbatch)) {
            // No pages in cache at all
            if (iocb->ki_flags & IOCB_NOIO)
                return -EAGAIN;  // ← EARLY EXIT 1
        }
          ↓
        OR if page exists but not ready:
          ↓
        if (!folio_test_uptodate(folio)) {
            // Page in cache but I/O still pending

            if (iocb->ki_flags & IOCB_NOWAIT) {
                folio_put(folio);
                return -EAGAIN;  // ← EARLY EXIT 2
            }

            // Blocking path would wait here...
        }
          ↓
RETURN: return -EAGAIN
          ↓
        [Unwind call stack with error...]
          ↓
USER:   read() returns -1, errno = EAGAIN
          ↓
        // Application decides what to do:
        // - Try again immediately (busy wait - bad!)
        // - Use epoll/select to wait for readability
        // - Do other work, try later

TIME: ~1-2 microseconds
NO BLOCKING, NO I/O, JUST ERROR RETURN
```

#### Blocking: Wait for Data

```
USER:   read(fd, buf, 4096)  [Blocking, data NOT ready]
          ↓
        [Same path until data not ready check...]
          ↓
PAGE    if (!folio_test_uptodate(folio)) {
CACHE:      // Blocking path:

            if (!folio_trylock(folio)) {
                // Can't get lock, need to wait

                // Check: Non-blocking?
                if (iocb->ki_flags & IOCB_NOWAIT)
                    return -EAGAIN;  // ← Non-blocking would exit here

                // Blocking: WAIT for lock
                folio_put_wait_locked(folio, TASK_KILLABLE);
                  ↓
                folio_wait_bit_common(folio, PG_locked, TASK_KILLABLE, ...)
                  ↓
WAIT        q = folio_waitqueue(folio)  ← Hash to wait queue
QUEUE:        ↓
            spin_lock(&q->lock)
              ↓
            __add_wait_queue_entry_tail(q, &wait)  ← Add to list
              ↓
            spin_unlock(&q->lock)
              ↓
SCHED:      set_current_state(TASK_KILLABLE)  ← Change state
              ↓
            io_schedule()  ← Give up CPU
              ↓
            schedule()
              ↓
            *** THREAD BLOCKED, OFF CPU ***
              ↓
            ... milliseconds pass, I/O completes ...
              ↓
IRQ:        folio_unlock(folio)  ← I/O complete, interrupt handler
              ↓
            folio_wake_bit(folio, PG_locked)
              ↓
            __wake_up_locked_key(q, TASK_NORMAL, &key)
              ↓
            wake_page_function()  ← Walk wait queue
              ↓
            try_to_wake_up(task)  ← Add back to runqueue
              ↓
            *** THREAD RUNNABLE AGAIN ***
              ↓
            [Scheduler gives CPU back to thread]
              ↓
RESUME:     [Resume after io_schedule()]
              ↓
            Check WQ_FLAG_WOKEN → set!
              ↓
            Break out of wait loop
              ↓
            finish_wait(q, &wait)
        }
          ↓
        // Now page is ready, copy data
          ↓
COPY:   copy_folio_to_iter(folio, offset, bytes, iter)
          ↓
USER:   read() returns 4096  ← SUCCESS (finally!)

TIME: ~1-100+ milliseconds (depends on disk speed)
FULL BLOCKING MACHINERY USED
```

#### Key Difference Points

**Where they diverge** (`mm/filemap.c:2608-2640`):
```c
// In filemap_get_pages():

if (!folio_test_uptodate(folio)) {
    // Data not ready, I/O in progress or not started

    // ===== DIVERGENCE POINT =====

    if (iocb->ki_flags & IOCB_NOWAIT) {
        // NON-BLOCKING PATH
        folio_put(folio);
        folio_batch_release(fbatch);
        return -EAGAIN;  // ← Return immediately

        // Thread never gives up CPU
        // No wait queues, no scheduler, no context switch
        // Just error return

    } else {
        // BLOCKING PATH
        error = filemap_update_page(iocb, mapping, folio, ...);
          ↓
        if (!folio_trylock(folio)) {
            // Need to wait for lock
            folio_put_wait_locked(folio, TASK_KILLABLE);
              ↓
            folio_wait_bit_common(...)
              ↓
            // Add to wait queue
            // Change task state
            // Call scheduler
            // Give up CPU
            // BLOCK
        }
    }
}
```

**Lock acquisition** (`mm/filemap.c:2446-2456`):
```c
static int filemap_update_page(struct kiocb *iocb, ...)
{
    int error;

    // Try to get invalidate lock
    if (iocb->ki_flags & IOCB_NOWAIT) {
        // Non-blocking: TRYLOCK only
        if (!filemap_invalidate_trylock_shared(mapping))
            return -EAGAIN;  // ← Can't get lock, fail immediately
    } else {
        // Blocking: Wait for lock
        filemap_invalidate_lock_shared(mapping);
    }

    // Try to lock the page
    if (!folio_trylock(folio)) {
        error = -EAGAIN;

        if (iocb->ki_flags & (IOCB_NOWAIT | IOCB_NOIO))
            goto unlock_mapping;  // ← Non-blocking: give up

        // Blocking: WAIT for page lock
        folio_put_wait_locked(folio, TASK_KILLABLE);
        return AOP_TRUNCATED_PAGE;
    }

    // ... rest of function
}
```

**Disk read decision** (`mm/filemap.c:2480-2486`):
```c
// In filemap_update_page():

// Check if data is ready
if (filemap_range_uptodate(mapping, iocb->ki_pos, count, folio, need_uptodate))
    goto unlock;  // Data ready, continue

// Data not ready, need to read from disk
error = -EAGAIN;
if (iocb->ki_flags & (IOCB_NOIO | IOCB_NOWAIT | IOCB_WAITQ))
    goto unlock;  // ← Non-blocking: refuse to do I/O

// Blocking: Read from disk and wait
error = filemap_read_folio(iocb->ki_filp, mapping->a_ops->read_folio, folio);
  ↓
[Submit I/O to disk]
  ↓
folio_wait_locked_killable(folio)  ← BLOCK until I/O completes
```

---

## Blocking vs Non-Blocking Comparison

### Side-by-Side Code Paths

```
┌────────────────────────────────────────────────────────────────────┐
│                         DATA READY                                 │
├────────────────────────────────────────────────────────────────────┤
│  BLOCKING              │  NON-BLOCKING                             │
│                        │                                           │
│  read(fd, buf, 4096)   │  read(fd, buf, 4096)  [O_NONBLOCK]       │
│    ↓                   │    ↓                                      │
│  ksys_read()           │  ksys_read()                              │
│    ↓                   │    ↓                                      │
│  vfs_read()            │  vfs_read()                               │
│    ↓                   │    ↓                                      │
│  filemap_read()        │  filemap_read()                           │
│    ↓                   │    ↓                                      │
│  xas_load() → FOUND    │  xas_load() → FOUND                       │
│    ↓                   │    ↓                                      │
│  uptodate? YES         │  uptodate? YES                            │
│    ↓                   │    ↓                                      │
│  copy_to_user()        │  copy_to_user()                           │
│    ↓                   │    ↓                                      │
│  return 4096           │  return 4096                              │
│                        │                                           │
│  ✅ SAME PATH          │  ✅ SAME PATH                             │
└────────────────────────────────────────────────────────────────────┘

┌────────────────────────────────────────────────────────────────────┐
│                       DATA NOT READY                               │
├────────────────────────────────────────────────────────────────────┤
│  BLOCKING              │  NON-BLOCKING                             │
│                        │                                           │
│  read(fd, buf, 4096)   │  read(fd, buf, 4096)  [O_NONBLOCK]       │
│    ↓                   │    ↓                                      │
│  ksys_read()           │  ksys_read()                              │
│    ↓                   │    ↓                                      │
│  vfs_read()            │  vfs_read()                               │
│    ↓                   │    ↓                                      │
│  filemap_read()        │  filemap_read()                           │
│    ↓                   │    ↓                                      │
│  xas_load() → FOUND    │  xas_load() → FOUND                       │
│    ↓                   │    ↓                                      │
│  uptodate? NO          │  uptodate? NO                             │
│    ↓                   │    ↓                                      │
│  ╔══════════════════╗  │  if (IOCB_NOWAIT)                         │
│  ║ DIVERGENCE POINT ║  │    return -EAGAIN  ← DONE!                │
│  ╚══════════════════╝  │    ↓                                      │
│    ↓                   │  return -1, errno=EAGAIN                  │
│  folio_trylock()       │                                           │
│    ↓                   │  TIME: ~1-2 μs                            │
│  Failed!               │  NO BLOCKING                              │
│    ↓                   │  NO WAIT QUEUES                           │
│  folio_wait_locked()   │  NO SCHEDULER                             │
│    ↓                   │  NO CONTEXT SWITCH                        │
│  q = folio_waitqueue() │                                           │
│    ↓                   │                                           │
│  __add_wait_queue()    │                                           │
│    ↓                   │                                           │
│  set_state(KILLABLE)   │                                           │
│    ↓                   │                                           │
│  io_schedule()         │                                           │
│    ↓                   │                                           │
│  *** BLOCKED ***       │                                           │
│    ↓                   │                                           │
│  [I/O completes]       │                                           │
│    ↓                   │                                           │
│  wake_up()             │                                           │
│    ↓                   │                                           │
│  *** RESUME ***        │                                           │
│    ↓                   │                                           │
│  copy_to_user()        │                                           │
│    ↓                   │                                           │
│  return 4096           │                                           │
│                        │                                           │
│  TIME: ~1-100+ ms      │                                           │
│  FULL BLOCKING         │                                           │
└────────────────────────────────────────────────────────────────────┘
```

### Operations Comparison

| Operation | Blocking (Data Not Ready) | Non-Blocking (Data Not Ready) | Both (Data Ready) |
|-----------|---------------------------|-------------------------------|-------------------|
| **Syscall entry** | ✅ | ✅ | ✅ |
| **FD → file*** | ✅ | ✅ | ✅ |
| **VFS routing** | ✅ | ✅ | ✅ |
| **Page cache lookup** | ✅ | ✅ | ✅ |
| **Check uptodate** | ✅ | ✅ | ✅ |
| **Wait queue alloc** | ✅ | ❌ | ❌ |
| **Add to wait queue** | ✅ | ❌ | ❌ |
| **Set task state** | ✅ | ❌ | ❌ |
| **Call scheduler** | ✅ | ❌ | ❌ |
| **Context switch out** | ✅ | ❌ | ❌ |
| **Disk I/O** | ✅ | ❌ | ❌ |
| **Wait for I/O** | ✅ | ❌ | ❌ |
| **Interrupt handling** | ✅ | ❌ | ❌ |
| **Wake-up traversal** | ✅ | ❌ | ❌ |
| **Context switch in** | ✅ | ❌ | ❌ |
| **Copy to userspace** | ✅ | ❌ | ✅ |
| **Return success** | ✅ | ❌ | ✅ |
| **Return -EAGAIN** | ❌ | ✅ | ❌ |

---

## Implementation Details

### Kernel Flags

**File-level flag** (`include/uapi/asm-generic/fcntl.h:38`):
```c
#define O_NONBLOCK   00004000
```

**I/O operation flag** (`include/linux/fs.h:355`):
```c
#define IOCB_NOWAIT  (__force int) RWF_NOWAIT
#define IOCB_NOIO    (1 << 20)
```

### Flag Propagation

```
USER:
  fd = open(path, O_RDONLY | O_NONBLOCK)
    ↓
  file->f_flags |= O_NONBLOCK
    ↓

USER:
  read(fd, buf, count)
    ↓

KERNEL (fs/read_write.c:389-434):
  new_sync_read(file, buf, len, ppos)
    ↓
  struct kiocb kiocb;
  init_sync_kiocb(&kiocb, file);
    ↓
  if (file->f_flags & O_NONBLOCK)
      kiocb.ki_flags |= IOCB_NOWAIT;  ← Translate to I/O flag
    ↓
  call_read_iter(file, &kiocb, &iter)
    ↓

Throughout I/O path:
  if (iocb->ki_flags & IOCB_NOWAIT) {
      // Use trylock, return -EAGAIN if would block
  }
```

### Check Points in Code

The kernel checks `IOCB_NOWAIT` at multiple points to avoid blocking:

#### 1. Invalidate Lock (`mm/filemap.c:2446-2451`)

```c
if (iocb->ki_flags & IOCB_NOWAIT) {
    if (!filemap_invalidate_trylock_shared(mapping))
        return -EAGAIN;  // Can't get lock
} else {
    filemap_invalidate_lock_shared(mapping);  // Wait for lock
}
```

#### 2. Page Lock (`mm/filemap.c:2453-2465`)

```c
if (!folio_trylock(folio)) {
    error = -EAGAIN;
    if (iocb->ki_flags & (IOCB_NOWAIT | IOCB_NOIO))
        goto unlock_mapping;  // Can't lock page

    // Blocking: wait for lock
    folio_put_wait_locked(folio, TASK_KILLABLE);
}
```

#### 3. Page Uptodate Check (`mm/filemap.c:2608-2625`)

```c
if (!folio_test_uptodate(folio)) {
    if (iocb->ki_flags & IOCB_NOWAIT) {
        folio_put(folio);
        folio_batch_release(fbatch);
        return -EAGAIN;  // Data not ready
    }

    // Blocking: wait for data
}
```

#### 4. Disk Read (`mm/filemap.c:2480-2486`)

```c
if (!filemap_range_uptodate(...)) {
    error = -EAGAIN;
    if (iocb->ki_flags & (IOCB_NOIO | IOCB_NOWAIT | IOCB_WAITQ))
        goto unlock;  // Won't initiate I/O

    // Blocking: read from disk
    error = filemap_read_folio(...);
}
```

#### 5. Page Creation (`mm/filemap.c:2504-2505`)

```c
if (iocb->ki_flags & (IOCB_NOWAIT | IOCB_WAITQ))
    return -EAGAIN;  // Won't allocate page and read

// Blocking: allocate and read
folio = filemap_alloc_folio(...);
```

#### 6. Readahead (`mm/filemap.c:2555-2556`)

```c
if (iocb->ki_flags & IOCB_NOIO)
    return -EAGAIN;  // Won't trigger readahead

// Trigger readahead
page_cache_async_ra(...);
```

#### 7. Writeback Wait (`mm/filemap.c:2785-2788`)

```c
if (iocb->ki_flags & IOCB_NOWAIT) {
    if (filemap_range_needs_writeback(mapping, pos, end))
        return -EAGAIN;  // Won't wait for writeback
}
```

### What Non-Blocking Avoids

```c
// All of these are NEVER called in non-blocking mode:

❌ folio_wait_bit_common()          // The blocking primitive
❌ folio_put_wait_locked()          // Wait for page lock
❌ folio_wait_locked_killable()     // Wait for page lock
❌ __folio_lock()                   // Lock page (blocking)
❌ __add_wait_queue_entry_tail()    // Add to wait queue
❌ set_current_state()              // Change task state
❌ io_schedule()                    // Give up CPU
❌ schedule()                       // Invoke scheduler
❌ try_to_wake_up()                 // Wake sleeping task
❌ filemap_read_folio()             // Read from disk (in non-blocking mode)
❌ submit_bio()                     // Submit disk I/O
```

### What Non-Blocking Uses

```c
// Only fast, non-blocking operations:

✅ xas_load()                       // XArray lookup (hash table)
✅ folio_try_get()                  // Atomic refcount increment
✅ folio_trylock()                  // Try to lock (doesn't wait)
✅ folio_test_uptodate()            // Check flag (atomic read)
✅ copy_folio_to_iter()             // Copy data
✅ copy_to_user()                   // Copy to userspace
✅ folio_put()                      // Atomic refcount decrement
✅ return -EAGAIN                   // Error return
```

---

## Use Cases and Patterns

### Anti-Pattern: Busy Waiting (BAD!)

```c
// ❌ DON'T DO THIS - wastes CPU!
int fd = open("/dev/ttyS0", O_RDONLY | O_NONBLOCK);

while (1) {
    ssize_t n = read(fd, buf, sizeof(buf));
    if (n > 0) {
        process_data(buf, n);
        break;
    } else if (n == -1 && errno == EAGAIN) {
        // Spin! Waste CPU!
        continue;
    } else {
        perror("read");
        break;
    }
}
```

**Problem**: 100% CPU usage, no other work can be done

### Pattern 1: Event Loop with epoll (GOOD)

```c
#include <sys/epoll.h>

int epollfd = epoll_create1(0);

// Add file descriptor to epoll
struct epoll_event ev = {
    .events = EPOLLIN,
    .data.fd = fd
};
epoll_ctl(epollfd, EPOLL_CTL_ADD, fd, &ev);

// Set non-blocking
fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) | O_NONBLOCK);

// Event loop
struct epoll_event events[MAX_EVENTS];
while (1) {
    // Wait for events (blocking!)
    int nfds = epoll_wait(epollfd, events, MAX_EVENTS, -1);

    for (int i = 0; i < nfds; i++) {
        int fd = events[i].data.fd;

        // Data is ready, non-blocking read will succeed
        ssize_t n = read(fd, buf, sizeof(buf));
        if (n > 0) {
            process_data(buf, n);
        } else if (n == -1 && errno != EAGAIN) {
            perror("read");
        }
        // If EAGAIN, just continue (spurious wakeup)
    }
}
```

**Advantages**:
- One thread handles many file descriptors
- No busy waiting (epoll_wait blocks)
- Scales to thousands of connections
- Low CPU usage

### Pattern 2: Try Read, Fall Back to epoll

```c
// Try non-blocking read first
ssize_t n = read(fd, buf, sizeof(buf));

if (n > 0) {
    // Success! Data was ready
    process_data(buf, n);

} else if (n == -1 && errno == EAGAIN) {
    // Not ready, add to epoll for notification
    struct epoll_event ev = {
        .events = EPOLLIN | EPOLLET,  // Edge-triggered
        .data.fd = fd
    };
    epoll_ctl(epollfd, EPOLL_CTL_ADD, fd, &ev);

    // Will be notified when data arrives

} else {
    // Real error
    perror("read");
}
```

### Pattern 3: Single-Threaded Server (Node.js-style)

```c
int server_fd = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK, 0);
bind(server_fd, ...);
listen(server_fd, SOMAXCONN);

int epollfd = epoll_create1(0);
epoll_ctl(epollfd, EPOLL_CTL_ADD, server_fd, &ev_server);

while (1) {
    int nfds = epoll_wait(epollfd, events, MAX_EVENTS, -1);

    for (int i = 0; i < nfds; i++) {
        if (events[i].data.fd == server_fd) {
            // New connection
            int client_fd = accept4(server_fd, ..., SOCK_NONBLOCK);
            epoll_ctl(epollfd, EPOLL_CTL_ADD, client_fd, &ev_client);

        } else {
            // Client has data
            int client_fd = events[i].data.fd;
            ssize_t n = read(client_fd, buf, sizeof(buf));

            if (n > 0) {
                // Process request, send response
                process_request(buf, n);
                write(client_fd, response, response_len);
            } else if (n == 0) {
                // EOF, client closed
                epoll_ctl(epollfd, EPOLL_CTL_DEL, client_fd, NULL);
                close(client_fd);
            }
        }
    }
}
```

**Scales to**: 10,000+ concurrent connections on a single thread!

### Pattern 4: io_uring (Modern Async I/O)

```c
#include <liburing.h>

struct io_uring ring;
io_uring_queue_init(QUEUE_DEPTH, &ring, 0);

// Submit non-blocking read
struct io_uring_sqe *sqe = io_uring_get_sqe(&ring);
io_uring_prep_read(sqe, fd, buf, sizeof(buf), offset);
sqe->flags |= IOSQE_ASYNC;  // Non-blocking
io_uring_submit(&ring);

// Wait for completion
struct io_uring_cqe *cqe;
io_uring_wait_cqe(&ring, &cqe);

if (cqe->res > 0) {
    // Success
    process_data(buf, cqe->res);
} else if (cqe->res == -EAGAIN) {
    // Would block
    retry_later();
}

io_uring_cqe_seen(&ring, cqe);
```

---

## Performance Characteristics

### Time Complexity

| Scenario | Blocking | Non-Blocking |
|----------|----------|--------------|
| **Data ready** | ~1-5 μs | ~1-5 μs (same) |
| **Data not ready** | ~1-100+ ms | ~1-2 μs |
| **Lock contention** | Wait (variable) | Return -EAGAIN (~1 μs) |

### Space Complexity

| Resource | Blocking | Non-Blocking |
|----------|----------|--------------|
| **Thread stack** | ~8 MB per thread | ~8 MB for one thread |
| **Wait queue entry** | ~64 bytes per waiter | 0 bytes |
| **Task struct overhead** | Scheduler metadata | No overhead |

### Scalability

**Blocking I/O** (one thread per connection):
```
1,000 connections = 1,000 threads
Memory: 1,000 × 8 MB = 8 GB stack space
Context switches: High
Scheduler overhead: High
```

**Non-Blocking I/O** (event loop):
```
10,000 connections = 1 thread
Memory: 8 MB stack + FD overhead
Context switches: None for I/O
Scheduler overhead: Minimal
```

### CPU Usage

**Blocking I/O**:
- Waiting: 0% CPU (thread asleep)
- Active: Normal CPU usage
- Context switches: Cost cycles

**Non-Blocking I/O** (with epoll):
- Waiting: 0% CPU (epoll_wait blocks)
- Active: Normal CPU usage
- No I/O context switches: Better cache locality

**Non-Blocking I/O** (busy polling - anti-pattern):
- Waiting: 100% CPU (spinning!)
- Active: Normal CPU usage
- Very bad unless latency is critical

### Throughput

Assuming 1,000 requests/sec, 10ms average I/O time:

**Blocking** (1 thread per request):
```
Active threads: 1,000 req/s × 0.01s = 10 threads needed
With safety margin: ~50-100 threads
Thread creation overhead: High
```

**Non-Blocking** (event loop):
```
Active threads: 1 thread
No creation overhead
Can handle 10,000+ req/s on one thread
```

---

## Complete Code Traces

### Trace 1: Non-Blocking Read, Data Ready

**File**: mm/filemap.c, fs/read_write.c

```
Line  File                      Function                          What Happens
────────────────────────────────────────────────────────────────────────────────
USER  app.c                     main()                            read(fd, buf, 4096)
                                                                  [O_NONBLOCK set]
────────────────────────────────────────────────────────────────────────────────
720   fs/read_write.c           SYSCALL_DEFINE3(read)             Syscall entry
702   fs/read_write.c           ksys_read()
713   fs/read_write.c           CLASS(fd_pos, f)(fd)              fd → file*
715   fs/read_write.c           vfs_read()                        Call VFS
────────────────────────────────────────────────────────────────────────────────
476   fs/read_write.c           vfs_read()
389   fs/read_write.c           new_sync_read()
415   fs/read_write.c           kiocb.ki_flags |= IOCB_NOWAIT     ← Set from O_NONBLOCK
423   fs/read_write.c           call_read_iter()
────────────────────────────────────────────────────────────────────────────────
2933  mm/filemap.c              generic_file_read_iter()
2664  mm/filemap.c              filemap_read()
2701  mm/filemap.c              filemap_get_pages()
────────────────────────────────────────────────────────────────────────────────
2563  mm/filemap.c              filemap_get_pages()
2580  mm/filemap.c              filemap_get_read_batch()
2354  mm/filemap.c              filemap_get_read_batch()
────────────────────────────────────────────────────────────────────────────────
2357  mm/filemap.c              XA_STATE(xas, &i_pages, index)    Setup XArray iter
2360  mm/filemap.c              rcu_read_lock()
2361  mm/filemap.c              folio = xas_load(&xas)            ← LOOKUP page cache
                                                                  *** FOUND! ***
2368  mm/filemap.c              folio_try_get(folio)              Increment refcount
2374  mm/filemap.c              folio_batch_add(fbatch, folio)    Add to batch
2376  mm/filemap.c              if (!folio_test_uptodate())       Check if ready
                                                                  *** YES, READY! ***
2387  mm/filemap.c              rcu_read_unlock()
────────────────────────────────────────────────────────────────────────────────
[Return to filemap_read()]
2732  mm/filemap.c              for each folio in batch:
2743  mm/filemap.c              folio_mark_accessed(folio)        LRU update
2752  mm/filemap.c              copy_folio_to_iter()              ← COPY to userspace
                                                                  copy_to_user()
2754  mm/filemap.c              already_read += copied
2755  mm/filemap.c              iocb->ki_pos += copied
────────────────────────────────────────────────────────────────────────────────
2768  mm/filemap.c              folio_put(folio)                  Release refcount
2775  mm/filemap.c              return already_read               return 4096
────────────────────────────────────────────────────────────────────────────────
[Unwind call stack]
USER  app.c                     main()                            read() returns 4096
                                                                  ✅ SUCCESS!
────────────────────────────────────────────────────────────────────────────────
TIME: ~1-5 microseconds
NO BLOCKING, NO I/O, NO CONTEXT SWITCH
```

### Trace 2: Non-Blocking Read, Data Not Ready

**File**: mm/filemap.c

```
Line  Function                          What Happens
────────────────────────────────────────────────────────────────────────────────
[... same as above until page cache lookup ...]
────────────────────────────────────────────────────────────────────────────────
2361  folio = xas_load(&xas)            ← LOOKUP page cache
                                        *** FOUND! ***
2368  folio_try_get(folio)              Increment refcount
2374  folio_batch_add(fbatch, folio)    Add to batch
2376  if (!folio_test_uptodate(folio))  Check if ready
                                        *** NO! I/O in progress ***
      break;                            Stop here
────────────────────────────────────────────────────────────────────────────────
[Return to filemap_get_pages()]
2608  if (!folio_test_uptodate(folio))  Data not ready
      {
2611    if (iocb->ki_flags & IOCB_NOWAIT)  ← CHECK: Non-blocking?
        {
          folio_put(folio);             Release refcount
          folio_batch_release(fbatch);
          return -EAGAIN;               ← RETURN ERROR IMMEDIATELY
        }
      }
────────────────────────────────────────────────────────────────────────────────
[Unwind call stack with error]
2775  return error                      return -EAGAIN
────────────────────────────────────────────────────────────────────────────────
USER  read() returns -1                 errno = EAGAIN
                                        ⚠️ WOULD BLOCK
────────────────────────────────────────────────────────────────────────────────
TIME: ~1-2 microseconds
NO BLOCKING, NO I/O, JUST ERROR RETURN

Application must:
  - Use epoll/select to wait for readability, OR
  - Try again later, OR
  - Do other work
```

### Trace 3: Blocking Read, Data Not Ready (for comparison)

**File**: mm/filemap.c

```
Line  Function                          What Happens
────────────────────────────────────────────────────────────────────────────────
[... same until uptodate check ...]
────────────────────────────────────────────────────────────────────────────────
2376  if (!folio_test_uptodate(folio))  Data not ready
2608  if (!folio_test_uptodate(folio))
      {
2611    if (iocb->ki_flags & IOCB_NOWAIT)  ← CHECK: Non-blocking?
                                        *** NO, blocking mode ***

        // Non-blocking would exit here

        // Blocking path continues:
2444    filemap_update_page()
2453    if (!folio_trylock(folio))      Can't lock page
        {
2455      if (iocb->ki_flags & IOCB_NOWAIT)  ← Non-blocking would exit
                                        *** But we're blocking ***

2463      folio_put_wait_locked(folio)  ← WAIT for lock
1469      folio_wait_locked_killable()
1238      folio_wait_bit_common()       ═══ THE BLOCKING FUNCTION ═══
────────────────────────────────────────────────────────────────────────────────
1241      q = folio_waitqueue(folio)    Hash to wait queue #42
1256      init_wait(&wait)
1257      wait->func = wake_page_function
1283      spin_lock_irq(&q->lock)
1286      __add_wait_queue_entry_tail(q, &wait)  ← ADD TO LINKED LIST
1287      spin_unlock_irq(&q->lock)
────────────────────────────────────────────────────────────────────────────────
1309      set_current_state(TASK_KILLABLE)  ← CHANGE STATE
1312      flags = smp_load_acquire(&wait->flags)
1313      if (!(flags & WQ_FLAG_WOKEN))  Not woken yet
1317        io_schedule()               ═══ GIVE UP CPU ═══
                                        *** THREAD BLOCKED ***
────────────────────────────────────────────────────────────────────────────────
          ... milliseconds pass ...
          ... disk I/O completes ...
          ... interrupt handler runs ...
────────────────────────────────────────────────────────────────────────────────
1493  folio_unlock(folio)               I/O complete
1500  folio_wake_bit(folio, PG_locked)
1177  q = folio_waitqueue(folio)        ← SAME QUEUE!
1186  __wake_up_locked_key(q, ...)
73    __wake_up_common(wq_head, ...)    Walk wait queue
89    curr->func(curr, ...)             ← Call wake_page_function
1124  wake_page_function()
1158  smp_store_release(&wait->flags, WQ_FLAG_WOKEN)  ← SET FLAG
1159  wake_up_state(wait->private)
7099  try_to_wake_up(task)              ═══ ADD TO RUNQUEUE ═══
────────────────────────────────────────────────────────────────────────────────
          ... scheduler picks our thread ...
────────────────────────────────────────────────────────────────────────────────
1317  [Resume after io_schedule()]      *** GOT CPU BACK ***
1312  flags = smp_load_acquire(&wait->flags)  Check flag
      if (flags & WQ_FLAG_WOKEN)        *** YES! Woken! ***
        break;                          Exit loop
────────────────────────────────────────────────────────────────────────────────
1347  finish_wait(q, &wait)             Cleanup
      return 0                          Success
────────────────────────────────────────────────────────────────────────────────
[Now page is ready, copy data]
2752  copy_folio_to_iter()              Copy to userspace
2775  return already_read               return 4096
────────────────────────────────────────────────────────────────────────────────
USER  read() returns 4096               ✅ SUCCESS (finally!)
────────────────────────────────────────────────────────────────────────────────
TIME: ~1-100+ milliseconds
FULL BLOCKING MACHINERY USED
```

---

## Key Takeaways

### 1. **Non-Blocking is Simpler (in Kernel)**

Non-blocking I/O **avoids** the complex blocking machinery:
```c
❌ NO wait queues
❌ NO linked list operations
❌ NO task state changes
❌ NO scheduler calls
❌ NO context switches
❌ NO wake-up traversals

✅ Just: check ready → if not, return -EAGAIN
```

### 2. **Complexity Moves to Userspace**

Kernel is simpler, but application becomes complex:
```c
// Blocking (simple app):
read(fd, buf, 1024);  // Just works

// Non-blocking (complex app):
while (1) {
    n = epoll_wait(...);
    for (i = 0; i < n; i++) {
        // State machine for each FD
        // Handle EAGAIN
        // Manage buffers
        // Track partial reads
    }
}
```

### 3. **Same Path When Data Ready**

Both blocking and non-blocking follow **exactly the same code path** when data is ready in page cache:
- Same XArray lookup
- Same uptodate check
- Same copy_to_user
- Same return

**Performance is identical** in fast path!

### 4. **Divergence Only When Blocking Would Occur**

The paths only diverge when:
- Lock can't be acquired
- Page not uptodate
- Need to read from disk

At these points:
- **Blocking**: Calls wait queue machinery
- **Non-Blocking**: Returns -EAGAIN immediately

### 5. **Multiple Check Points**

Kernel checks `IOCB_NOWAIT` at **many** points:
1. Invalidate lock acquisition
2. Page lock acquisition
3. Page uptodate check
4. Disk I/O initiation
5. Page allocation
6. Readahead triggering
7. Writeback waiting

Each is a potential "would block" point.

### 6. **Error Code is the Signal**

```c
ssize_t n = read(fd, buf, 1024);

if (n > 0) {
    // SUCCESS: Got data

} else if (n == 0) {
    // EOF: Connection closed

} else if (n == -1) {
    if (errno == EAGAIN || errno == EWOULDBLOCK) {
        // WOULD BLOCK: Not an error, just not ready
        // Use epoll or try later

    } else {
        // REAL ERROR: Something went wrong
        perror("read");
    }
}
```

### 7. **Use with Event Notification**

Non-blocking I/O is **useless without event notification**:

```c
// ❌ BAD: Busy wait
while (read(fd, buf, 1024) == -1 && errno == EAGAIN)
    ;  // Spin, waste CPU

// ✅ GOOD: Event-driven
epoll_ctl(epollfd, EPOLL_CTL_ADD, fd, &ev);
while (1) {
    epoll_wait(epollfd, events, MAX_EVENTS, -1);
    // Now we know fd is ready
    read(fd, buf, 1024);  // Won't return EAGAIN (usually)
}
```

### 8. **Scalability Win**

**One thread can handle thousands of connections**:

```
Blocking:
  1,000 connections → 1,000 threads → 8 GB RAM + high overhead

Non-Blocking:
  10,000 connections → 1 thread → 8 MB RAM + minimal overhead
```

This is why all high-performance servers (nginx, Redis, Node.js) use non-blocking I/O.

### 9. **Latency Considerations**

- **Blocking**: Woken immediately when ready (interrupt → wake)
- **Non-Blocking with epoll**: Latency depends on epoll_wait timeout
- **Non-Blocking busy poll**: Zero latency, but 100% CPU

Choose based on requirements:
- **Normal servers**: epoll with timeout (good latency, low CPU)
- **Ultra low-latency**: busy poll (trading CPU for nanoseconds)
- **Simple applications**: blocking (simplest code)

### 10. **Modern Alternative: io_uring**

io_uring provides:
- Truly async I/O (submission queue + completion queue)
- No syscalls in fast path
- Kernel-side polling option
- Best of both worlds: simple application + async + performance

But still built on the same `IOCB_NOWAIT` mechanism under the hood!

---

## Conclusion

Non-blocking I/O is **fundamentally simpler in the kernel** than blocking I/O:

**Blocking I/O** (when data not ready):
1. Allocate wait queue entry
2. Add to linked list
3. Change task state
4. Call scheduler
5. Context switch out
6. Wait for I/O
7. Interrupt handler
8. Walk wait queue
9. Wake task
10. Context switch in
11. Copy data
12. Return

**Non-Blocking I/O** (when data not ready):
1. Check if ready
2. Not ready → return -EAGAIN

The **complexity moves to userspace**, where applications must:
- Use event notification (epoll/select/io_uring)
- Manage state machines for partial I/O
- Handle EAGAIN appropriately
- Coordinate multiple file descriptors

But the **scalability win is enormous**: one thread can handle 10,000+ connections, versus needing 10,000 threads with blocking I/O.

This is why every high-performance network server uses non-blocking I/O with an event loop.
