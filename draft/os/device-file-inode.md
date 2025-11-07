# Linux Fundamentals: Device, File, and Inode

This document explains the core concepts of devices, files, and inodes in Linux - what they are, how they differ, and how they work together to provide the "everything is a file" abstraction.

## Table of Contents

- [Quick Summary](#quick-summary)
- [What is a File?](#what-is-a-file)
- [What is a Device?](#what-is-a-device)
- [What is a Device File?](#what-is-a-device-file)
- [What is an Inode?](#what-is-an-inode)
- [Regular File Deep Dive](#regular-file-deep-dive)
- [Device File Deep Dive](#device-file-deep-dive)
- [Inode Structure Comparison](#inode-structure-comparison)
- [The VFS Abstraction](#the-vfs-abstraction)
- [Driver Development: The Three-Layer Abstraction](#driver-development-the-three-layer-abstraction)
- [What Happens When You Delete](#what-happens-when-you-delete)
- [Complete Examples](#complete-examples)
- [Visual Diagrams](#visual-diagrams)
- [Key Takeaways](#key-takeaways)
- [Quick Reference](#quick-reference)

---

## Quick Summary

### The Three Concepts

**File (Regular File)**:
- Data stored on a filesystem (disk, RAM, network)
- Has actual content (bytes)
- Lives in storage
- Example: `/home/user/document.txt`

**Device (Hardware)**:
- Physical or virtual hardware
- Connected to system (PCI, USB, etc.)
- The actual hardware itself
- Example: Your hard disk, keyboard, GPU

**Device File (Special File)**:
- File-like interface to a device
- Lives in `/dev` directory
- Contains no data - just device numbers
- Example: `/dev/sda`, `/dev/tty`, `/dev/null`

**Inode**:
- Data structure describing a file or device file
- Contains metadata (permissions, timestamps, etc.)
- Points to data blocks (for files) or device numbers (for device files)
- Lives in the filesystem

---

## What is a File?

A **file** is a container for data stored on a storage medium.

### Characteristics

```
Regular File: /home/user/document.txt
─────────────────────────────────────
- Type: S_IFREG (regular file)
- Size: 1024 bytes (has actual size)
- Data location: Disk blocks on /dev/sda1
- Content: "Hello World\n..." (actual bytes)
- Managed by: Filesystem (ext4, xfs, btrfs, etc.)
```

### Example

```bash
# Create a file
echo "Hello, World!" > /tmp/test.txt

# View it
ls -l /tmp/test.txt
-rw-r--r-- 1 user user 14 Nov 5 10:00 /tmp/test.txt
#^                     ^^
#│                     └─ Size: 14 bytes
#└─ Regular file (-)

# Read it
cat /tmp/test.txt
# Output: Hello, World!

# Check detailed info
stat /tmp/test.txt
#   File: /tmp/test.txt
#   Size: 14            ← Has actual size
# Blocks: 8             ← Uses disk blocks
# Device: 253,0         ← Stored on this device
#  Inode: 12345678      ← Inode number
```

### Where Data Lives

```
File: /tmp/test.txt
        ↓
    Inode #12345678
        ↓
    Points to disk blocks
        ↓
    Block #4521: [H][e][l][l][o][,][ ][W]
    Block #4522: [o][r][l][d][!][\n]
        ↓
    Physical disk sectors
```

---

## What is a Device?

A **device** is physical or virtual hardware connected to the system.

### Types of Devices

**Physical Devices**:
- Hard disk (SATA, NVMe)
- Keyboard, mouse
- Graphics card
- Network card
- USB devices

**Virtual Devices**:
- Loop devices (`/dev/loop0` - mount files as disks)
- Null device (`/dev/null` - data black hole)
- Random device (`/dev/urandom` - random number generator)
- TTY devices (`/dev/tty` - terminal)

### Device Characteristics

```
Device: Samsung SSD 980 Pro
────────────────────────────
- Physical location: PCIe slot, NVMe controller
- Capacity: 1TB
- Connection: PCIe Gen4 x4
- Driver: nvme.ko kernel module
- Identified by: PCI address (0000:01:00.0)
- Device file: /dev/nvme0n1
```

### How Devices are Identified

```bash
# PCI devices
lspci
# 01:00.0 Non-Volatile memory controller: Samsung Electronics Co Ltd NVMe SSD

# USB devices
lsusb
# Bus 001 Device 003: ID 046d:c52b Logitech, Inc. USB Keyboard

# Block devices
lsblk
# NAME   MAJ:MIN SIZE TYPE
# sda      8:0    1T  disk
# nvme0n1  259:0  1T  disk
```

---

## What is a Device File?

A **device file** is a special file that provides an interface to a device. It's the "bridge" between userspace and hardware.

### Characteristics

```
Device File: /dev/sda
─────────────────────
- Type: S_IFBLK (block device)
- Size: N/A (no meaningful size)
- Data location: None! (just contains device numbers)
- Content: major=8, minor=0 (device numbers)
- Managed by: Device driver (sd.ko - SCSI disk driver)
```

### Example

```bash
# List device file
ls -l /dev/sda
brw-rw---- 1 root disk 8, 0 Nov 5 10:00 /dev/sda
#^                   ^^^^
#│                   └─ major=8, minor=0
#└─ Block device (b)

# Check detailed info
stat /dev/sda
#   File: /dev/sda
#   Size: 0              ← No size!
# Blocks: 0              ← No disk blocks!
# Device: 0,6            ← devtmpfs (special filesystem)
#  Inode: 1234
# Device type: 8, 0      ← This is the device number!

# Reading from it reads from the actual hardware
sudo dd if=/dev/sda bs=512 count=1 | xxd
# Reads first 512 bytes from physical disk
```

### Device Numbers

Every device file has two numbers:

**Major Number**: Identifies the driver
```
Major 8  = SCSI disk driver (sd.ko)
Major 1  = Memory devices (/dev/null, /dev/zero, /dev/random)
Major 226 = DRM devices (/dev/dri/card0)
```

**Minor Number**: Identifies the specific device instance
```
/dev/sda  → major=8, minor=0   (first SCSI disk)
/dev/sda1 → major=8, minor=1   (first partition)
/dev/sdb  → major=8, minor=16  (second SCSI disk)
```

### Types of Device Files

```bash
# Character device (c)
crw-rw-rw- 1 root root 1, 3 /dev/null      # Null device
crw------- 1 root root 1, 8 /dev/random    # Random generator
crw-rw----+ 1 root video 226, 0 /dev/dri/card0  # Graphics card

# Block device (b)
brw-rw---- 1 root disk 8, 0 /dev/sda       # Hard disk
brw-rw---- 1 root disk 259, 0 /dev/nvme0n1 # NVMe SSD
```

---

## What is an Inode?

An **inode** (index node) is a data structure that describes a file or device file. It contains metadata about the file.

### Inode Contents

```c
// Simplified view of struct inode
struct inode {
    // Type and permissions
    umode_t     i_mode;      // File type (regular/device/dir) + permissions

    // Ownership
    kuid_t      i_uid;       // Owner user ID
    kgid_t      i_gid;       // Owner group ID

    // Timestamps
    struct timespec i_atime; // Last access time
    struct timespec i_mtime; // Last modification time
    struct timespec i_ctime; // Last status change time

    // Size and blocks (for regular files)
    loff_t      i_size;      // File size in bytes
    blkcnt_t    i_blocks;    // Number of blocks

    // Device number (for device files!)
    dev_t       i_rdev;      // Device number (major/minor)

    // Operations
    const struct file_operations *i_fop;  // Function pointers

    // Data location (for regular files)
    struct address_space *i_mapping;      // Page cache

    // Inode number
    unsigned long i_ino;     // Inode number (unique in filesystem)
};
```

### Key Fields

**For Regular Files**:
- `i_mode` = `S_IFREG | 0644` (regular file + permissions)
- `i_size` = actual file size (e.g., 1024 bytes)
- `i_blocks` = number of disk blocks used
- `i_mapping` = points to page cache and disk blocks
- `i_rdev` = not used

**For Device Files**:
- `i_mode` = `S_IFCHR | 0666` or `S_IFBLK | 0660` (device type + permissions)
- `i_size` = 0 (no meaningful size)
- `i_blocks` = 0 (no disk blocks)
- `i_mapping` = not used
- `i_rdev` = **device number** (major/minor)

---

## Regular File Deep Dive

### Creation

```bash
# Create file
echo "Hello World" > /tmp/myfile.txt
```

**What happens**:
1. Filesystem allocates a new inode
2. Sets `i_mode = S_IFREG | 0644`
3. Allocates disk blocks for data
4. Writes "Hello World\n" to blocks
5. Updates inode to point to blocks
6. Adds directory entry: "myfile.txt" → inode number

### Reading

```c
// Application
int fd = open("/tmp/myfile.txt", O_RDONLY);
char buf[100];
ssize_t n = read(fd, buf, 100);
```

**Kernel path**:
```
sys_open("/tmp/myfile.txt")
    ↓
VFS: Lookup path
    ↓
VFS: Get inode for myfile.txt
    ↓ i_mode = S_IFREG (regular file)
    ↓ i_fop = &ext4_file_operations
    ↓
Call ext4_file_operations.open()
    ↓
Return file descriptor

sys_read(fd, buf, 100)
    ↓
VFS: vfs_read()
    ↓
Call file->f_op->read_iter()
    ↓ (points to ext4_file_read_iter)
    ↓
ext4: generic_file_read_iter()
    ↓
Check page cache
    ↓ (cache miss)
Read from disk blocks
    ↓
DMA transfer: disk → memory
    ↓
Copy to userspace buffer
    ↓
Return bytes read
```

### Data Flow

```
Application buffer
    ↑
    │ copy_to_user()
    │
Page cache (kernel memory)
    ↑
    │ DMA transfer
    │
Disk blocks (physical storage)
    ↑
    │ Read operation
    │
Physical disk sectors
```

---

## Device File Deep Dive

### Creation

**Manual (old way)**:
```bash
# mknod <path> <type> <major> <minor>
sudo mknod /dev/mydevice c 240 0
sudo chmod 666 /dev/mydevice
```

**Automatic (modern way - udev)**:
```c
// In driver
device_create(my_class, NULL, MKDEV(240, 0), NULL, "mydevice");
// udev automatically creates /dev/mydevice
```

**What happens**:
1. Filesystem allocates an inode
2. Sets `i_mode = S_IFCHR | 0666`
3. Sets `i_rdev = MKDEV(240, 0)` (major=240, minor=0)
4. **No disk blocks allocated!**
5. Adds directory entry: "mydevice" → inode number

### Opening

```c
// Application
int fd = open("/dev/mydevice", O_RDWR);
```

**Kernel path**:
```
sys_open("/dev/mydevice")
    ↓
VFS: Lookup path
    ↓
VFS: Get inode for mydevice
    ↓ i_mode = S_IFCHR (character device)
    ↓ i_rdev = MKDEV(240, 0)
    ↓
chrdev_open()
    ↓
Look up character device table for major=240
    ↓
Find cdev registered by driver
    ↓
Replace file->f_op with driver's file_operations
    ↓ file->f_op = &my_driver_fops
    ↓
Call my_driver_fops.open()
    ↓ (driver's my_open() function)
    ↓
Return file descriptor
```

### Reading

```c
// Application
char buf[100];
ssize_t n = read(fd, buf, 100);
```

**Kernel path**:
```
sys_read(fd, buf, 100)
    ↓
VFS: vfs_read()
    ↓
Call file->f_op->read()
    ↓ (points to my_driver_read - driver function!)
    ↓
Driver: my_driver_read()
    ↓
Driver reads from hardware
    ↓ (or generates data, or returns buffered data)
    ↓
copy_to_user(buf, device_data, count)
    ↓
Return bytes read
```

### Data Flow

```
Application buffer
    ↑
    │ copy_to_user()
    │
Driver buffer (kernel memory)
    ↑
    │ DMA transfer or MMIO
    │
Hardware registers/memory
    ↑
    │ Hardware operation
    │
Physical device
```

**No filesystem! No disk blocks! Direct to hardware!**

---

## Inode Structure Comparison

### Regular File Inode

```c
// For /tmp/myfile.txt
struct inode {
    i_ino  = 12345678,
    i_mode = S_IFREG | 0644,         // Regular file, rw-r--r--
    i_uid  = 1000,                   // Owner UID
    i_gid  = 1000,                   // Owner GID

    i_size = 12,                     // 12 bytes
    i_blocks = 8,                    // Uses 8 blocks (512-byte units)

    i_atime = {1699200000, 0},       // Access time
    i_mtime = {1699200000, 0},       // Modification time
    i_ctime = {1699200000, 0},       // Status change time

    // Points to filesystem operations
    i_fop = &ext4_file_operations,

    // Points to data blocks
    i_mapping = {
        .a_ops = &ext4_aops,
        .nrpages = 1,
        // Page cache containing file data
    },

    // NOT USED for regular files
    i_rdev = 0,
};
```

### Device File Inode

```c
// For /dev/sda
struct inode {
    i_ino  = 1234,
    i_mode = S_IFBLK | 0660,         // Block device, rw-rw----
    i_uid  = 0,                      // root
    i_gid  = 6,                      // disk group

    i_size = 0,                      // No size!
    i_blocks = 0,                    // No blocks!

    i_atime = {1699200000, 0},
    i_mtime = {1699200000, 0},
    i_ctime = {1699200000, 0},

    // Points to device operations
    i_fop = &def_blk_fops,           // Default block device ops

    // NO data blocks!
    i_mapping = NULL,                // Not used

    // THIS IS THE KEY!
    i_rdev = MKDEV(8, 0),            // major=8, minor=0

    // Device-specific
    i_bdev = {
        .bd_disk = pointer_to_gendisk,  // Points to SCSI disk driver
    },
};
```

### Side-by-Side Comparison

| Field | Regular File | Device File |
|-------|--------------|-------------|
| `i_mode` | `S_IFREG \| perms` | `S_IFCHR \| perms` or `S_IFBLK \| perms` |
| `i_size` | Actual file size (bytes) | 0 (no meaningful size) |
| `i_blocks` | Number of disk blocks | 0 (no blocks) |
| `i_rdev` | Not used (0) | **Device number (major/minor)** |
| `i_mapping` | Points to page cache & disk | NULL (not used) |
| `i_fop` | Filesystem operations | Device driver operations |
| Purpose | Describes data on disk | Describes hardware interface |

---

## The VFS Abstraction

VFS (Virtual File System) makes files and devices look identical to applications.

### Unified Interface

```c
// Application code - identical for both!
int fd = open(path, O_RDONLY);
char buf[100];
ssize_t n = read(fd, buf, 100);
close(fd);
```

This works for:
- Regular files: `/home/user/document.txt`
- Device files: `/dev/sda`, `/dev/tty`, `/dev/urandom`
- Pipes: `mkfifo mypipe`
- Sockets: Unix domain sockets
- Special filesystems: `/proc/cpuinfo`, `/sys/class/block/sda/size`

### How VFS Routes Operations

```
Application: read(fd, buf, count)
    ↓
System call: sys_read()
    ↓
VFS: vfs_read(file, buf, count)
    ↓
VFS checks file->f_op->read
    ↓
    ├─→ For regular file: ext4_file_read_iter()
    │       ↓
    │   Read from disk via filesystem
    │
    ├─→ For device file: my_device_read()
    │       ↓
    │   Read from hardware via driver
    │
    └─→ For pipe: pipe_read()
            ↓
        Read from pipe buffer
```

**Same interface, different implementation!**

---

## Driver Development: The Three-Layer Abstraction

When writing a device driver, you work with three key abstractions that form a hierarchy:

```
Layer 3: struct inode      ← Filesystem representation (persistent)
Layer 2: struct file       ← Per-process open file (runtime)
Layer 1: struct cdev       ← Device representation (driver-owned)
```

### The Three Structures

#### Layer 1: struct cdev - Device Representation

This is the **actual device object** your driver creates. It's registered with the kernel and contains your driver's operations.

**Location**: `include/linux/cdev.h`

```c
struct cdev {
    struct kobject kobj;                  // Kernel object for sysfs
    struct module *owner;                 // THIS_MODULE
    const struct file_operations *ops;    // YOUR driver functions!
    struct list_head list;
    dev_t dev;                            // Device number (major/minor)
    unsigned int count;                   // Number of minor numbers
};
```

**For block devices**, you use `struct gendisk` instead:
```c
struct gendisk {
    int major;                            // Major number
    int first_minor;                      // First minor number
    int minors;                           // Number of minors
    char disk_name[DISK_NAME_LEN];       // Name (e.g., "sda")
    const struct block_device_operations *fops;  // YOUR block operations!
    struct request_queue *queue;          // I/O request queue
    void *private_data;                   // Your device-specific data
    // ... more fields
};
```

#### Layer 2: struct file - Runtime Open Instance

Created **every time** a process opens your device file. Multiple processes can have multiple `struct file` instances for the same device.

**Location**: `include/linux/fs.h`

```c
struct file {
    struct path f_path;                   // Path to file
    struct inode *f_inode;                // Points to inode!
    const struct file_operations *f_op;   // Points to YOUR ops!

    atomic_long_t f_count;                // Reference count
    unsigned int f_flags;                 // O_RDONLY, O_WRONLY, etc.
    fmode_t f_mode;                       // FMODE_READ, FMODE_WRITE
    loff_t f_pos;                         // File position (for seekable devices)

    void *private_data;                   // YOUR device-specific data!
    // ... more fields
};
```

**Key field**: `private_data` - This is where you store your device-specific structure pointer!

#### Layer 3: struct inode - Filesystem Representation

The **persistent** representation of the device file in `/dev`. Created when device file is created (via `mknod` or `device_create()`).

**Location**: `include/linux/fs.h`

```c
struct inode {
    umode_t i_mode;                       // File type + permissions
    kuid_t i_uid;                         // Owner
    kgid_t i_gid;                         // Group

    dev_t i_rdev;                         // Device number (major/minor)!
    loff_t i_size;                        // Size (0 for devices)

    const struct file_operations *i_fop;  // Initial fops (replaced on open)

    union {
        struct block_device *i_bdev;      // For block devices
        struct cdev *i_cdev;              // For character devices!
    };
    // ... more fields
};
```

**Key fields**:
- `i_rdev`: Contains the device number (major/minor)
- `i_cdev`: Points to your `struct cdev`!

---

### How They Connect

```
┌─────────────────────────────────────────────────────────┐
│  /dev/mydevice (Device File)                            │
│                                                         │
│  Created by: mknod or device_create()                  │
└────────────────────┬────────────────────────────────────┘
                     │
                     │ Creates
                     ↓
┌─────────────────────────────────────────────────────────┐
│  struct inode (Filesystem Layer)                        │
│                                                         │
│  i_mode  = S_IFCHR | 0666                              │
│  i_rdev  = MKDEV(240, 0)  ← Device number              │
│  i_cdev  = &my_cdev       ← Points to driver's cdev!   │
│  i_fop   = &def_chr_fops  ← Default char device ops    │
└────────────────────┬────────────────────────────────────┘
                     │
                     │ Links to
                     ↓
┌─────────────────────────────────────────────────────────┐
│  struct cdev (Driver Layer)                             │
│                                                         │
│  dev   = MKDEV(240, 0)                                 │
│  ops   = &my_fops         ← YOUR file_operations!      │
│  owner = THIS_MODULE                                   │
└─────────────────────────────────────────────────────────┘
         │
         │ Contains
         ↓
┌─────────────────────────────────────────────────────────┐
│  struct file_operations (YOUR driver functions)         │
│                                                         │
│  .open    = my_open                                    │
│  .release = my_release                                 │
│  .read    = my_read                                    │
│  .write   = my_write                                   │
│  .ioctl   = my_ioctl                                   │
└─────────────────────────────────────────────────────────┘


When process opens /dev/mydevice:

┌─────────────────────────────────────────────────────────┐
│  struct file (Per-Process Layer)                        │
│                                                         │
│  f_inode       = &inode       ← Points to inode        │
│  f_op          = &my_fops     ← Copied from cdev.ops!  │
│  f_flags       = O_RDWR                                │
│  f_pos         = 0                                     │
│  private_data  = my_device    ← YOUR device structure  │
└─────────────────────────────────────────────────────────┘
```

---

### The Connection Process

#### Step 1: Driver Initialization

```c
// Your device structure
struct my_device {
    struct cdev cdev;        // Embedded cdev
    char buffer[1024];       // Device-specific data
    int count;
    // ... more fields
};

static struct my_device *my_dev;
static dev_t dev_num;       // Device number
static struct class *my_class;

static int __init my_driver_init(void)
{
    // 1. Allocate device number
    alloc_chrdev_region(&dev_num, 0, 1, "mydevice");
    //                  ^^^^^^^^  ^  ^  ^^^^^^^^^^
    //                  output   minor count  name
    //                           start

    // 2. Create your device structure
    my_dev = kzalloc(sizeof(struct my_device), GFP_KERNEL);

    // 3. Initialize cdev
    cdev_init(&my_dev->cdev, &my_fops);
    //                       ^^^^^^^^
    //                       YOUR file_operations
    my_dev->cdev.owner = THIS_MODULE;

    // 4. Add cdev to kernel
    cdev_add(&my_dev->cdev, dev_num, 1);
    //                      ^^^^^^^^  ^
    //                      device    count
    //                      number

    // NOW: inode.i_cdev will point to &my_dev->cdev

    // 5. Create /dev/mydevice file (automatic via udev)
    my_class = class_create(THIS_MODULE, "myclass");
    device_create(my_class, NULL, dev_num, NULL, "mydevice");
    //                                            ^^^^^^^^^^
    //                                            Creates /dev/mydevice

    printk(KERN_INFO "mydevice: registered with major=%d\n", MAJOR(dev_num));
    return 0;
}
```

**What gets created**:
```
Kernel:
    cdev_map[240] → my_dev->cdev
                        │
                        └─> .ops = &my_fops
                            .dev = MKDEV(240, 0)

Filesystem:
    /dev/mydevice → inode
                      │
                      ├─> i_rdev = MKDEV(240, 0)
                      ├─> i_cdev = &my_dev->cdev  ← Connection!
                      └─> i_fop  = &def_chr_fops
```

#### Step 2: Opening the Device

```c
// Userspace
int fd = open("/dev/mydevice", O_RDWR);
```

**Kernel path** (`fs/char_dev.c:chrdev_open()`):

```c
// fs/char_dev.c
static int chrdev_open(struct inode *inode, struct file *filp)
{
    struct cdev *cdev;
    struct cdev *new = NULL;

    // 1. Get device number from inode
    dev_t dev = inode->i_rdev;  // MKDEV(240, 0)

    // 2. Look up cdev from inode
    cdev = inode->i_cdev;
    if (!cdev) {
        // First time: look up in cdev_map
        cdev = cdev_get(kobj_lookup(cdev_map, dev, ...));
        // Store it in inode for next time
        inode->i_cdev = cdev;
    }

    // 3. Replace file operations with driver's ops
    filp->f_op = cdev->ops;  // filp->f_op = &my_fops

    // 4. Call driver's open function
    if (filp->f_op->open) {
        ret = filp->f_op->open(inode, filp);  // my_open(inode, filp)
    }

    return ret;
}
```

**Your driver's open function**:

```c
static int my_open(struct inode *inode, struct file *filp)
{
    struct my_device *dev;

    // Get your device structure from inode
    dev = container_of(inode->i_cdev, struct my_device, cdev);
    //                 ^^^^^^^^^^^^   ^^^^^^^^^^^^^^^^  ^^^^
    //                 inode has      your device       cdev field
    //                 i_cdev         structure type    in struct

    // Store it in file's private_data
    filp->private_data = dev;

    printk(KERN_INFO "mydevice: opened\n");
    return 0;
}
```

**Now the connection is complete**:
```
struct file {
    .f_inode       = &inode          ← Points to inode
    .f_op          = &my_fops        ← YOUR operations!
    .private_data  = my_dev          ← YOUR device structure!
}

struct inode {
    .i_rdev = MKDEV(240, 0)          ← Device number
    .i_cdev = &my_dev->cdev          ← YOUR cdev
}

struct cdev (my_dev->cdev) {
    .dev   = MKDEV(240, 0)           ← Same device number
    .ops   = &my_fops                ← YOUR operations
}
```

#### Step 3: Reading from the Device

```c
// Userspace
char buf[100];
read(fd, buf, 100);
```

**Kernel path**:

```c
// fs/read_write.c
SYSCALL_DEFINE3(read, int, fd, char __user *, buf, size_t, count)
{
    struct file *filp = fget(fd);   // Get struct file from fd

    return vfs_read(filp, buf, count, &filp->f_pos);
}

ssize_t vfs_read(struct file *filp, char __user *buf, size_t count, loff_t *pos)
{
    // Call driver's read function
    return filp->f_op->read(filp, buf, count, pos);  // my_read()
}
```

**Your driver's read function**:

```c
static ssize_t my_read(struct file *filp, char __user *buf,
                       size_t count, loff_t *f_pos)
{
    struct my_device *dev = filp->private_data;  // Get your device!

    // Read from your device
    if (copy_to_user(buf, dev->buffer + *f_pos, count))
        return -EFAULT;

    *f_pos += count;
    return count;
}
```

---

### Complete Example: Character Device Driver

```c
#include <linux/module.h>
#include <linux/fs.h>
#include <linux/cdev.h>
#include <linux/device.h>
#include <linux/uaccess.h>

// Step 1: Define your device structure
struct my_device {
    struct cdev cdev;           // Character device structure
    char buffer[1024];          // Device data
    size_t size;                // Data size
};

static struct my_device *my_dev;
static dev_t dev_num;           // Device number (major/minor)
static struct class *my_class;

// Step 2: Define file operations
static int my_open(struct inode *inode, struct file *filp)
{
    // Extract device structure from inode
    struct my_device *dev = container_of(inode->i_cdev, struct my_device, cdev);

    // Store in file's private_data for easy access
    filp->private_data = dev;

    printk(KERN_INFO "mydevice: Device opened\n");
    return 0;
}

static int my_release(struct inode *inode, struct file *filp)
{
    printk(KERN_INFO "mydevice: Device closed\n");
    return 0;
}

static ssize_t my_read(struct file *filp, char __user *buf,
                       size_t count, loff_t *f_pos)
{
    struct my_device *dev = filp->private_data;
    size_t to_read;

    // Check bounds
    if (*f_pos >= dev->size)
        return 0;  // EOF

    to_read = min(count, dev->size - (size_t)*f_pos);

    // Copy to userspace
    if (copy_to_user(buf, dev->buffer + *f_pos, to_read))
        return -EFAULT;

    *f_pos += to_read;
    return to_read;
}

static ssize_t my_write(struct file *filp, const char __user *buf,
                        size_t count, loff_t *f_pos)
{
    struct my_device *dev = filp->private_data;
    size_t to_write;

    // Check bounds
    if (*f_pos >= sizeof(dev->buffer))
        return -ENOSPC;

    to_write = min(count, sizeof(dev->buffer) - (size_t)*f_pos);

    // Copy from userspace
    if (copy_from_user(dev->buffer + *f_pos, buf, to_write))
        return -EFAULT;

    *f_pos += to_write;
    if (*f_pos > dev->size)
        dev->size = *f_pos;

    return to_write;
}

static struct file_operations my_fops = {
    .owner   = THIS_MODULE,
    .open    = my_open,
    .release = my_release,
    .read    = my_read,
    .write   = my_write,
};

// Step 3: Module initialization
static int __init my_driver_init(void)
{
    int ret;

    // Allocate device number
    ret = alloc_chrdev_region(&dev_num, 0, 1, "mydevice");
    if (ret < 0) {
        printk(KERN_ERR "mydevice: Failed to allocate device number\n");
        return ret;
    }

    printk(KERN_INFO "mydevice: Allocated major=%d, minor=%d\n",
           MAJOR(dev_num), MINOR(dev_num));

    // Allocate device structure
    my_dev = kzalloc(sizeof(struct my_device), GFP_KERNEL);
    if (!my_dev) {
        unregister_chrdev_region(dev_num, 1);
        return -ENOMEM;
    }

    // Initialize cdev
    cdev_init(&my_dev->cdev, &my_fops);
    my_dev->cdev.owner = THIS_MODULE;

    // Add cdev to kernel
    ret = cdev_add(&my_dev->cdev, dev_num, 1);
    if (ret < 0) {
        kfree(my_dev);
        unregister_chrdev_region(dev_num, 1);
        return ret;
    }

    // Create device class
    my_class = class_create(THIS_MODULE, "myclass");
    if (IS_ERR(my_class)) {
        cdev_del(&my_dev->cdev);
        kfree(my_dev);
        unregister_chrdev_region(dev_num, 1);
        return PTR_ERR(my_class);
    }

    // Create device file (/dev/mydevice)
    if (IS_ERR(device_create(my_class, NULL, dev_num, NULL, "mydevice"))) {
        class_destroy(my_class);
        cdev_del(&my_dev->cdev);
        kfree(my_dev);
        unregister_chrdev_region(dev_num, 1);
        return -1;
    }

    printk(KERN_INFO "mydevice: Device created at /dev/mydevice\n");
    return 0;
}

// Step 4: Module cleanup
static void __exit my_driver_exit(void)
{
    device_destroy(my_class, dev_num);
    class_destroy(my_class);
    cdev_del(&my_dev->cdev);
    kfree(my_dev);
    unregister_chrdev_region(dev_num, 1);

    printk(KERN_INFO "mydevice: Driver unloaded\n");
}

module_init(my_driver_init);
module_exit(my_driver_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Your Name");
MODULE_DESCRIPTION("Simple character device driver");
```

---

### Visual Summary: The Three Layers in Action

```
┌────────────────────────────────────────────────────────────────┐
│  Userspace: open("/dev/mydevice", O_RDWR)                      │
└───────────────────────────┬────────────────────────────────────┘
                            │ syscall
                            ↓
┌────────────────────────────────────────────────────────────────┐
│  VFS Layer                                                     │
│                                                                │
│  1. Lookup /dev/mydevice                                      │
│  2. Get inode                                                 │
│                                                                │
│     struct inode {                                            │
│         i_mode = S_IFCHR | 0666                               │
│         i_rdev = MKDEV(240, 0)    ← Device number             │
│         i_cdev = &my_dev->cdev    ← Driver's cdev             │
│     }                                                          │
│                                                                │
│  3. Call chrdev_open()                                        │
└───────────────────────────┬────────────────────────────────────┘
                            │
                            ↓
┌────────────────────────────────────────────────────────────────┐
│  chrdev_open() (fs/char_dev.c)                                │
│                                                                │
│  1. cdev = inode->i_cdev          ← Get driver's cdev         │
│  2. filp->f_op = cdev->ops        ← Use driver's operations   │
│  3. Call filp->f_op->open()       ← Call driver's my_open()   │
└───────────────────────────┬────────────────────────────────────┘
                            │
                            ↓
┌────────────────────────────────────────────────────────────────┐
│  Driver Layer: my_open()                                       │
│                                                                │
│  struct my_device *dev = container_of(inode->i_cdev, ...);   │
│  filp->private_data = dev;                                    │
│                                                                │
│  Now we have:                                                 │
│                                                                │
│  struct file {            struct inode {                      │
│      f_inode = &inode        i_cdev = &my_dev->cdev          │
│      f_op = &my_fops         i_rdev = MKDEV(240,0)           │
│      private_data = my_dev   ...                              │
│  }                        }                                   │
│                                                                │
│  struct cdev (my_dev->cdev) {                                │
│      ops = &my_fops                                           │
│      dev = MKDEV(240,0)                                       │
│  }                                                             │
│                                                                │
│  ALL THREE LAYERS CONNECTED!                                  │
└────────────────────────────────────────────────────────────────┘
```

When `read()` is called:
```
read(fd, buf, 100)
    ↓
vfs_read(filp, buf, 100)
    ↓
filp->f_op->read()    ← Points to my_read()
    ↓
my_read(filp, buf, 100)
    ↓
dev = filp->private_data    ← Get device structure
    ↓
copy_to_user(buf, dev->buffer, 100)    ← Access device data
```

---

### Key Insights

1. **Three distinct objects**:
   - `struct cdev`: Driver-owned, lives in kernel memory, one per device
   - `struct inode`: Filesystem-owned, lives in dcache, one per device file
   - `struct file`: Per-process, one per open() call, many can exist

2. **Connection points**:
   - `inode.i_rdev` → device number → used to find cdev
   - `inode.i_cdev` → pointer to driver's cdev (cached after first open)
   - `file.f_op` → pointer to driver's operations (copied from cdev)
   - `file.private_data` → pointer to driver's device structure

3. **Data flow**:
   ```
   Device number → inode.i_rdev
   cdev → inode.i_cdev
   Operations → file.f_op
   Device data → file.private_data
   ```

4. **Why this design?**:
   - **Separation of concerns**: Filesystem doesn't know about driver internals
   - **Multiple opens**: Many processes can open same device (different `struct file`, same `struct cdev`)
   - **Hot-plugging**: Can create/destroy cdev without affecting inode (until next open)
   - **Flexibility**: Driver can store per-open state in `file.private_data`

---

## What Happens When You Delete

### Deleting a Regular File

```bash
$ rm /tmp/myfile.txt
```

**What happens**:
1. VFS looks up inode for `myfile.txt`
2. Removes directory entry
3. Decrements inode link count
4. If link count == 0:
   - Marks inode as free
   - **Frees disk blocks**
   - **Data is gone!**

**Result**: File and its data are deleted

### Deleting a Device File

```bash
$ sudo rm /dev/sda
```

**What happens**:
1. VFS looks up inode for `/dev/sda`
2. Removes directory entry from `/dev`
3. Decrements inode link count
4. If link count == 0:
   - Marks inode as free
   - **No disk blocks to free!**
   - **Physical disk still exists!**
   - **Driver still loaded!**

**Result**: Only the device file entry is removed

**You can recreate it**:
```bash
$ sudo mknod /dev/sda b 8 0
# Device file recreated, same disk accessible again!
```

**Key difference**: Device files are just **pointers** to hardware. Deleting them doesn't affect the hardware!

---

## Complete Examples

### Example 1: Regular File

```bash
# Create file
$ echo "Hello World" > /tmp/test.txt

# View file
$ ls -l /tmp/test.txt
-rw-r--r-- 1 user user 12 Nov 5 10:00 /tmp/test.txt

# Detailed info
$ stat /tmp/test.txt
  File: /tmp/test.txt
  Size: 12              ← Actual size
Blocks: 8               ← Uses disk blocks
Device: 253,0           ← Stored on device 253,0
 Inode: 12345678        ← Inode number
 Links: 1
Access: (0644/-rw-r--r--)
```

**Inode contains**:
- Type: Regular file
- Size: 12 bytes
- Points to disk blocks containing "Hello World\n"

**Reading it**:
```bash
$ cat /tmp/test.txt
Hello World
```

**Kernel flow**:
1. Opens file → gets inode
2. Sees `S_IFREG` → it's a regular file
3. Calls filesystem's read function
4. Reads from disk blocks
5. Returns data

### Example 2: Character Device

```bash
# Device exists
$ ls -l /dev/urandom
crw-rw-rw- 1 root root 1, 9 Nov 5 10:00 /dev/urandom

# Detailed info
$ stat /dev/urandom
  File: /dev/urandom
  Size: 0               ← No size!
Blocks: 0               ← No blocks!
Device: 0,6             ← devtmpfs
 Inode: 1234
Device type: 1,9        ← major=1, minor=9
 Links: 1
Access: (0666/crw-rw-rw-)
```

**Inode contains**:
- Type: Character device
- Device number: major=1, minor=9
- No data blocks!

**Reading it**:
```bash
$ dd if=/dev/urandom bs=10 count=1 status=none | xxd
00000000: a3f2 b8c4 d5e6 f7a8 b9c0
```

**Kernel flow**:
1. Opens file → gets inode
2. Sees `S_IFCHR` → it's a character device
3. Looks up driver for major=1 (mem driver)
4. Calls driver's read function: `urandom_read()`
5. Driver generates random bytes
6. Returns data

### Example 3: Block Device

```bash
# Block device
$ ls -l /dev/sda
brw-rw---- 1 root disk 8, 0 Nov 5 10:00 /dev/sda

# Detailed info
$ stat /dev/sda
  File: /dev/sda
  Size: 0               ← No size (device has capacity, not file size)
Blocks: 0               ← No blocks
Device: 0,6
 Inode: 5678
Device type: 8,0        ← major=8, minor=0 (SCSI disk)
 Links: 1
Access: (0660/brw-rw----)
```

**Reading first sector**:
```bash
$ sudo dd if=/dev/sda bs=512 count=1 status=none | xxd | head -n 5
00000000: eb63 9010 8ed0 bc00 b0b8 0000 8ed8 8ec0  .c..............
00000010: fbbe 007c bf00 06b9 0002 f3a4 ea21 0600  ...|.........!..
00000020: 00be be07 3804 750b 83c6 1081 fefe 0775  ....8.u........u
00000030: f3eb 16b4 02b0 01bb 007c b280 8a74 018b  .........|...t..
00000040: 4c02 cd13 ea00 7c00 00eb fe00 0000 0000  L.....|.........
```

This reads **actual data from physical disk**!

### Example 4: Virtual Device (/dev/null)

```bash
# The famous null device
$ ls -l /dev/null
crw-rw-rw- 1 root root 1, 3 Nov 5 10:00 /dev/null

# Write to it - data disappears!
$ echo "Hello" > /dev/null

# Read from it - always returns EOF
$ cat /dev/null
# (no output)

# It's a device file with no physical hardware!
```

**Driver implementation** (`drivers/char/mem.c`):
```c
static ssize_t read_null(struct file *file, char __user *buf,
                         size_t count, loff_t *ppos)
{
    return 0;  // Always EOF!
}

static ssize_t write_null(struct file *file, const char __user *buf,
                          size_t count, loff_t *ppos)
{
    return count;  // Pretend we wrote it, discard data!
}
```

---

## Visual Diagrams

### Regular File Architecture

```
┌─────────────────────────────────────────────────┐
│        Application: cat /tmp/test.txt           │
└────────────────────┬────────────────────────────┘
                     │ open(), read()
                     ↓
┌─────────────────────────────────────────────────┐
│              VFS Layer                          │
│                                                 │
│  - Lookup: /tmp/test.txt                       │
│  - Get inode #12345678                         │
│  - Check: i_mode = S_IFREG (regular file)      │
│  - Call: file->f_op->read_iter()               │
└────────────────────┬────────────────────────────┘
                     │
                     ↓
┌─────────────────────────────────────────────────┐
│         Filesystem Layer (ext4)                 │
│                                                 │
│  - ext4_file_read_iter()                       │
│  - Check page cache                            │
│  - Read from disk if not cached                │
└────────────────────┬────────────────────────────┘
                     │
                     ↓
┌─────────────────────────────────────────────────┐
│            Block Layer                          │
│                                                 │
│  - Translate to block numbers                  │
│  - Submit I/O request                          │
└────────────────────┬────────────────────────────┘
                     │
                     ↓
┌─────────────────────────────────────────────────┐
│          Block Device Driver                    │
│                                                 │
│  - DMA transfer: disk → memory                 │
└────────────────────┬────────────────────────────┘
                     │
                     ↓
┌─────────────────────────────────────────────────┐
│          Physical Disk                          │
│                                                 │
│  Block #4521: [H][e][l][l][o][ ][W][o]        │
│  Block #4522: [r][l][d][\n]                    │
└─────────────────────────────────────────────────┘
```

### Device File Architecture

```
┌─────────────────────────────────────────────────┐
│     Application: cat /dev/urandom               │
└────────────────────┬────────────────────────────┘
                     │ open(), read()
                     ↓
┌─────────────────────────────────────────────────┐
│              VFS Layer                          │
│                                                 │
│  - Lookup: /dev/urandom                        │
│  - Get inode #1234                             │
│  - Check: i_mode = S_IFCHR (char device)       │
│  - Check: i_rdev = MKDEV(1, 9)                 │
│  - Lookup driver for major=1                   │
│  - Replace file->f_op with driver ops          │
│  - Call: file->f_op->read()                    │
└────────────────────┬────────────────────────────┘
                     │
                     ↓
┌─────────────────────────────────────────────────┐
│       Character Device Driver (mem.c)           │
│                                                 │
│  - urandom_read()                              │
│  - Generate random bytes                       │
│  - copy_to_user()                              │
└────────────────────┬────────────────────────────┘
                     │
                     ↓
┌─────────────────────────────────────────────────┐
│           Virtual Device                        │
│         (No physical hardware!)                 │
│                                                 │
│  - Uses kernel random number pool              │
│  - Software-generated randomness                │
└─────────────────────────────────────────────────┘
```

### Inode Comparison Diagram

```
Regular File Inode                  Device File Inode
(/tmp/test.txt)                     (/dev/sda)
─────────────────                   ─────────────────

┌──────────────────┐               ┌──────────────────┐
│ i_ino: 12345678  │               │ i_ino: 1234      │
│ i_mode: S_IFREG  │               │ i_mode: S_IFBLK  │
│        | 0644    │               │        | 0660    │
│                  │               │                  │
│ i_size: 12       │               │ i_size: 0        │
│ i_blocks: 8      │               │ i_blocks: 0      │
│                  │               │                  │
│ i_rdev: (unused) │               │ i_rdev: MKDEV(8,0)│ ← KEY!
│                  │               │         ^^^^^^^^^ │
│ i_fop:           │               │         major=8   │
│   ext4_file_ops  │               │         minor=0   │
│                  │               │                  │
│ i_mapping:       │               │ i_fop:           │
│   → Page cache   │               │   def_blk_fops   │
│   → Disk blocks  │               │                  │
│     [4521][4522] │               │ i_bdev:          │
│        ↓    ↓    │               │   → gendisk      │
│      Actual data │               │   → SCSI driver  │
└──────────────────┘               └──────────────────┘
```

---

## Key Takeaways

### Core Concepts

1. **Files** are data containers stored on filesystems
2. **Devices** are physical or virtual hardware
3. **Device files** are interfaces to devices (not actual data!)
4. **Inodes** describe both files and device files

### Critical Differences

| Concept | Contains Data? | Has Size? | Has Disk Blocks? |
|---------|---------------|-----------|------------------|
| Regular File | ✅ Yes | ✅ Yes | ✅ Yes |
| Device File | ❌ No | ❌ No | ❌ No |
| Device (Hardware) | ✅ Yes (on device) | ✅ Yes (capacity) | N/A |

### The Inode Distinction

**Regular file inode**:
- Points to **data blocks** (actual content)
- `i_size` = file size
- `i_mapping` = page cache

**Device file inode**:
- Contains **device number** (major/minor)
- `i_rdev` = identifies driver and device
- No data blocks!

### The Magic of VFS

VFS makes files and devices look identical:
- Same API: `open()`, `read()`, `write()`, `close()`
- Different implementations underneath
- Applications don't need to know the difference!

### What You Can Delete

**Regular file**: `rm file.txt`
- ✅ Deletes file entry
- ✅ Frees disk blocks
- ✅ Data is gone

**Device file**: `rm /dev/sda`
- ✅ Deletes device file entry
- ❌ Device hardware still exists
- ❌ Driver still loaded
- ✅ Can recreate with `mknod`

---

## Quick Reference

### File Types in i_mode

```c
#define S_IFREG  0100000   // Regular file
#define S_IFDIR  0040000   // Directory
#define S_IFCHR  0020000   // Character device
#define S_IFBLK  0060000   // Block device
#define S_IFIFO  0010000   // Named pipe (FIFO)
#define S_IFLNK  0120000   // Symbolic link
#define S_IFSOCK 0140000   // Socket

// Check file type
#define S_ISREG(m)  (((m) & S_IFMT) == S_IFREG)
#define S_ISDIR(m)  (((m) & S_IFMT) == S_IFDIR)
#define S_ISCHR(m)  (((m) & S_IFMT) == S_IFCHR)
#define S_ISBLK(m)  (((m) & S_IFMT) == S_IFBLK)
```

### Device Number Macros

```c
// include/linux/kdev_t.h
#define MAJOR(dev)  ((unsigned int) ((dev) >> 20))
#define MINOR(dev)  ((unsigned int) ((dev) & 0xfffff))
#define MKDEV(ma,mi) (((ma) << 20) | (mi))
```

### Checking File Type in Shell

```bash
# List with file type indicator
ls -l /dev/sda
brw-rw---- 1 root disk 8, 0 Nov 5 10:00 /dev/sda
#^
#└─ b = block device, c = char device, - = regular file

# Detailed info
stat /dev/sda
stat /tmp/file.txt

# Check major/minor numbers
ls -l /dev/ | awk '{print $5, $6, $10}'
```

### Common Device Major Numbers

```
Major 1   = Memory devices (/dev/null, /dev/zero, /dev/random)
Major 4   = TTY devices (/dev/tty*)
Major 8   = SCSI disk devices (/dev/sd*)
Major 13  = Input devices (/dev/input/*)
Major 226 = DRM devices (/dev/dri/card*)
Major 259 = NVMe devices (/dev/nvme*)
```

### Kernel Source Files

```bash
# Inode definition
include/linux/fs.h                # struct inode

# VFS operations
fs/read_write.c                   # vfs_read(), vfs_write()
fs/open.c                         # vfs_open()

# Character device
fs/char_dev.c                     # chrdev_open()

# Block device
fs/block_dev.c                    # blkdev_open()

# Device files
fs/namei.c                        # mknod system call
```

---

## Summary

In Linux:

1. **Files** store data in filesystems
   - Have size, use disk blocks
   - Inode points to data blocks

2. **Devices** are hardware (or virtual hardware)
   - Identified by bus addresses
   - Managed by device drivers

3. **Device files** bridge userspace to devices
   - Live in `/dev`
   - Contain device numbers, not data
   - Inode stores major/minor numbers

4. **Inodes** describe both files and device files
   - For files: point to data blocks
   - For devices: store device numbers

5. **VFS** provides unified interface
   - Same API for files and devices
   - Routes operations appropriately
   - "Everything is a file" abstraction

The beauty of this design is that applications don't need to know whether they're reading from a file on disk or from a hardware device - the API is identical, and VFS handles the complexity underneath!
