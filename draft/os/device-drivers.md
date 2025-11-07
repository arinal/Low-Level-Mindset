# Linux Device Drivers: Everything is a File

This document explains how Linux device drivers work, how devices appear as files in `/dev`, and how the VFS (Virtual File System) layer connects userspace applications to kernel drivers.

## Table of Contents

- [High-Level Overview](#high-level-overview)
- [The Device Model](#the-device-model)
- [Everything is a File](#everything-is-a-file)
- [The VFS Layer](#the-vfs-layer)
- [Character Devices](#character-devices)
- [Block Devices](#block-devices)
- [Network Devices](#network-devices)
- [Writing a Simple Character Driver](#writing-a-simple-character-driver)
- [Complete System Call Path](#complete-system-call-path)
- [Key Data Structures](#key-data-structures)
- [Device Registration](#device-registration)
- [Device Files in /dev](#device-files-in-dev)
- [Visual Architecture](#visual-architecture)
- [Key Takeaways](#key-takeaways)
- [Quick Reference](#quick-reference)

---

## High-Level Overview

Linux follows the **"everything is a file"** philosophy. Devices (hardware) are accessed through file-like interfaces in `/dev`. When you `open()`, `read()`, or `write()` a device file, the kernel routes these operations to the appropriate device driver.

**The magic happens through**:
1. **VFS (Virtual File System)** - Provides a uniform interface for all file operations
2. **Device drivers** - Kernel modules that control hardware
3. **Device files** - Special files in `/dev` that represent devices
4. **Device numbers** - Major/minor numbers that identify devices

**Example flow**:
```
Application: open("/dev/mydevice", O_RDWR)
    ↓
System call: sys_open()
    ↓
VFS layer: Looks up /dev/mydevice
    ↓
Device subsystem: Routes to driver based on major number
    ↓
Driver: my_device_open() function executes
    ↓
Hardware: Driver initializes hardware
```

---

## The Device Model

Linux categorizes devices into three main types:

### 1. Character Devices (char devices)
**Description**: Stream of bytes, accessed sequentially
**Examples**: `/dev/tty`, `/dev/random`, `/dev/null`, `/dev/input/event0`
**Interface**: `read()`, `write()`, `ioctl()`

```
Userspace app → open("/dev/ttyS0") → Serial port driver → UART hardware
```

### 2. Block Devices
**Description**: Random access, fixed-size blocks (512/4096 bytes)
**Examples**: `/dev/sda`, `/dev/nvme0n1`, `/dev/loop0`
**Interface**: `read()`, `write()`, filesystem operations
**Note**: Block layer provides caching and scheduling

```
Userspace app → open("/dev/sda1") → Block layer → Disk driver → SATA/NVMe hardware
```

### 3. Network Devices
**Description**: Packet-based communication
**Examples**: `eth0`, `wlan0`, `lo`
**Interface**: Socket API (not file-based!)
**Note**: No `/dev` entry, managed differently

```
Userspace app → socket() → Network stack → Network driver → Ethernet hardware
```

---

## Everything is a File

### The Philosophy

In Linux, devices are represented as files in `/dev`:

```bash
$ ls -l /dev/
crw-rw-rw- 1 root root   1,   3 Nov  5 10:00 null
crw------- 1 root root  10,  63 Nov  5 10:00 vga_arbiter
brw-rw---- 1 root disk   8,   0 Nov  5 10:00 sda
crw-rw----+ 1 root video 226,  0 Nov  5 10:00 card0
```

**Notice**:
- First character: `c` = character device, `b` = block device
- Two numbers: **major** and **minor** device numbers
  - Major (226): Identifies the driver
  - Minor (0): Identifies the specific device instance

### Device Numbers

```c
// include/linux/kdev_t.h
typedef u32 dev_t;

#define MINORBITS   20
#define MINORMASK   ((1U << MINORBITS) - 1)

#define MAJOR(dev)  ((unsigned int) ((dev) >> MINORBITS))
#define MINOR(dev)  ((unsigned int) ((dev) & MINORMASK))
#define MKDEV(ma,mi) (((ma) << MINORBITS) | (mi))
```

**Example**: `/dev/sda` has major=8, minor=0
- Major 8 → SCSI disk driver
- Minor 0 → First disk

**Example**: `/dev/sda1` has major=8, minor=1
- Major 8 → Same driver
- Minor 1 → First partition

### How Device Files Work

Device files are **not regular files**! They contain no data.

```c
// include/linux/fs.h
struct inode {
    umode_t         i_mode;     // File type and permissions
    dev_t           i_rdev;     // Device number (for device files)
    struct super_block *i_sb;   // Filesystem superblock

    const struct inode_operations *i_op;
    const struct file_operations *i_fop;  // ← Points to driver ops!

    // ... many other fields
};
```

When you `open("/dev/null")`:
1. VFS looks up inode for `/dev/null`
2. Sees it's a character device (from `i_mode`)
3. Extracts device number from `i_rdev` (major=1, minor=3)
4. Looks up driver registered for major=1
5. Calls that driver's `open()` function

---

## The VFS Layer

VFS is the abstraction layer that makes "everything is a file" work.

### Architecture

```
┌─────────────────────────────────────────────────────┐
│              Userspace Application                  │
│                                                     │
│  open("/dev/mydevice", O_RDWR);                    │
│  read(fd, buf, count);                             │
│  write(fd, buf, count);                            │
│  close(fd);                                        │
└────────────────┬────────────────────────────────────┘
                 │ System calls
                 ↓
┌─────────────────────────────────────────────────────┐
│              System Call Interface                  │
│                                                     │
│  sys_open(), sys_read(), sys_write(), sys_close()  │
└────────────────┬────────────────────────────────────┘
                 │
                 ↓
┌─────────────────────────────────────────────────────┐
│           VFS (Virtual File System)                 │
│                                                     │
│  - Path lookup (namei)                             │
│  - Inode cache                                     │
│  - Dentry cache                                    │
│  - File descriptor table                           │
│  - Generic file operations                         │
└─────┬────────────────┬────────────────┬─────────────┘
      │                │                │
      ↓                ↓                ↓
┌──────────┐   ┌──────────┐   ┌──────────────────┐
│   ext4   │   │   tmpfs  │   │ Device Drivers   │
│ (block)  │   │  (RAM)   │   │  (char/block)    │
└────┬─────┘   └────┬─────┘   └────┬─────────────┘
     │              │              │
     ↓              ↓              ↓
┌──────────┐   ┌──────────┐   ┌──────────────────┐
│  Block   │   │  Memory  │   │   Hardware       │
│  Device  │   │          │   │   Devices        │
└──────────┘   └──────────┘   └──────────────────┘
```

### Key VFS Structures

```c
// include/linux/fs.h

// Represents an open file
struct file {
    struct path             f_path;        // Dentry and vfsmount
    struct inode            *f_inode;      // Cached inode
    const struct file_operations *f_op;    // ← Function pointers!

    fmode_t                 f_mode;        // Read/write mode
    loff_t                  f_pos;         // Current position

    void                    *private_data; // Driver can store data here
};

// File operations - implemented by drivers!
struct file_operations {
    struct module *owner;

    loff_t (*llseek) (struct file *, loff_t, int);
    ssize_t (*read) (struct file *, char __user *, size_t, loff_t *);
    ssize_t (*write) (struct file *, const char __user *, size_t, loff_t *);

    int (*open) (struct inode *, struct file *);
    int (*release) (struct inode *, struct file *);

    long (*unlocked_ioctl) (struct file *, unsigned int, unsigned long);
    int (*mmap) (struct file *, struct vm_area_struct *);

    // ... many more
};
```

**Key insight**: Drivers implement `struct file_operations`, and VFS calls these functions!

---

## Character Devices

Character devices are the simplest type. Let's trace how they work.

### Character Device Subsystem

```c
// fs/char_dev.c

// Global array of character devices
static struct char_device_struct {
    struct char_device_struct *next;
    unsigned int major;
    unsigned int baseminor;
    int minorct;
    char name[64];
    struct cdev *cdev;      // ← Points to driver's cdev
} *chrdevs[CHRDEV_MAJOR_HASH_SIZE];
```

### The `cdev` Structure

```c
// include/linux/cdev.h

struct cdev {
    struct kobject kobj;
    struct module *owner;
    const struct file_operations *ops;  // ← Driver's operations!
    struct list_head list;
    dev_t dev;              // Device number
    unsigned int count;     // Number of minor numbers
};
```

### Registration Process

```c
// Driver initialization
static int __init my_driver_init(void)
{
    // 1. Allocate device number
    dev_t dev;
    alloc_chrdev_region(&dev, 0, 1, "mydevice");

    int major = MAJOR(dev);

    // 2. Initialize cdev
    struct cdev *my_cdev = cdev_alloc();
    my_cdev->ops = &my_fops;  // Assign our file_operations
    my_cdev->owner = THIS_MODULE;

    // 3. Register with kernel
    cdev_add(my_cdev, dev, 1);

    // 4. Create device file (via udev or manually)
    // mknod /dev/mydevice c <major> 0

    return 0;
}
```

---

## Writing a Simple Character Driver

Let's write a complete, minimal character device driver.

### Complete Driver Example

```c
// mydevice.c - Simple character device driver

#include <linux/module.h>
#include <linux/fs.h>
#include <linux/cdev.h>
#include <linux/uaccess.h>

#define DEVICE_NAME "mydevice"
#define BUFFER_SIZE 1024

// Driver state
static dev_t dev_num;           // Device number (major + minor)
static struct cdev *my_cdev;    // Character device structure
static char device_buffer[BUFFER_SIZE];
static int buffer_pos = 0;

// ============================================================================
// FILE OPERATIONS - These are called by VFS
// ============================================================================

static int my_open(struct inode *inode, struct file *filp)
{
    printk(KERN_INFO "mydevice: open() called\n");
    return 0;
}

static int my_release(struct inode *inode, struct file *filp)
{
    printk(KERN_INFO "mydevice: close() called\n");
    return 0;
}

static ssize_t my_read(struct file *filp, char __user *buf,
                       size_t count, loff_t *f_pos)
{
    ssize_t bytes_read = 0;

    printk(KERN_INFO "mydevice: read(%zu bytes) at pos %lld\n",
           count, *f_pos);

    // Check bounds
    if (*f_pos >= buffer_pos)
        return 0;  // EOF

    if (*f_pos + count > buffer_pos)
        count = buffer_pos - *f_pos;

    // Copy to userspace
    if (copy_to_user(buf, device_buffer + *f_pos, count))
        return -EFAULT;

    *f_pos += count;
    bytes_read = count;

    return bytes_read;
}

static ssize_t my_write(struct file *filp, const char __user *buf,
                        size_t count, loff_t *f_pos)
{
    ssize_t bytes_written = 0;

    printk(KERN_INFO "mydevice: write(%zu bytes)\n", count);

    // Check bounds
    if (buffer_pos + count > BUFFER_SIZE)
        count = BUFFER_SIZE - buffer_pos;

    if (count <= 0)
        return -ENOSPC;  // No space

    // Copy from userspace
    if (copy_from_user(device_buffer + buffer_pos, buf, count))
        return -EFAULT;

    buffer_pos += count;
    bytes_written = count;

    return bytes_written;
}

static long my_ioctl(struct file *filp, unsigned int cmd, unsigned long arg)
{
    printk(KERN_INFO "mydevice: ioctl(cmd=%u, arg=%lu)\n", cmd, arg);

    switch (cmd) {
        case 0:  // Example: clear buffer
            buffer_pos = 0;
            memset(device_buffer, 0, BUFFER_SIZE);
            return 0;
        default:
            return -EINVAL;
    }
}

// File operations structure - this is what VFS calls!
static struct file_operations my_fops = {
    .owner = THIS_MODULE,
    .open = my_open,
    .release = my_release,
    .read = my_read,
    .write = my_write,
    .unlocked_ioctl = my_ioctl,
};

// ============================================================================
// MODULE INITIALIZATION AND CLEANUP
// ============================================================================

static int __init my_driver_init(void)
{
    int ret;

    printk(KERN_INFO "mydevice: Initializing driver\n");

    // 1. Allocate device number dynamically
    ret = alloc_chrdev_region(&dev_num, 0, 1, DEVICE_NAME);
    if (ret < 0) {
        printk(KERN_ERR "mydevice: Failed to allocate device number\n");
        return ret;
    }

    printk(KERN_INFO "mydevice: Allocated major=%d, minor=%d\n",
           MAJOR(dev_num), MINOR(dev_num));

    // 2. Allocate and initialize cdev
    my_cdev = cdev_alloc();
    if (!my_cdev) {
        unregister_chrdev_region(dev_num, 1);
        return -ENOMEM;
    }

    my_cdev->ops = &my_fops;
    my_cdev->owner = THIS_MODULE;

    // 3. Add character device to kernel
    ret = cdev_add(my_cdev, dev_num, 1);
    if (ret < 0) {
        printk(KERN_ERR "mydevice: Failed to add cdev\n");
        kfree(my_cdev);
        unregister_chrdev_region(dev_num, 1);
        return ret;
    }

    printk(KERN_INFO "mydevice: Driver initialized successfully\n");
    printk(KERN_INFO "mydevice: Create device file with:\n");
    printk(KERN_INFO "  mknod /dev/mydevice c %d 0\n", MAJOR(dev_num));

    return 0;
}

static void __exit my_driver_exit(void)
{
    printk(KERN_INFO "mydevice: Cleaning up\n");

    // Remove character device
    cdev_del(my_cdev);

    // Free device number
    unregister_chrdev_region(dev_num, 1);

    printk(KERN_INFO "mydevice: Driver unloaded\n");
}

module_init(my_driver_init);
module_exit(my_driver_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Your Name");
MODULE_DESCRIPTION("Simple character device driver");
```

### Building the Driver

```makefile
# Makefile
obj-m += mydevice.o

all:
	make -C /lib/modules/$(shell uname -r)/build M=$(PWD) modules

clean:
	make -C /lib/modules/$(shell uname -r)/build M=$(PWD) clean
```

### Using the Driver

```bash
# Build
make

# Load module
sudo insmod mydevice.ko

# Check kernel messages
dmesg | tail
# mydevice: Allocated major=240, minor=0
# mydevice: Create device file with:
#   mknod /dev/mydevice c 240 0

# Create device file
sudo mknod /dev/mydevice c 240 0
sudo chmod 666 /dev/mydevice

# Test it!
echo "Hello, driver!" > /dev/mydevice
cat /dev/mydevice
# Output: Hello, driver!

# Check kernel logs
dmesg | tail
# mydevice: open() called
# mydevice: write(15 bytes)
# mydevice: close() called
# mydevice: open() called
# mydevice: read(131072 bytes) at pos 0
# mydevice: close() called

# Unload
sudo rmmod mydevice
```

---

## Complete System Call Path

Let's trace what happens when userspace calls `read()` on our device.

### Step-by-Step Flow

```
Application Code:
    int fd = open("/dev/mydevice", O_RDWR);
    char buf[100];
    read(fd, buf, 100);

    ↓

1. USERSPACE → KERNEL (System Call)
   ────────────────────────────────

   glibc read() wrapper
       ↓ syscall instruction (x86: int 0x80 or syscall)

   Kernel entry: arch/x86/entry/entry_64.S
       ↓

   do_syscall_64() in arch/x86/entry/common.c
       ↓

   sys_read() in fs/read_write.c:607

2. VFS LAYER
   ──────────

   SYSCALL_DEFINE3(read, ...) {
       struct fd f = fdget_pos(fd);         // Get file from fd table
       loff_t pos = file_pos_read(f.file);  // Get current position
       ret = vfs_read(f.file, buf, count, &pos);  // ← VFS!
   }

   vfs_read() in fs/read_write.c:434
       ↓
       Checks: is file readable? is count valid?
       ↓
       file->f_op->read(file, buf, count, pos);  // ← Calls driver!

3. DRIVER LAYER
   ────────────

   my_read() in mydevice.c
       ↓
       Check buffer bounds
       ↓
       copy_to_user(buf, device_buffer, count)  // Copy to userspace
       ↓
       Update file position
       ↓
       Return bytes read

4. RETURN PATH
   ────────────

   Driver returns byte count
       ↓
   vfs_read() returns
       ↓
   sys_read() returns
       ↓
   Kernel exit: return to userspace
       ↓
   Application receives data
```

### Detailed Code Trace

```c
// fs/read_write.c:607
SYSCALL_DEFINE3(read, unsigned int, fd, char __user *, buf, size_t, count)
{
    struct fd f = fdget_pos(fd);
    ssize_t ret = -EBADF;

    if (f.file) {
        loff_t pos = file_pos_read(f.file);
        ret = vfs_read(f.file, buf, count, &pos);  // ← HERE
        if (ret >= 0)
            file_pos_write(f.file, pos);
        fdput_pos(f);
    }
    return ret;
}

// fs/read_write.c:434
ssize_t vfs_read(struct file *file, char __user *buf, size_t count, loff_t *pos)
{
    ssize_t ret;

    // Validation
    if (!(file->f_mode & FMODE_READ))
        return -EBADF;
    if (!(file->f_mode & FMODE_CAN_READ))
        return -EINVAL;
    if (unlikely(!access_ok(buf, count)))
        return -EFAULT;

    // Call driver's read function
    if (file->f_op->read)
        ret = file->f_op->read(file, buf, count, pos);  // ← Calls my_read()!
    else if (file->f_op->read_iter)
        ret = new_sync_read(file, buf, count, pos);
    else
        ret = -EINVAL;

    return ret;
}
```

**Key point**: `file->f_op->read` points to our driver's `my_read()` function!

---

## Key Data Structures

### 1. struct inode

Represents a file in the filesystem (on disk).

```c
// include/linux/fs.h
struct inode {
    umode_t             i_mode;      // File type and permissions
    kuid_t              i_uid;       // Owner user ID
    kgid_t              i_gid;       // Owner group ID

    dev_t               i_rdev;      // Device number (for device files!)
    loff_t              i_size;      // File size

    struct timespec     i_atime;     // Access time
    struct timespec     i_mtime;     // Modification time
    struct timespec     i_ctime;     // Change time

    const struct inode_operations   *i_op;   // Inode operations
    const struct file_operations    *i_fop;  // Default file operations

    struct super_block  *i_sb;       // Superblock
    struct address_space *i_mapping; // Page cache

    unsigned long       i_ino;       // Inode number
    union {
        // ...
        struct cdev     *i_cdev;     // Character device (if char device!)
        // ...
    };
};
```

**Location**: Cached in memory, persistent on disk (for filesystems)

### 2. struct file

Represents an open file (in memory only).

```c
// include/linux/fs.h
struct file {
    union {
        struct llist_node   fu_llist;
        struct rcu_head     fu_rcuhead;
    } f_u;

    struct path         f_path;          // Dentry and vfsmount
    struct inode        *f_inode;        // Cached inode pointer

    const struct file_operations *f_op;  // ← Function pointers to driver!

    spinlock_t          f_lock;
    atomic_long_t       f_count;         // Reference count
    unsigned int        f_flags;         // O_RDONLY, O_WRONLY, etc.
    fmode_t             f_mode;          // FMODE_READ, FMODE_WRITE

    loff_t              f_pos;           // Current file position

    struct fown_struct  f_owner;         // Owner for signals

    void                *private_data;   // ← Driver can store data here!

    struct address_space *f_mapping;     // Page cache
};
```

**Multiple processes can open the same file**: Each gets its own `struct file` with separate `f_pos`.

### 3. struct file_operations

Function pointers implemented by driver.

```c
// include/linux/fs.h
struct file_operations {
    struct module *owner;

    // Position operations
    loff_t (*llseek) (struct file *, loff_t, int);

    // Read/Write
    ssize_t (*read) (struct file *, char __user *, size_t, loff_t *);
    ssize_t (*write) (struct file *, const char __user *, size_t, loff_t *);
    ssize_t (*read_iter) (struct kiocb *, struct iov_iter *);
    ssize_t (*write_iter) (struct kiocb *, struct iov_iter *);

    // Async I/O
    ssize_t (*aio_read) (struct kiocb *, const struct iovec *, unsigned long, loff_t);
    ssize_t (*aio_write) (struct kiocb *, const struct iovec *, unsigned long, loff_t);

    // Directory operations
    int (*iterate) (struct file *, struct dir_context *);
    int (*iterate_shared) (struct file *, struct dir_context *);

    // Poll/Select
    __poll_t (*poll) (struct file *, struct poll_table_struct *);

    // ioctl
    long (*unlocked_ioctl) (struct file *, unsigned int, unsigned long);
    long (*compat_ioctl) (struct file *, unsigned int, unsigned long);

    // Memory mapping
    int (*mmap) (struct file *, struct vm_area_struct *);

    // Open/Close
    int (*open) (struct inode *, struct file *);
    int (*flush) (struct file *, fl_owner_t id);
    int (*release) (struct inode *, struct file *);

    // Synchronization
    int (*fsync) (struct file *, loff_t, loff_t, int datasync);

    // File locking
    int (*flock) (struct file *, int, struct file_lock *);

    // Many more...
};
```

### 4. struct cdev

Character device structure.

```c
// include/linux/cdev.h
struct cdev {
    struct kobject kobj;                        // Kernel object
    struct module *owner;                       // Owning module
    const struct file_operations *ops;          // ← File operations!
    struct list_head list;                      // List of cdevs
    dev_t dev;                                  // Device number
    unsigned int count;                         // Number of minors
};
```

---

## Device Registration

### Character Device Registration

```c
// 1. Allocate device number
dev_t dev;
int ret = alloc_chrdev_region(&dev, 0, 1, "mydevice");

// Kernel code: fs/char_dev.c:228
int alloc_chrdev_region(dev_t *dev, unsigned baseminor, unsigned count,
                        const char *name)
{
    struct char_device_struct *cd;
    cd = __register_chrdev_region(0, baseminor, count, name);
    if (IS_ERR(cd))
        return PTR_ERR(cd);
    *dev = MKDEV(cd->major, cd->baseminor);
    return 0;
}

// 2. Initialize cdev
struct cdev *my_cdev = cdev_alloc();
my_cdev->ops = &my_fops;
my_cdev->owner = THIS_MODULE;

// 3. Add to kernel
int ret = cdev_add(my_cdev, dev, 1);

// Kernel code: fs/char_dev.c:471
int cdev_add(struct cdev *p, dev_t dev, unsigned count)
{
    int error;
    p->dev = dev;
    p->count = count;

    // Add to kernel's character device table
    error = kobj_map(cdev_map, dev, count, NULL,
                     exact_match, exact_lock, p);
    if (error)
        return error;

    kobject_get(p->kobj.parent);
    return 0;
}
```

### How open() Finds the Driver

```c
// fs/char_dev.c:365
static struct kobject *exact_match(dev_t dev, int *part, void *data)
{
    struct cdev *p = data;
    return &p->kobj;
}

// fs/char_dev.c:392
static struct kobject *base_probe(dev_t dev, int *part, void *data)
{
    // Look up in character device table
    struct char_device_struct *cd = __lookup_chrdev(MAJOR(dev));
    if (!cd)
        return NULL;
    return get_device(cd->cdev);
}

// When open("/dev/mydevice") is called:
int chrdev_open(struct inode *inode, struct file *filp)
{
    struct cdev *p;
    struct cdev *new = NULL;

    // Look up cdev from inode's device number
    p = inode->i_cdev;
    if (!p) {
        struct kobject *kobj = kobj_lookup(cdev_map, inode->i_rdev, ...);
        new = container_of(kobj, struct cdev, kobj);
        inode->i_cdev = p = new;
    }

    // Replace file operations with driver's operations!
    filp->f_op = fops_get(p->ops);  // ← Sets f_op to our my_fops!

    // Call driver's open function
    if (filp->f_op->open) {
        ret = filp->f_op->open(inode, filp);  // ← Calls my_open()!
    }

    return ret;
}
```

**Key point**: When you `open()` a device file, the kernel:
1. Looks up the inode
2. Gets the device number from `i_rdev`
3. Looks up the `cdev` registered for that device number
4. Replaces `file->f_op` with the driver's `file_operations`
5. Calls the driver's `open()` function

After this, all subsequent operations (`read()`, `write()`, etc.) go directly to the driver!

---

## Device Files in /dev

### How Device Files are Created

**Two ways**:

#### 1. Manual Creation (Old Way)
```bash
# mknod <path> <type> <major> <minor>
sudo mknod /dev/mydevice c 240 0
sudo chmod 666 /dev/mydevice
```

The `mknod` system call:
```c
// fs/namei.c
SYSCALL_DEFINE3(mknod, const char __user *, filename, umode_t, mode, unsigned, dev)
{
    return sys_mknodat(AT_FDCWD, filename, mode, dev);
}

// Creates inode with:
// - i_mode = S_IFCHR | 0666  (character device + permissions)
// - i_rdev = MKDEV(240, 0)   (device number)
```

#### 2. Automatic Creation (Modern Way - udev)

```c
// In driver, use device class:
static struct class *my_class;
static struct device *my_device;

static int __init my_driver_init(void)
{
    // ... allocate device number, create cdev ...

    // Create device class
    my_class = class_create(THIS_MODULE, "mydevice_class");

    // Create device file (udev will create /dev entry!)
    my_device = device_create(my_class, NULL, dev_num, NULL, "mydevice");

    return 0;
}

static void __exit my_driver_exit(void)
{
    device_destroy(my_class, dev_num);
    class_destroy(my_class);
    // ... cleanup cdev ...
}
```

When you call `device_create()`:
1. Kernel creates device in sysfs: `/sys/class/mydevice_class/mydevice`
2. udev daemon receives notification
3. udev creates device file: `/dev/mydevice` with correct permissions
4. No manual `mknod` needed!

### sysfs Integration

```bash
$ ls -l /sys/class/drm/card0/
total 0
-r--r--r-- 1 root root 4096 dev           # Contains "226:0"
drwxr-xr-x 2 root root    0 device
-r--r--r-- 1 root root 4096 uevent

$ cat /sys/class/drm/card0/dev
226:0

$ cat /sys/class/drm/card0/uevent
MAJOR=226
MINOR=0
DEVNAME=dri/card0
```

udev reads these and creates `/dev/dri/card0`.

---

## Block Devices

Block devices are more complex than character devices.

### Key Differences

| Aspect | Character Device | Block Device |
|--------|-----------------|--------------|
| Access | Sequential stream | Random access |
| Unit | Bytes | Fixed-size blocks (512/4096 bytes) |
| Caching | No kernel caching | Page cache |
| Buffering | No | Yes (block layer) |
| Scheduling | No | I/O scheduler |
| Example | `/dev/tty`, `/dev/input/mouse0` | `/dev/sda`, `/dev/nvme0n1` |

### Block Device Structure

```c
// include/linux/blkdev.h
struct gendisk {
    int major;                      // Major number
    int first_minor;                // First minor
    int minors;                     // Number of minors

    char disk_name[DISK_NAME_LEN];  // "sda", "nvme0n1", etc.

    const struct block_device_operations *fops;  // Operations

    struct request_queue *queue;    // Request queue

    void *private_data;             // Driver private data

    // Partitions, etc.
};

struct block_device_operations {
    int (*open) (struct block_device *, fmode_t);
    void (*release) (struct gendisk *, fmode_t);
    int (*ioctl) (struct block_device *, fmode_t, unsigned, unsigned long);
    int (*getgeo)(struct block_device *, struct hd_geometry *);
    // ...
};
```

### Block I/O Path

```
Application
    ↓ read()/write()
VFS
    ↓
Page Cache (check if data cached)
    ↓ Cache miss
File System (ext4, xfs, etc.)
    ↓ Convert to block numbers
Block Layer
    ↓ Create bio (block I/O) structure
I/O Scheduler (merge, sort requests)
    ↓
Block Device Driver
    ↓ Submit to hardware
Disk Controller
    ↓
Physical Disk
```

**Key difference**: Block layer sits between VFS and driver, providing caching and scheduling.

---

## Network Devices

Network devices don't appear in `/dev` and don't use file operations!

### Network Device Structure

```c
// include/linux/netdevice.h
struct net_device {
    char name[IFNAMSIZ];            // "eth0", "wlan0", etc.

    unsigned long state;            // Device state

    const struct net_device_ops *netdev_ops;  // Operations
    const struct ethtool_ops *ethtool_ops;

    unsigned int mtu;               // Maximum transmission unit
    unsigned char dev_addr[MAX_ADDR_LEN];  // MAC address

    struct netdev_queue *_tx;       // Transmit queue

    void *priv;                     // Driver private data
};

struct net_device_ops {
    int (*ndo_open)(struct net_device *dev);
    int (*ndo_stop)(struct net_device *dev);
    netdev_tx_t (*ndo_start_xmit)(struct sk_buff *skb, struct net_device *dev);
    void (*ndo_set_rx_mode)(struct net_device *dev);
    int (*ndo_set_mac_address)(struct net_device *dev, void *addr);
    // ...
};
```

### Why No /dev Entry?

Network devices use sockets, not files:

```c
// Application
int sock = socket(AF_INET, SOCK_STREAM, 0);
bind(sock, &addr, sizeof(addr));
send(sock, data, len, 0);
recv(sock, buf, len, 0);
```

These go through the **network stack**, not VFS!

```
Application: socket(), send(), recv()
    ↓
System calls: sys_socket(), sys_sendto(), sys_recvfrom()
    ↓
Socket Layer
    ↓
Protocol Layer (TCP/IP)
    ↓
Network Device Layer
    ↓
Network Driver (eth0, wlan0)
    ↓
Hardware (NIC)
```

---

## Visual Architecture

### Complete System View

```
┌─────────────────────────────────────────────────────────────┐
│                         USERSPACE                           │
│                                                             │
│  Application: open("/dev/mydevice", O_RDWR);               │
│               read(fd, buf, 100);                           │
└────────────────────────┬────────────────────────────────────┘
                         │ System call
                         ↓
┌─────────────────────────────────────────────────────────────┐
│                     KERNEL SPACE                            │
│                                                             │
│  ┌───────────────────────────────────────────────────┐     │
│  │           System Call Interface                    │     │
│  │   sys_open(), sys_read(), sys_write()             │     │
│  └────────────────────┬──────────────────────────────┘     │
│                       │                                     │
│  ┌────────────────────▼──────────────────────────────┐     │
│  │              VFS Layer                             │     │
│  │                                                    │     │
│  │  - File descriptor table                          │     │
│  │  - Inode cache                                    │     │
│  │  - Dentry cache                                   │     │
│  │  - vfs_read() → file->f_op->read()                │     │
│  └────────────────────┬──────────────────────────────┘     │
│                       │                                     │
│  ┌────────────────────▼──────────────────────────────┐     │
│  │         Character Device Subsystem                 │     │
│  │                                                    │     │
│  │  chrdevs[] array                                  │     │
│  │  Maps major number → cdev                         │     │
│  └────────────────────┬──────────────────────────────┘     │
│                       │                                     │
│  ┌────────────────────▼──────────────────────────────┐     │
│  │            Device Driver (mydevice.ko)             │     │
│  │                                                    │     │
│  │  struct cdev {                                    │     │
│  │      .ops = &my_fops                              │     │
│  │  }                                                │     │
│  │                                                    │     │
│  │  struct file_operations my_fops = {               │     │
│  │      .read = my_read,                             │     │
│  │      .write = my_write,                           │     │
│  │      .open = my_open,                             │     │
│  │  }                                                │     │
│  │                                                    │     │
│  │  my_read() {                                      │     │
│  │      copy_to_user(buf, device_buffer, count);    │     │
│  │  }                                                │     │
│  └────────────────────┬──────────────────────────────┘     │
│                       │                                     │
└───────────────────────┼─────────────────────────────────────┘
                        │ Hardware access
                        ↓
              ┌─────────────────┐
              │  Hardware Device │
              └─────────────────┘
```

### open() System Call Flow

```
Application                VFS                    Character Device      Driver
    │                      │                          │                  │
    │──open("/dev/mydevice")→                         │                  │
    │                      │                          │                  │
    │                      ├─ Lookup path             │                  │
    │                      ├─ Get inode               │                  │
    │                      ├─ See i_rdev = (240, 0)   │                  │
    │                      │                          │                  │
    │                      ├─ Look up major 240 ──────┤                  │
    │                      │                          │                  │
    │                      │                          ├─ Found cdev      │
    │                      │                          │                  │
    │                      │←─ Return cdev ───────────┤                  │
    │                      │                          │                  │
    │                      ├─ file->f_op = cdev->ops  │                  │
    │                      │                          │                  │
    │                      ├─ Call f_op->open() ──────┼──────────────────┤
    │                      │                          │                  │
    │                      │                          │      my_open()   │
    │                      │                          │      executes    │
    │                      │                          │                  │
    │                      │←───── Return 0 ──────────┼──────────────────┤
    │                      │                          │                  │
    │←──── Return fd ──────┤                          │                  │
    │                      │                          │                  │
```

---

## Key Takeaways

### The "Everything is a File" Philosophy

1. **Devices appear as files** in `/dev`
2. **Same API** for devices and files: `open()`, `read()`, `write()`, `close()`
3. **VFS provides abstraction** - applications don't know if they're talking to a disk, device, or network socket

### How It All Connects

1. **Device numbers** identify devices (major = driver, minor = instance)
2. **Device files** in `/dev` have device numbers in their inode
3. **VFS** looks up the device number and finds the driver
4. **Driver registers** `file_operations` structure with function pointers
5. **When you call `read()`**, VFS calls `file->f_op->read()`, which is the driver's function

### Three Device Types

| Type | Access | Example | Special |
|------|--------|---------|---------|
| Character | Stream | `/dev/tty`, `/dev/null` | Simple, direct |
| Block | Random | `/dev/sda` | Caching, scheduling |
| Network | Packet | `eth0`, `wlan0` | No `/dev`, uses sockets |

### Driver Registration Process

1. Allocate device number (`alloc_chrdev_region`)
2. Create `cdev` structure
3. Set `cdev->ops` to your `file_operations`
4. Add to kernel (`cdev_add`)
5. Create device file (manual `mknod` or `device_create` for udev)

### The Magic of VFS

VFS makes all this work transparently:
- Applications use standard file I/O
- Drivers implement `file_operations`
- VFS connects them together
- Everything just works!

---

## Quick Reference

### Key Kernel Source Files

```bash
# VFS
fs/read_write.c              # sys_read(), sys_write(), vfs_read(), vfs_write()
fs/open.c                    # sys_open(), do_sys_open()
include/linux/fs.h           # struct file, struct inode, struct file_operations

# Character devices
fs/char_dev.c                # cdev_add(), alloc_chrdev_region(), chrdev_open()
include/linux/cdev.h         # struct cdev

# Block devices
block/blk-core.c             # Block layer core
include/linux/blkdev.h       # struct gendisk, struct block_device_operations

# Device model
drivers/base/core.c          # device_create(), device_register()
include/linux/device.h       # struct device, struct device_driver
```

### Essential Functions for Driver Development

```c
// Device number allocation
int alloc_chrdev_region(dev_t *dev, unsigned baseminor, unsigned count, const char *name);
void unregister_chrdev_region(dev_t from, unsigned count);

// Character device operations
struct cdev *cdev_alloc(void);
void cdev_init(struct cdev *cdev, const struct file_operations *fops);
int cdev_add(struct cdev *p, dev_t dev, unsigned count);
void cdev_del(struct cdev *p);

// Device class (for udev)
struct class *class_create(struct module *owner, const char *name);
void class_destroy(struct class *cls);

struct device *device_create(struct class *class, struct device *parent,
                             dev_t devt, void *drvdata, const char *fmt, ...);
void device_destroy(struct class *class, dev_t devt);

// Userspace memory access
unsigned long copy_to_user(void __user *to, const void *from, unsigned long n);
unsigned long copy_from_user(void *to, const void __user *from, unsigned long n);
```

### Common ioctl Commands

```c
// Define ioctl commands
#define MYDEV_IOC_MAGIC  'k'
#define MYDEV_IOCRESET   _IO(MYDEV_IOC_MAGIC, 0)
#define MYDEV_IOCGETDATA _IOR(MYDEV_IOC_MAGIC, 1, int)
#define MYDEV_IOCSETDATA _IOW(MYDEV_IOC_MAGIC, 2, int)

// In driver
static long my_ioctl(struct file *filp, unsigned int cmd, unsigned long arg)
{
    switch (cmd) {
        case MYDEV_IOCRESET:
            // Reset device
            break;
        case MYDEV_IOCGETDATA:
            // Get data
            if (copy_to_user((int __user *)arg, &data, sizeof(data)))
                return -EFAULT;
            break;
        case MYDEV_IOCSETDATA:
            // Set data
            if (copy_from_user(&data, (int __user *)arg, sizeof(data)))
                return -EFAULT;
            break;
        default:
            return -EINVAL;
    }
    return 0;
}
```

### Debugging Commands

```bash
# View kernel messages
dmesg | tail

# List loaded modules
lsmod

# View module info
modinfo mydevice.ko

# Load module
sudo insmod mydevice.ko

# Unload module
sudo rmmod mydevice

# View device files
ls -l /dev/

# View sysfs
ls -l /sys/class/
cat /sys/class/mydevice/dev

# View major/minor numbers
cat /proc/devices
```

---

## Summary

Linux device drivers connect hardware to userspace through a layered architecture:

1. **VFS** provides a uniform file-like interface
2. **Device files** in `/dev` represent hardware
3. **Device numbers** (major/minor) identify devices
4. **Drivers** implement `file_operations` to handle I/O
5. **Character devices** are simple stream-based devices
6. **Block devices** provide random access with caching
7. **Network devices** use sockets instead of files

The "everything is a file" philosophy means you can use familiar `open()`, `read()`, `write()`, and `close()` calls to interact with hardware, from disk drives to serial ports to GPUs. VFS makes this possible by abstracting the differences and routing calls to the appropriate driver.

This architecture is why Linux is so flexible: new hardware can be added by writing a driver that implements the standard interface, and existing applications can use it immediately without modification.