# Linux Kernel Video Playback: From Compressed Bits to Pixels on Screen

This document explains how video playback works in Linux at the kernel level, tracing the complete path from a compressed video file to pixels displayed on your screen. We'll explore hardware video decoding, zero-copy buffer sharing, and the modern DRM display pipeline.

## Table of Contents
- [Why Can't We Just Decode Video in Userspace?](#why-cant-we-just-decode-video-in-userspace)
- [High-Level Overview](#high-level-overview)
- [The Complete Pipeline](#the-complete-pipeline)
- [Video Decoding with V4L2](#video-decoding-with-v4l2)
  - [V4L2 Device Model](#v4l2-device-model)
  - [Memory-to-Memory Codec Framework](#memory-to-memory-codec-framework)
  - [Buffer Management with VideoBuf2](#buffer-management-with-videobuf2)
- [Zero-Copy Buffer Sharing with DMA-BUF](#zero-copy-buffer-sharing-with-dma-buf)
  - [DMA-BUF Architecture](#dma-buf-architecture)
  - [Synchronization with DMA Fences](#synchronization-with-dma-fences)
  - [Exporting Decoded Frames](#exporting-decoded-frames)
- [Display Pipeline with DRM](#display-pipeline-with-drm)
  - [DRM Display Objects](#drm-display-objects)
  - [Atomic Display Updates](#atomic-display-updates)
  - [Importing DMA-BUF to GPU](#importing-dma-buf-to-gpu)
- [Complete Flow: Syscalls to Hardware](#complete-flow-syscalls-to-hardware)
- [Hardware Decoder Deep Dive](#hardware-decoder-deep-dive)
- [Synchronization and Timing](#synchronization-and-timing)
- [Key Takeaways](#key-takeaways)
- [Quick Reference](#quick-reference)

## Why Can't We Just Decode Video in Userspace?

**Short answer**: You can, but it's slow, power-hungry, and defeats the purpose of having dedicated hardware.

### The Naive Approach

You might think: "Just read the H.264 file, decompress it, and write pixels to the screen." Let's see what that involves:

```c
// Naive video playback (pseudocode)
while (1) {
    // 1. Read compressed frame from file
    read(video_fd, compressed_frame, size);

    // 2. Decode H.264/VP9/AV1 (pure software)
    decode_h264(compressed_frame, &raw_pixels);  // ← THE PROBLEM

    // 3. Write pixels to screen
    memcpy(framebuffer, raw_pixels, 1920*1080*4);
}
```

### The Problem: Video Codecs Are Incredibly Complex

Modern video codecs like **H.264**, **VP9**, and **AV1** are extraordinarily sophisticated compression algorithms. A single 1080p H.264 frame requires:

**Computational Cost**:
```
1080p frame @ 60 FPS = 124,416,000 pixels/second

Per-pixel operations:
- DCT/IDCT transforms (8×8 or 16×16 blocks)
- Motion compensation (searching previous frames)
- Entropy decoding (CABAC/CAVLC)
- Deblocking filter
- In-loop filtering

Total: ~50-100 operations per pixel
= 6-12 billion operations per second
```

**Software Decoding Reality**:
- **4K60 H.264**: Consumes 4-6 CPU cores at 100%
- **4K60 VP9**: Consumes 6-8 CPU cores at 100%
- **4K60 AV1**: Consumes 10-16 CPU cores at 100% (basically impossible on current CPUs)

### The Solution: Hardware Video Decoders

Modern CPUs and SoCs contain **dedicated silicon** for video decoding:

```
┌──────────────────────────────────────────────────────┐
│                   CPU DIE                            │
│                                                      │
│  ┌──────────┐  ┌──────────┐  ┌─────────────────┐   │
│  │ CPU Core │  │ CPU Core │  │  GPU            │   │
│  │          │  │          │  │                 │   │
│  └──────────┘  └──────────┘  └─────────────────┘   │
│                                                      │
│  ┌────────────────────────────────────────────┐     │
│  │      VIDEO DECODER BLOCK                   │     │
│  │  - H.264 decoder circuits                  │     │
│  │  - VP9 decoder circuits                    │     │
│  │  - AV1 decoder circuits (newer chips)      │     │
│  │  - Dedicated entropy decoder               │     │
│  │  - Motion compensation engine              │     │
│  │  - DCT/IDCT transform units                │     │
│  └────────────────────────────────────────────┘     │
│                                                      │
└──────────────────────────────────────────────────────┘
```

**Performance Comparison**:

| Method | Power (4K60 H.264) | CPU Usage | Laptop Battery Life |
|--------|-------------------|-----------|---------------------|
| **Software decode (FFmpeg)** | 25-35W | 400-600% (4-6 cores) | 1-2 hours |
| **Hardware decode** | 2-5W | <10% (1 core for demuxing) | 6-8 hours |

**That's a 10× power difference!** This is why your phone can play 4K video for hours without draining the battery.

### Why the Kernel Is Involved

The hardware decoder is a **device** that needs kernel-level access. Here's why the kernel must be involved:

#### 1. Hardware Access Requires Kernel Drivers

Hardware decoders are memory-mapped I/O devices:

```c
// Hardware decoder registers (example: MediaTek)
#define VDEC_BASE_ADDR    0x16000000    // Physical address
#define VDEC_CTRL_REG     0x16000000
#define VDEC_STATUS_REG   0x16000004
#define VDEC_BUF_ADDR     0x16000008
```

**Userspace cannot**:
- Map physical addresses (requires kernel `ioremap()`)
- Receive hardware interrupts (requires kernel `request_irq()`)
- Manage DMA (requires kernel DMA API)

#### 2. Memory Management

Decoded video frames are **huge**:
- 1080p NV12 frame: 3,110,400 bytes (3 MB)
- 4K NV12 frame: 12,441,600 bytes (12 MB)
- 60 FPS buffer queue (8 frames): **96 MB for 4K!**

This memory must be:
- **Physically contiguous** (hardware decoders can't handle scattered pages)
- **DMA-accessible** (decoder writes directly via DMA)
- **Shared between devices** (decoder → GPU without CPU copy)

Only the kernel can:
```c
// Allocate physically contiguous DMA memory
dma_alloc_coherent(dev, size, &dma_addr, GFP_KERNEL);

// Share buffer between decoder and GPU (zero-copy)
dma_buf_export(buffer, &dma_buf_ops, size, flags);
```

#### 3. Resource Arbitration

Multiple processes might want to use the decoder:
- Video player playing a movie
- Browser playing YouTube
- Video conferencing app

The kernel's **V4L2-MEM2MEM framework** schedules these jobs:

```c
// Process A queues decode job
ioctl(video_fd, VIDIOC_QBUF, &buf_a);  // → Queued

// Process B queues decode job
ioctl(video_fd, VIDIOC_QBUF, &buf_b);  // → Queued

// Kernel scheduler:
// 1. Run job A on hardware
// 2. Hardware IRQ when done
// 3. Run job B on hardware
// 4. Hardware IRQ when done
```

Without the kernel, processes would fight for the decoder, causing crashes or corruption.

### What the Kernel Actually Does

**Important**: The kernel does **NOT** decode video. The hardware does. The kernel just:

1. **Provides an API** (V4L2) for userspace to access the hardware
2. **Manages buffers** (allocates DMA memory, tracks ownership)
3. **Schedules jobs** (queues decode requests, runs them on hardware)
4. **Handles interrupts** (wakes up userspace when decode finishes)
5. **Shares buffers** (zero-copy via DMA-BUF to GPU/display)

### The Architecture

```
┌─────────────────────────────────────────────────────┐
│            USERSPACE APPLICATION                    │
│  - Demux video file (MP4, MKV, WebM)                │
│  - Extract compressed frames                        │
│  - Call kernel APIs                                 │
└────────┬──────────────────────────────┬─────────────┘
         │                               │
         │ V4L2 API                      │ DRM API
         │ (submit frames)               │ (display frames)
         │                               │
┌────────▼───────────────────────────────▼─────────────┐
│                   LINUX KERNEL                       │
│                                                      │
│  ┌──────────────────┐       ┌──────────────────┐    │
│  │  V4L2 DRIVER     │       │   DRM DRIVER     │    │
│  │  - Job queue     │       │   - Framebuffer  │    │
│  │  - Buffer mgmt   │◄──────┤   - Atomic API   │    │
│  │  - DMA setup     │ DMABUF│   - Plane mgmt   │    │
│  └─────────┬────────┘       └──────────┬───────┘    │
│            │                            │            │
└────────────┼────────────────────────────┼────────────┘
             │                            │
             │ MMIO, DMA, IRQ             │ MMIO, DMA
             │                            │
┌────────────▼────────────────────────────▼────────────┐
│                    HARDWARE                          │
│                                                      │
│  ┌────────────────────┐    ┌──────────────────────┐ │
│  │  VIDEO DECODER     │    │  GPU / DISPLAY       │ │
│  │  - H.264 engine    │───▶│  - Compositor        │ │
│  │  - VP9 engine      │DMA │  - Scaler            │ │
│  │  - AV1 engine      │    │  - CRTC (scanout)    │ │
│  └────────────────────┘    └──────────────────────┘ │
│         Writes to               Reads from          │
│         shared memory           shared memory       │
└─────────────────────────────────────────────────────┘
```

### Pure Software Decoding (FFmpeg)

You **can** decode video entirely in userspace with libraries like **FFmpeg**:

```c
// FFmpeg software decoding (simplified)
AVCodec *codec = avcodec_find_decoder(AV_CODEC_ID_H264);
AVCodecContext *ctx = avcodec_alloc_context3(codec);
avcodec_open2(ctx, codec, NULL);

while (1) {
    av_read_frame(format_ctx, &packet);
    avcodec_send_packet(ctx, &packet);  // ← Pure CPU decoding
    avcodec_receive_frame(ctx, &frame); // ← All in userspace

    // Now copy to GPU for display
    glTexImage2D(..., frame->data[0]);  // CPU → GPU copy
}
```

**Downsides**:
- **Slow**: Can't decode 4K in real-time on most CPUs
- **Power hungry**: Drains laptop battery in 1-2 hours
- **CPU copy**: Must copy decoded frames from CPU memory to GPU memory (slow!)

**When to use**:
- Old hardware without video decoder
- Exotic codecs not supported by hardware
- Transcoding (where you need CPU access to pixels anyway)

### Summary

| Aspect | Software Decode | Hardware Decode (Kernel) |
|--------|----------------|-------------------------|
| **Speed** | Slow (can't do 4K60) | Fast (handles 8K60) |
| **Power** | 25-35W | 2-5W |
| **CPU usage** | 400-600% | <10% |
| **Memory copies** | CPU → GPU (slow) | Zero-copy via DMA-BUF |
| **Kernel involvement** | None (pure userspace) | V4L2 driver manages hardware |
| **What does the work** | CPU running codec algorithms | Dedicated silicon circuits |

**The kernel doesn't decode video—it manages access to the hardware that does.**

## High-Level Overview

When you play a video in Linux, several kernel subsystems work together to deliver smooth playback:

```
┌─────────────────────────────────────────────────────────────────┐
│                     VIDEO PLAYER APPLICATION                     │
│                    (mpv, VLC, Firefox, etc.)                     │
└────┬──────────────────────────────────────────────────┬─────────┘
     │                                                    │
     │ Compressed video                                  │ DMA-BUF FD
     │ (H.264, VP9, AV1)                                │ (decoded frame)
     │                                                    │
     v                                                    v
┌─────────────────────────────┐         ┌──────────────────────────┐
│   V4L2 VIDEO DECODER API    │         │     DRM DISPLAY API      │
│  (/dev/video0, /dev/video1) │         │   (/dev/dri/card0)       │
└────┬────────────────────────┘         └──────┬───────────────────┘
     │                                           │
     │ VIDIOC_QBUF/DQBUF                        │ DRM_IOCTL_MODE_*
     │ VIDIOC_EXPBUF                            │ DRM_IOCTL_PRIME_*
     │                                           │
     v                                           v
┌─────────────────────────────┐         ┌──────────────────────────┐
│    V4L2-MEM2MEM FRAMEWORK   │         │   DRM ATOMIC FRAMEWORK   │
│  (Job scheduling, queuing)  │         │  (Display state machine) │
└────┬────────────────────────┘         └──────┬───────────────────┘
     │                                           │
     │                                           │
     v                                           v
┌─────────────────────────────┐         ┌──────────────────────────┐
│   HARDWARE VIDEO DECODER    │──DMA──▶│    GPU / DISPLAY ENGINE  │
│ (MediaTek, Qualcomm, Intel) │ BUF    │   (Intel i915, AMD, etc.)│
└─────────────────────────────┘         └──────────────────────────┘
                                                    │
                                                    v
                                         ┌──────────────────────────┐
                                         │  DISPLAY HARDWARE (CRTC) │
                                         │   Scans to HDMI/DP/eDP   │
                                         └──────────────────────────┘
```

**Key Insight**: Modern video playback uses **zero-copy** buffer sharing. The hardware decoder writes decoded frames directly to memory that the GPU can read, avoiding expensive CPU copies. This is enabled by **DMA-BUF**, a kernel framework for sharing buffers between devices.

## The Complete Pipeline

Let's trace a single video frame from file to display:

```
APPLICATION LAYER
├─ 1. Read compressed frame from file (H.264, VP9, AV1)
├─ 2. Submit to V4L2 decoder (/dev/videoX)
│
KERNEL: V4L2 SUBSYSTEM (refs/linux/drivers/media/v4l2-core/)
├─ 3. V4L2 queues compressed buffer
├─ 4. V4L2-MEM2MEM schedules decode job
├─ 5. Hardware decoder processes frame
│      └─ Decoder writes to DMA-accessible memory
├─ 6. Decoder signals completion via interrupt
├─ 7. V4L2 marks buffer ready, exports DMA-BUF FD
│
APPLICATION LAYER
├─ 8. App receives decoded frame as DMA-BUF FD
├─ 9. App passes FD to DRM display API
│
KERNEL: DRM SUBSYSTEM (refs/linux/drivers/gpu/drm/)
├─ 10. DRM imports DMA-BUF as framebuffer
├─ 11. DRM waits for decoder fence (synchronization)
├─ 12. DRM atomically updates display state
├─ 13. GPU/Display engine scans out framebuffer
│
HARDWARE
└─ 14. CRTC reads framebuffer at vblank
       └─ Pixels appear on screen
```

**Timeframe**: Steps 1-14 typically complete in 16ms (60 FPS) or 8ms (120 FPS).

## Video Decoding with V4L2

### V4L2 Device Model

**V4L2 (Video4Linux2)** is the Linux kernel's video device API. Video decoders appear as `/dev/videoX` devices.

#### Key Data Structure: `struct video_device`

**Location**: `refs/linux/include/media/v4l2-dev.h`

```c
struct video_device {
    struct device dev;               // Linux device
    struct cdev *cdev;               // Character device
    struct v4l2_device *v4l2_dev;    // Parent V4L2 device

    // Device type
    enum vfl_devnode_type type;      // VIDEO, VBI, RADIO, SUBDEV, etc.
    enum vfl_devnode_direction direction;  // RX, TX, M2M

    // File operations
    const struct v4l2_file_operations *fops;
    const struct v4l2_ioctl_ops *ioctl_ops;

    // Properties
    char name[32];                   // "mtk-vcodec-dec", "venus-decoder"
    int vfl_dir;                     // VFL_DIR_RX, VFL_DIR_TX, VFL_DIR_M2M

    // Reference counting
    struct kref kref;
    void (*release)(struct video_device *vdev);
};
```

**Device Types**:
- `VFL_TYPE_VIDEO` - Video input/output devices
- `VFL_DIR_M2M` - **Memory-to-Memory** (codecs, scalers)
  - Input: Compressed video
  - Output: Raw video frames

#### Device Registration

**File**: `refs/linux/drivers/media/v4l2-core/v4l2-dev.c:1247`

```c
int video_register_device(struct video_device *vdev,
                          enum vfl_devnode_type type,
                          int nr)
{
    // Assign device number
    vdev->num = get_index(vdev);

    // Create character device
    cdev_init(&vdev->cdev, &v4l2_fops);
    cdev_add(&vdev->cdev, MKDEV(VIDEO_MAJOR, vdev->minor));

    // Create /dev/videoX node
    device_create(video_device_class, vdev->dev.parent,
                  MKDEV(VIDEO_MAJOR, vdev->minor), NULL,
                  "%s%d", name_base, vdev->num);
}
```

**Result**: Creates `/dev/video0`, `/dev/video1`, etc.

### Memory-to-Memory Codec Framework

The **V4L2-MEM2MEM framework** provides infrastructure for devices that transform data between memory buffers (codecs, scalers, format converters).

#### Architecture

```
┌────────────────────────────────────────────────────────┐
│              V4L2 M2M DEVICE (/dev/videoX)             │
└────────────────────┬───────────────────────────────────┘
                     │
        ┌────────────┴────────────┐
        │                         │
        v                         v
┌─────────────────┐      ┌─────────────────┐
│  OUTPUT QUEUE   │      │  CAPTURE QUEUE  │
│  (compressed)   │      │  (raw frames)   │
└────────┬────────┘      └────────┬────────┘
         │                         │
    VIDIOC_QBUF              VIDIOC_DQBUF
         │                         │
         v                         v
   ┌─────────────────────────────────┐
   │      V4L2-MEM2MEM SCHEDULER     │
   │  - Job queue management         │
   │  - Device scheduling            │
   └────────────┬────────────────────┘
                │
                v
   ┌─────────────────────────────────┐
   │     HARDWARE DECODER DRIVER     │
   │  .device_run() callback         │
   └─────────────────────────────────┘
```

#### Key Data Structure: `struct v4l2_m2m_dev`

**Location**: `refs/linux/include/media/v4l2-mem2mem.h`

```c
struct v4l2_m2m_dev {
    struct v4l2_m2m_ctx *curr_ctx;      // Currently running context
    struct list_head job_queue;          // Queued jobs
    spinlock_t job_spinlock;             // Lock for job queue

    struct work_struct job_work;         // Workqueue for scheduling
    unsigned long job_queue_flags;       // QUEUE_PAUSED, etc.

    const struct v4l2_m2m_ops *m2m_ops;  // Driver callbacks
};
```

#### Driver Operations: `struct v4l2_m2m_ops`

```c
struct v4l2_m2m_ops {
    // Called when buffers are ready on both queues
    void (*device_run)(void *priv);

    // Check if device is ready to run
    int (*job_ready)(void *priv);

    // Abort current job
    void (*job_abort)(void *priv);

    // Lock/unlock for queue access
    void (*lock)(void *priv);
    void (*unlock)(void *priv);
};
```

#### Job Scheduling Flow

**File**: `refs/linux/drivers/media/v4l2-core/v4l2-mem2mem.c:1643`

```c
void v4l2_m2m_try_schedule(struct v4l2_m2m_ctx *m2m_ctx)
{
    struct v4l2_m2m_dev *m2m_dev = m2m_ctx->m2m_dev;

    // Check if both queues have buffers
    if (!v4l2_m2m_num_src_bufs_ready(m2m_ctx) ||
        !v4l2_m2m_num_dst_bufs_ready(m2m_ctx)) {
        return;  // Not ready yet
    }

    // Check if device is ready
    if (m2m_dev->m2m_ops->job_ready &&
        !m2m_dev->m2m_ops->job_ready(m2m_ctx->priv)) {
        return;  // Device not ready
    }

    // Schedule job
    spin_lock_irqsave(&m2m_dev->job_spinlock, flags);
    list_add_tail(&m2m_ctx->queue, &m2m_dev->job_queue);
    spin_unlock_irqrestore(&m2m_dev->job_spinlock, flags);

    // Run immediately if device idle
    if (m2m_dev->curr_ctx == NULL) {
        m2m_dev->curr_ctx = m2m_ctx;
        m2m_dev->m2m_ops->device_run(m2m_ctx->priv);  // ← HARDWARE STARTS
    }
}
```

**Key Point**: The framework calls `device_run()` when:
1. At least one compressed buffer is queued (OUTPUT queue)
2. At least one empty buffer is queued (CAPTURE queue)
3. Device is not busy with another job

### Buffer Management with VideoBuf2

**VideoBuf2 (VB2)** manages video buffer allocation and queueing.

#### Buffer Memory Types

```c
enum vb2_memory {
    VB2_MEMORY_MMAP,      // Kernel-allocated, mmap'd to userspace
    VB2_MEMORY_USERPTR,   // Userspace-allocated pointers
    VB2_MEMORY_DMABUF,    // DMA-BUF file descriptors (zero-copy!)
};
```

#### Key Structure: `struct vb2_queue`

**Location**: `refs/linux/include/media/videobuf2-core.h`

```c
struct vb2_queue {
    enum vb2_queue_type type;           // VIDEO_OUTPUT, VIDEO_CAPTURE
    enum vb2_memory memory;             // MMAP, USERPTR, DMABUF

    unsigned int num_buffers;           // Number of allocated buffers
    struct list_head queued_list;       // Queued buffers

    const struct vb2_ops *ops;          // Driver callbacks
    const struct vb2_mem_ops *mem_ops;  // Memory operations

    void *drv_priv;                     // Driver private data
    struct device *dev;                 // DMA device
};
```

#### DMA-Contiguous Memory Operations

**File**: `refs/linux/include/media/videobuf2-dma-contig.h`

```c
extern const struct vb2_mem_ops vb2_dma_contig_memops;

// Get DMA address of buffer plane
dma_addr_t vb2_dma_contig_plane_dma_addr(struct vb2_buffer *vb,
                                         unsigned int plane_no);
```

**Implementation**: Allocates physically contiguous DMA memory that hardware decoders can access directly.

## Zero-Copy Buffer Sharing with DMA-BUF

### DMA-BUF Architecture

**DMA-BUF** enables **zero-copy** buffer sharing between different devices (decoder ↔ GPU, GPU ↔ display).

```
┌──────────────────────────────────────────────────────────┐
│                     APPLICATION                          │
│                                                          │
│  fd = ioctl(video_fd, VIDIOC_EXPBUF, &expbuf);          │
│       ^^^ DMA-BUF file descriptor                        │
│                                                          │
│  ioctl(drm_fd, DRM_IOCTL_PRIME_FD_TO_HANDLE, &fd);      │
│       ^^^ Import DMA-BUF to GPU                          │
└──────────────────────────────────────────────────────────┘
                          │
                          │ DMA-BUF FD (file descriptor)
                          │
         ┌────────────────┴─────────────────┐
         │                                  │
         v                                  v
┌──────────────────┐              ┌──────────────────┐
│  VIDEO DECODER   │              │   GPU / DISPLAY  │
│  (Producer)      │              │   (Consumer)     │
└────────┬─────────┘              └────────┬─────────┘
         │                                  │
         │      ┌──────────────────┐       │
         └─────▶│    DMA-BUF       │◀──────┘
                │  (Shared Memory) │
                │                  │
                │  - Reference cnt │
                │  - Attachments   │
                │  - DMA fences    │
                └──────────────────┘
```

#### Key Structure: `struct dma_buf`

**Location**: `refs/linux/include/linux/dma-buf.h`

```c
struct dma_buf {
    size_t size;                        // Buffer size
    struct file *file;                  // Backing file

    struct list_head attachments;       // Devices using this buffer
    const struct dma_buf_ops *ops;      // Export operations

    void *priv;                         // Exporter private data
    struct dma_resv *resv;              // Reservation object (fences)

    // Userspace metadata
    const char *name;
    spinlock_t name_lock;
};
```

#### DMA-BUF Operations

```c
struct dma_buf_ops {
    // Device attachment/detachment
    int (*attach)(struct dma_buf *, struct dma_buf_attachment *);
    void (*detach)(struct dma_buf *, struct dma_buf_attachment *);

    // Pin/unpin for DMA access
    int (*pin)(struct dma_buf_attachment *);
    void (*unpin)(struct dma_buf_attachment *);

    // Map for DMA access (returns scatter-gather table)
    struct sg_table *(*map_dma_buf)(struct dma_buf_attachment *,
                                     enum dma_data_direction);
    void (*unmap_dma_buf)(struct dma_buf_attachment *,
                          struct sg_table *,
                          enum dma_data_direction);

    // CPU access (optional, usually not needed for video)
    int (*vmap)(struct dma_buf *, struct iosys_map *);
    void (*vunmap)(struct dma_buf *, struct iosys_map *);
};
```

### Synchronization with DMA Fences

**Problem**: How does the GPU know when the decoder has finished writing the frame?

**Solution**: **DMA fences** - synchronization primitives attached to buffers.

#### Key Structure: `struct dma_fence`

**Location**: `refs/linux/include/linux/dma-fence.h`

```c
struct dma_fence {
    spinlock_t *lock;                   // Synchronization
    const struct dma_fence_ops *ops;    // Fence operations

    struct list_head cb_list;           // Completion callbacks
    u64 context;                        // Fence context
    u64 seqno;                          // Sequence number

    unsigned long flags;
    ktime_t timestamp;                  // When fence was signaled
    int error;
};
```

#### Fence Operations

```c
struct dma_fence_ops {
    // Get driver name
    const char *(*get_driver_name)(struct dma_fence *);
    const char *(*get_timeline_name)(struct dma_fence *);

    // Enable signaling (activate hardware interrupts)
    bool (*enable_signaling)(struct dma_fence *);

    // Check if fence is signaled
    bool (*signaled)(struct dma_fence *);

    // Wait for fence completion
    signed long (*wait)(struct dma_fence *, bool intr, signed long timeout);

    // Release fence
    void (*release)(struct dma_fence *);
};
```

#### DMA Reservation Objects

**Location**: `refs/linux/include/linux/dma-resv.h`

```c
struct dma_resv {
    struct ww_mutex lock;               // Wound-wait mutex
    struct dma_resv_list *fences;       // List of pending fences
};
```

Each DMA-BUF has a `dma_resv` object that tracks all pending operations:

```c
// Add write fence (decoder is writing)
void dma_resv_add_fence(struct dma_resv *obj,
                        struct dma_fence *fence,
                        enum dma_resv_usage usage);

// Wait for all fences to complete
long dma_resv_wait_timeout(struct dma_resv *obj,
                           enum dma_resv_usage usage,
                           bool intr, unsigned long timeout);
```

### Exporting Decoded Frames

#### From V4L2 to DMA-BUF

**Syscall**: `ioctl(fd, VIDIOC_EXPBUF, &expbuf)`

**Implementation**: `refs/linux/drivers/media/v4l2-core/v4l2-ioctl.c`

```c
static int v4l2_ioctl_expbuf(struct file *file, void *priv,
                             struct v4l2_exportbuffer *p)
{
    struct video_device *vdev = video_devdata(file);

    // Get VB2 queue (OUTPUT or CAPTURE)
    struct vb2_queue *q = v4l2_m2m_get_vq(m2m_ctx, p->type);

    // Export buffer as DMA-BUF
    return vb2_expbuf(q, p);
}
```

**VideoBuf2 Export**: `refs/linux/drivers/media/common/videobuf2/videobuf2-core.c`

```c
int vb2_expbuf(struct vb2_queue *q, struct v4l2_exportbuffer *eb)
{
    struct vb2_buffer *vb = q->bufs[eb->index];
    struct vb2_plane *plane = &vb->planes[eb->plane];

    // Get DMA-BUF from memory allocator
    struct dma_buf *dbuf = plane->mem_ops->get_dmabuf(
        vb, plane->mem_priv, eb->flags);

    // Create file descriptor
    int fd = dma_buf_fd(dbuf, O_CLOEXEC);

    eb->fd = fd;
    return 0;
}
```

**Result**: Application receives a file descriptor representing the decoded frame, which can be passed to the GPU.

## Display Pipeline with DRM

### DRM Display Objects

The **DRM (Direct Rendering Manager)** subsystem manages displays. The display pipeline consists of several objects:

```
┌─────────────────────────────────────────────────────┐
│                   FRAMEBUFFER                       │
│  - Wraps video frame memory (GEM object)            │
│  - Pixel format (NV12, ARGB8888, etc.)              │
│  - Width, height, stride                            │
└──────────────────┬──────────────────────────────────┘
                   │
                   v
┌─────────────────────────────────────────────────────┐
│                     PLANE                           │
│  - Image source layer                               │
│  - Crop/scale/position                              │
│  - Blending (alpha, zpos)                           │
│  - Types: PRIMARY, OVERLAY, CURSOR                  │
└──────────────────┬──────────────────────────────────┘
                   │
                   v
┌─────────────────────────────────────────────────────┐
│                     CRTC                            │
│  - Display timing controller                        │
│  - Composes multiple planes                         │
│  - Generates video timing (hsync, vsync)            │
└──────────────────┬──────────────────────────────────┘
                   │
                   v
┌─────────────────────────────────────────────────────┐
│                   ENCODER                           │
│  - Signal format conversion                         │
│  - TMDS (HDMI), DP, LVDS, DSI                       │
└──────────────────┬──────────────────────────────────┘
                   │
                   v
┌─────────────────────────────────────────────────────┐
│                  CONNECTOR                          │
│  - Physical output port                             │
│  - HDMI-A, DisplayPort, eDP, VGA                    │
│  - EDID reading                                     │
└─────────────────────────────────────────────────────┘
```

#### 1. Framebuffer

**File**: `refs/linux/include/drm/drm_framebuffer.h`

```c
struct drm_framebuffer {
    struct drm_device *dev;

    unsigned int width;                 // Width in pixels
    unsigned int height;                // Height in pixels
    unsigned int pitches[4];            // Bytes per line (per plane)
    unsigned int offsets[4];            // Byte offset of each plane

    uint32_t pixel_format;              // DRM_FORMAT_* fourcc code
    const struct drm_format_info *format;

    struct list_head head;
    struct drm_mode_object base;

    const struct drm_framebuffer_funcs *funcs;
};
```

**Common Formats**:
- `DRM_FORMAT_NV12` - YUV 4:2:0, used by video decoders
- `DRM_FORMAT_ARGB8888` - RGB with alpha, used by compositors
- `DRM_FORMAT_XRGB8888` - RGB without alpha

#### 2. Plane

**File**: `refs/linux/include/drm/drm_plane.h`

```c
struct drm_plane {
    struct drm_device *dev;
    struct list_head head;

    char *name;                         // "plane-0", "cursor"

    enum drm_plane_type type;           // PRIMARY, OVERLAY, CURSOR
    unsigned long possible_crtcs;       // Bitmask of compatible CRTCs

    uint32_t *format_types;             // Supported pixel formats
    unsigned int format_count;

    struct drm_plane_state *state;      // Current state

    const struct drm_plane_funcs *funcs;
};

enum drm_plane_type {
    DRM_PLANE_TYPE_PRIMARY,             // Main display surface
    DRM_PLANE_TYPE_OVERLAY,             // Additional layers
    DRM_PLANE_TYPE_CURSOR,              // Hardware cursor
};
```

**Plane State**: `struct drm_plane_state`

```c
struct drm_plane_state {
    struct drm_plane *plane;
    struct drm_crtc *crtc;              // Target CRTC
    struct drm_framebuffer *fb;         // Framebuffer to display

    // Source rectangle (in 16.16 fixed-point)
    uint32_t src_x, src_y;              // Crop start
    uint32_t src_w, src_h;              // Crop size

    // Destination rectangle
    int32_t crtc_x, crtc_y;             // Position on screen
    uint32_t crtc_w, crtc_h;            // Size on screen

    // Blending
    uint16_t alpha;                     // Global alpha (0-65535)
    uint32_t pixel_blend_mode;          // Pre-multiplied, coverage, none
    uint32_t zpos;                      // Z-order (0 = bottom)

    unsigned int rotation;              // DRM_MODE_ROTATE_*, DRM_MODE_REFLECT_*

    // Synchronization
    struct dma_fence *fence;            // Wait for buffer ready

    bool visible;                       // Plane visible after clipping
};
```

#### 3. CRTC (Display Controller)

**File**: `refs/linux/include/drm/drm_crtc.h`

```c
struct drm_crtc {
    struct drm_device *dev;
    struct list_head head;
    char *name;                         // "crtc-0", "crtc-1"

    struct drm_plane *primary;          // Primary plane
    struct drm_plane *cursor;           // Cursor plane (optional)

    bool enabled;                       // CRTC enabled

    struct drm_display_mode mode;       // Display timing
    struct drm_crtc_state *state;       // Current state

    const struct drm_crtc_funcs *funcs;
};
```

**Display Mode**: `struct drm_display_mode`

```c
struct drm_display_mode {
    int clock;                          // Pixel clock (kHz)

    // Horizontal timing
    int hdisplay;                       // Visible width
    int hsync_start;
    int hsync_end;
    int htotal;

    // Vertical timing
    int vdisplay;                       // Visible height
    int vsync_start;
    int vsync_end;
    int vtotal;

    int vrefresh;                       // Refresh rate (Hz)

    char name[DRM_DISPLAY_MODE_LEN];    // "1920x1080@60"
};
```

Example: **1920x1080@60Hz**
- `hdisplay = 1920`, `vdisplay = 1080`
- `vrefresh = 60`
- `clock = 148500` (148.5 MHz pixel clock)

#### 4. Connector

**File**: `refs/linux/include/drm/drm_connector.h`

```c
struct drm_connector {
    struct drm_device *dev;
    char *name;                         // "HDMI-A-1", "DP-1"

    int connector_type;                 // DRM_MODE_CONNECTOR_*
    int connector_type_id;              // Instance number

    enum drm_connector_status status;   // connected, disconnected, unknown

    struct list_head modes;             // Supported modes (from EDID)
    struct edid *edid_blob_ptr;         // Raw EDID data

    struct drm_encoder *encoder;        // Connected encoder

    const struct drm_connector_funcs *funcs;
};
```

### Atomic Display Updates

Modern DRM uses **atomic updates** where all changes are applied together at vblank (no tearing).

#### Atomic State Machine

```
┌─────────────────────────────────────────────────────┐
│  1. Create atomic state                             │
│     drm_atomic_state_alloc()                        │
└──────────────────┬──────────────────────────────────┘
                   v
┌─────────────────────────────────────────────────────┐
│  2. Get object states                               │
│     drm_atomic_get_plane_state()                    │
│     drm_atomic_get_crtc_state()                     │
└──────────────────┬──────────────────────────────────┘
                   v
┌─────────────────────────────────────────────────────┐
│  3. Modify state properties                         │
│     - Set framebuffer                               │
│     - Set position/crop                             │
│     - Set fences                                    │
└──────────────────┬──────────────────────────────────┘
                   v
┌─────────────────────────────────────────────────────┐
│  4. Check configuration                             │
│     drm_atomic_check()                              │
│     - Validate mode                                 │
│     - Check resource constraints                    │
└──────────────────┬──────────────────────────────────┘
                   │
                   v
           ┌───────┴──────┐
           │  Valid?      │
           └───┬──────┬───┘
               │      │
            NO │      │ YES
               v      v
           [Fail]  ┌─────────────────────────────────┐
                   │  5. Commit atomically           │
                   │     drm_atomic_commit()         │
                   │     - Wait for fences           │
                   │     - Program hardware          │
                   │     - Apply at vblank           │
                   └─────────────────────────────────┘
```

#### Atomic Commit Implementation

**File**: `refs/linux/drivers/gpu/drm/drm_atomic_helper.c:3794`

```c
int drm_atomic_helper_commit(struct drm_device *dev,
                              struct drm_atomic_state *state,
                              bool nonblock)
{
    // 1. Wait for fences (decoder finished writing)
    for_each_new_plane_in_state(state, plane, plane_state, i) {
        if (plane_state->fence) {
            ret = dma_fence_wait(plane_state->fence, true);
            if (ret)
                return ret;
        }
    }

    // 2. Prepare commit (allocate resources)
    funcs->atomic_commit_prepare(dev, state);

    // 3. Disable old planes/crtcs
    drm_atomic_helper_commit_modeset_disables(dev, state);

    // 4. Program new plane configurations
    drm_atomic_helper_commit_planes(dev, state, 0);

    // 5. Enable new crtcs/encoders
    drm_atomic_helper_commit_modeset_enables(dev, state);

    // 6. Wait for vblank (changes take effect)
    drm_atomic_helper_wait_for_vblanks(dev, state);

    return 0;
}
```

**Key Insight**: All changes happen atomically at vblank - no tearing or partial updates visible.

### Importing DMA-BUF to GPU

#### Step 1: Import DMA-BUF as GEM Object

**Syscall**: `ioctl(drm_fd, DRM_IOCTL_PRIME_FD_TO_HANDLE, &args)`

```c
struct drm_prime_handle {
    __u32 handle;       // OUT: GEM handle
    __u32 flags;
    __s32 fd;           // IN: DMA-BUF file descriptor
};
```

**Implementation**: `refs/linux/drivers/gpu/drm/drm_prime.c`

```c
int drm_gem_prime_fd_to_handle(struct drm_device *dev,
                               struct drm_file *file_priv,
                               int prime_fd, uint32_t *handle)
{
    // Get DMA-BUF from file descriptor
    struct dma_buf *dma_buf = dma_buf_get(prime_fd);

    // Check if already imported
    obj = drm_gem_prime_lookup(file_priv, dma_buf);
    if (obj) {
        *handle = obj->handle;
        return 0;
    }

    // Import DMA-BUF as GEM object
    obj = dev->driver->gem_prime_import(dev, dma_buf);

    // Attach GPU to DMA-BUF
    dma_buf_attach(dma_buf, dev->dev);

    // Create handle for userspace
    ret = drm_gem_handle_create(file_priv, obj, handle);

    return 0;
}
```

#### Step 2: Create Framebuffer from GEM

**Syscall**: `ioctl(drm_fd, DRM_IOCTL_MODE_ADDFB2, &fb)`

```c
struct drm_mode_fb_cmd2 {
    __u32 fb_id;                // OUT: Framebuffer ID
    __u32 width, height;        // Frame dimensions
    __u32 pixel_format;         // DRM_FORMAT_NV12, etc.
    __u32 flags;

    __u32 handles[4];           // GEM handles (one per plane)
    __u32 pitches[4];           // Bytes per line
    __u32 offsets[4];           // Plane offsets
    __u64 modifier[4];          // Tiling modifiers
};
```

**Implementation**: `refs/linux/drivers/gpu/drm/drm_framebuffer.c`

```c
int drm_mode_addfb2(struct drm_device *dev,
                    void *data, struct drm_file *file_priv)
{
    struct drm_mode_fb_cmd2 *r = data;

    // Lookup GEM objects from handles
    for (i = 0; i < num_planes; i++) {
        obj[i] = drm_gem_object_lookup(file_priv, r->handles[i]);
    }

    // Create framebuffer
    fb = dev->mode_config.funcs->fb_create(dev, file_priv, r);

    r->fb_id = fb->base.id;
    return 0;
}
```

#### Step 3: Attach Framebuffer to Plane

**Syscall**: `ioctl(drm_fd, DRM_IOCTL_MODE_ATOMIC, &atomic)`

```c
// Simplified: Set plane properties
drmModeAtomicReq *req = drmModeAtomicAlloc();
drmModeAtomicAddProperty(req, plane_id, prop_fb_id, fb_id);
drmModeAtomicAddProperty(req, plane_id, prop_crtc_id, crtc_id);
drmModeAtomicAddProperty(req, plane_id, prop_src_x, src_x << 16);
drmModeAtomicAddProperty(req, plane_id, prop_src_y, src_y << 16);
drmModeAtomicAddProperty(req, plane_id, prop_src_w, src_w << 16);
drmModeAtomicAddProperty(req, plane_id, prop_src_h, src_h << 16);
drmModeAtomicAddProperty(req, plane_id, prop_crtc_x, crtc_x);
drmModeAtomicAddProperty(req, plane_id, prop_crtc_y, crtc_y);
drmModeAtomicAddProperty(req, plane_id, prop_crtc_w, crtc_w);
drmModeAtomicAddProperty(req, plane_id, prop_crtc_h, crtc_h);

drmModeAtomicCommit(drm_fd, req, DRM_MODE_ATOMIC_NONBLOCK, NULL);
```

## Complete Flow: Syscalls to Hardware

Let's trace a complete frame decode and display with actual syscalls:

### Application Code (Pseudo-code)

```c
// ============================================================
// PHASE 1: SETUP DECODER
// ============================================================

// Open video decoder
int video_fd = open("/dev/video0", O_RDWR);

// Query capabilities
struct v4l2_capability cap;
ioctl(video_fd, VIDIOC_QUERYCAP, &cap);
// Result: cap.device_caps & V4L2_CAP_VIDEO_M2M_MPLANE

// Set input format (compressed)
struct v4l2_format fmt_in = {
    .type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE,
    .fmt.pix_mp = {
        .width = 1920,
        .height = 1080,
        .pixelformat = V4L2_PIX_FMT_H264,  // H.264 compressed
    }
};
ioctl(video_fd, VIDIOC_S_FMT, &fmt_in);

// Set output format (raw frames)
struct v4l2_format fmt_out = {
    .type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE,
    .fmt.pix_mp = {
        .width = 1920,
        .height = 1080,
        .pixelformat = V4L2_PIX_FMT_NV12,  // YUV 4:2:0
    }
};
ioctl(video_fd, VIDIOC_S_FMT, &fmt_out);

// Allocate buffers (OUTPUT queue - compressed)
struct v4l2_requestbuffers reqbufs_in = {
    .count = 4,
    .type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE,
    .memory = V4L2_MEMORY_MMAP,
};
ioctl(video_fd, VIDIOC_REQBUFS, &reqbufs_in);

// Allocate buffers (CAPTURE queue - decoded)
struct v4l2_requestbuffers reqbufs_out = {
    .count = 8,
    .type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE,
    .memory = V4L2_MEMORY_DMABUF,  // Use DMA-BUF for zero-copy
};
ioctl(video_fd, VIDIOC_REQBUFS, &reqbufs_out);

// Start streaming
ioctl(video_fd, VIDIOC_STREAMON, &(enum v4l2_buf_type){
    V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE});
ioctl(video_fd, VIDIOC_STREAMON, &(enum v4l2_buf_type){
    V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE});

// ============================================================
// PHASE 2: DECODE FRAME
// ============================================================

// Queue compressed data
struct v4l2_buffer buf_in = {
    .type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE,
    .memory = V4L2_MEMORY_MMAP,
    .index = 0,
};
ioctl(video_fd, VIDIOC_QBUF, &buf_in);
// → Kernel: v4l2_m2m_try_schedule() → device_run() → HARDWARE STARTS

// Queue empty buffer for output
struct v4l2_buffer buf_out = {
    .type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE,
    .memory = V4L2_MEMORY_DMABUF,
    .index = 0,
};
ioctl(video_fd, VIDIOC_QBUF, &buf_out);

// Wait for decoded frame
ioctl(video_fd, VIDIOC_DQBUF, &buf_out);  // BLOCKS until decode complete
// → Kernel: Hardware IRQ → wake_up_ctx() → buffer ready

// Export as DMA-BUF
struct v4l2_exportbuffer expbuf = {
    .type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE,
    .index = buf_out.index,
    .plane = 0,
};
ioctl(video_fd, VIDIOC_EXPBUF, &expbuf);
int dmabuf_fd = expbuf.fd;  // ← DMA-BUF file descriptor

// ============================================================
// PHASE 3: DISPLAY FRAME
// ============================================================

// Open DRM device
int drm_fd = open("/dev/dri/card0", O_RDWR);

// Import DMA-BUF as GEM object
struct drm_prime_handle prime = {
    .fd = dmabuf_fd,
};
ioctl(drm_fd, DRM_IOCTL_PRIME_FD_TO_HANDLE, &prime);
uint32_t gem_handle = prime.handle;

// Create framebuffer
struct drm_mode_fb_cmd2 fb_cmd = {
    .width = 1920,
    .height = 1080,
    .pixel_format = DRM_FORMAT_NV12,
    .handles = { gem_handle, gem_handle },  // Y and UV planes
    .pitches = { 1920, 1920 },
    .offsets = { 0, 1920 * 1080 },  // UV starts after Y
};
ioctl(drm_fd, DRM_IOCTL_MODE_ADDFB2, &fb_cmd);
uint32_t fb_id = fb_cmd.fb_id;

// Atomic update: display framebuffer on plane
drmModeAtomicReq *req = drmModeAtomicAlloc();

// Set plane properties
drmModeAtomicAddProperty(req, plane_id, prop_fb_id, fb_id);
drmModeAtomicAddProperty(req, plane_id, prop_crtc_id, crtc_id);
drmModeAtomicAddProperty(req, plane_id, prop_src_x, 0);
drmModeAtomicAddProperty(req, plane_id, prop_src_y, 0);
drmModeAtomicAddProperty(req, plane_id, prop_src_w, 1920 << 16);
drmModeAtomicAddProperty(req, plane_id, prop_src_h, 1080 << 16);
drmModeAtomicAddProperty(req, plane_id, prop_crtc_x, 0);
drmModeAtomicAddProperty(req, plane_id, prop_crtc_y, 0);
drmModeAtomicAddProperty(req, plane_id, prop_crtc_w, 1920);
drmModeAtomicAddProperty(req, plane_id, prop_crtc_h, 1080);

// Commit (apply at next vblank)
drmModeAtomicCommit(drm_fd, req, DRM_MODE_ATOMIC_NONBLOCK, NULL);
// → Kernel: Wait for decoder fence → Program hardware → Apply at vblank

// Frame appears on screen!
```

### Kernel Call Chain

#### Decode Path

```
Application: ioctl(VIDIOC_QBUF)
    ↓
v4l2_ioctl() [refs/linux/drivers/media/v4l2-core/v4l2-ioctl.c]
    ↓
vb2_ioctl_qbuf() [refs/linux/drivers/media/common/videobuf2/videobuf2-v4l2.c]
    ↓
vb2_core_qbuf() [refs/linux/drivers/media/common/videobuf2/videobuf2-core.c]
    ↓
v4l2_m2m_qbuf() [refs/linux/drivers/media/v4l2-core/v4l2-mem2mem.c]
    ↓
v4l2_m2m_try_schedule()
    ↓
v4l2_m2m_ops->device_run()  ← DRIVER CALLBACK
    ↓
mtk_vcodec_dec_device_run() [refs/linux/drivers/media/platform/mediatek/vcodec/decoder/mtk_vcodec_dec.c]
    ↓
mtk_vdec_worker() [workqueue]
    ↓
vpu_dec_start()  ← START HARDWARE
    ↓
[HARDWARE DECODING... asynchronous]
    ↓
[INTERRUPT]
    ↓
mtk_vcodec_dec_irq_handler() [refs/linux/drivers/media/platform/mediatek/vcodec/decoder/mtk_vcodec_dec_drv.c]
    ↓
wake_up_dec_ctx()
    ↓
v4l2_m2m_buf_done_and_job_finish()
    ↓
wake_up(&done_wait)
    ↓
Application: ioctl(VIDIOC_DQBUF) returns
```

#### Display Path

```
Application: ioctl(DRM_IOCTL_MODE_ATOMIC)
    ↓
drm_mode_atomic_ioctl() [refs/linux/drivers/gpu/drm/drm_atomic_uapi.c]
    ↓
drm_atomic_commit() [refs/linux/drivers/gpu/drm/drm_atomic.c]
    ↓
drm_atomic_helper_commit() [refs/linux/drivers/gpu/drm/drm_atomic_helper.c:3794]
    ↓
┌─────────────────────────────────────────┐
│ 1. Wait for fences                      │
│    dma_fence_wait(plane_state->fence)   │
│    ← Wait for decoder to finish         │
└──────────────────┬──────────────────────┘
                   ↓
┌─────────────────────────────────────────┐
│ 2. Prepare commit                       │
│    funcs->atomic_commit_prepare()       │
└──────────────────┬──────────────────────┘
                   ↓
┌─────────────────────────────────────────┐
│ 3. Disable old planes/crtcs             │
│    drm_atomic_helper_commit_modeset_... │
└──────────────────┬──────────────────────┘
                   ↓
┌─────────────────────────────────────────┐
│ 4. Program planes                       │
│    drm_atomic_helper_commit_planes()    │
│    ← Write framebuffer address to hw    │
└──────────────────┬──────────────────────┘
                   ↓
┌─────────────────────────────────────────┐
│ 5. Enable new crtcs                     │
│    drm_atomic_helper_commit_modeset_... │
└──────────────────┬──────────────────────┘
                   ↓
┌─────────────────────────────────────────┐
│ 6. Wait for vblank                      │
│    drm_atomic_helper_wait_for_vblanks() │
│    ← Changes take effect                │
└──────────────────┬──────────────────────┘
                   ↓
           [FRAME ON SCREEN]
```

## Hardware Decoder Deep Dive

Let's examine a real hardware decoder implementation.

### MediaTek VCODEC Decoder

**Location**: `refs/linux/drivers/media/platform/mediatek/vcodec/decoder/`

#### Device Initialization

**File**: `mtk_vcodec_dec_drv.c`

```c
static int mtk_vcodec_probe(struct platform_device *pdev)
{
    struct mtk_vcodec_dev *dev;

    // Allocate device structure
    dev = devm_kzalloc(&pdev->dev, sizeof(*dev), GFP_KERNEL);
    dev->plat_dev = pdev;

    // Get hardware register bases (11 register spaces)
    for (i = 0; i < NUM_MAX_VDEC_REG_BASE; i++) {
        res = platform_get_resource(pdev, IORESOURCE_MEM, i);
        dev->reg_base[i] = devm_ioremap_resource(&pdev->dev, res);
    }

    // Register interrupt handler
    dev->dec_irq = platform_get_irq(pdev, 0);
    ret = devm_request_irq(&pdev->dev, dev->dec_irq,
                           mtk_vcodec_dec_irq_handler, 0,
                           pdev->name, dev);

    // Initialize V4L2 device
    ret = v4l2_device_register(&pdev->dev, &dev->v4l2_dev);

    // Initialize M2M device
    dev->m2m_dev_dec = v4l2_m2m_init(&mtk_vdec_m2m_ops);

    // Register video device
    vfd_dec = video_device_alloc();
    vfd_dec->fops = &mtk_vcodec_fops;
    vfd_dec->ioctl_ops = &mtk_vdec_ioctl_ops;
    vfd_dec->v4l2_dev = &dev->v4l2_dev;
    vfd_dec->vfl_dir = VFL_DIR_M2M;

    ret = video_register_device(vfd_dec, VFL_TYPE_VIDEO, -1);
    // Creates /dev/videoX node

    return 0;
}
```

#### Register Map

```c
enum mtk_vdec_hw_reg_idx {
    VDEC_SYS,       // 0x00 - System control, clock gating
    VDEC_MISC,      // 0x04 - Interrupt status/control
    VDEC_LD,        // 0x08 - Loader (firmware loading)
    VDEC_TOP,       // 0x0C - Top-level control
    VDEC_CM,        // 0x10 - Common control
    VDEC_AD,        // 0x14 - Address mapping
    VDEC_AV,        // 0x18 - AV sync
    VDEC_PP,        // 0x1C - Post-processing
    VDEC_HWD,       // 0x20 - Hardware decoder
    VDEC_HWQ,       // 0x24 - Hardware queue
    VDEC_HWB,       // 0x28 - Hardware buffer
    VDEC_HWG,       // 0x2C - Hardware global
    NUM_MAX_VDEC_REG_BASE,
};
```

#### Interrupt Handler

```c
static irqreturn_t mtk_vcodec_dec_irq_handler(int irq, void *priv)
{
    struct mtk_vcodec_dev *dev = priv;
    struct mtk_vcodec_ctx *ctx;
    u32 status;

    // Read interrupt status
    status = readl(dev->reg_base[VDEC_MISC] + VDEC_IRQ_CFG_REG);

    if (status & MTK_VDEC_IRQ_STATUS_DEC_SUCCESS) {
        // Decode succeeded
        ctx = dev->curr_dec_ctx;
        wake_up_dec_ctx(ctx, MTK_INST_IRQ_RECEIVED);

        // Clear interrupt
        writel(status | VDEC_IRQ_CLR,
               dev->reg_base[VDEC_MISC] + VDEC_IRQ_CFG_REG);
    }

    return IRQ_HANDLED;
}
```

#### Decode Job Execution

```c
static void mtk_vcodec_dec_device_run(void *priv)
{
    struct mtk_vcodec_ctx *ctx = priv;
    struct vb2_v4l2_buffer *src_buf, *dst_buf;

    // Get input buffer (compressed data)
    src_buf = v4l2_m2m_next_src_buf(ctx->m2m_ctx);

    // Get output buffer (for decoded frame)
    dst_buf = v4l2_m2m_next_dst_buf(ctx->m2m_ctx);

    // Queue to worker thread
    queue_work(ctx->dev->decode_workqueue, &ctx->decode_work);
}

static void mtk_vdec_worker(struct work_struct *work)
{
    struct mtk_vcodec_ctx *ctx =
        container_of(work, struct mtk_vcodec_ctx, decode_work);

    // Prepare decode parameters
    mtk_vdec_set_param(ctx);

    // Start VPU (Video Processing Unit - firmware processor)
    ret = vpu_dec_start(&ctx->vpu, NULL, 0);

    // Hardware is now decoding...
    // (IRQ will fire when complete)

    // Wait for completion
    mtk_vdec_wait_for_done_ctx(ctx, MTK_INST_IRQ_RECEIVED);

    // Check result
    ret = vpu_dec_get_param(&ctx->vpu, &dec_result, ...);

    if (dec_result.is_key_frame)
        dst_buf->flags |= V4L2_BUF_FLAG_KEYFRAME;

    // Mark buffers done
    v4l2_m2m_buf_done(src_buf, VB2_BUF_STATE_DONE);
    v4l2_m2m_buf_done(dst_buf, VB2_BUF_STATE_DONE);

    // Signal DMA fence (GPU can now use buffer)
    dma_fence_signal(dst_buf->fence);

    // Job complete, run next job
    v4l2_m2m_job_finish(ctx->dev->m2m_dev_dec, ctx->m2m_ctx);
}
```

## Synchronization and Timing

### V-Sync and Frame Timing

**Goal**: Display each frame for exactly one refresh cycle (16.67ms @ 60Hz).

```
Timeline:
│
├─ t=0ms    Frame N decoded
│            └─ dma_fence_signal()
│
├─ t=2ms    DRM atomic commit
│            └─ dma_fence_wait() [waits for decoder]
│            └─ Programs plane hardware registers
│
├─ t=16.67ms  VBLANK
│              └─ Hardware switches to new framebuffer
│              └─ Frame N appears on screen
│
├─ t=33.33ms  VBLANK
│              └─ Frame N+1 appears
│
└─ t=50ms     VBLANK
               └─ Frame N+2 appears
```

### Preventing Tearing

**Problem**: If framebuffer changes mid-scan, screen shows half old frame, half new frame.

**Solution**: Atomic commit waits for vblank before applying changes.

```c
// In drm_atomic_helper.c
void drm_atomic_helper_wait_for_vblanks(struct drm_device *dev,
                                        struct drm_atomic_state *state)
{
    struct drm_crtc *crtc;
    struct drm_crtc_state *old_state;
    int i;

    for_each_old_crtc_in_state(state, crtc, old_state, i) {
        if (!crtc->state->active)
            continue;

        // Wait for vblank on this CRTC
        ret = drm_crtc_vblank_get(crtc);
        if (ret == 0) {
            drm_crtc_wait_one_vblank(crtc);
            drm_crtc_vblank_put(crtc);
        }
    }
}
```

### Frame Drops and Late Frames

**Scenario**: Decoder finishes late, misses vblank deadline.

```
Timeline:
│
├─ t=0ms     Frame N-1 displayed
│
├─ t=10ms    Frame N decode starts
│
├─ t=16.67ms VBLANK ← Should display Frame N
│            BUT: Frame N not ready yet
│            Result: Display Frame N-1 again (frame drop)
│
├─ t=20ms    Frame N decode finishes (LATE!)
│
├─ t=33.33ms VBLANK
│            Now display Frame N
│
└─ Result: Frame N-1 shown for 33ms instead of 16ms (judder)
```

**Mitigation**:
- Buffering: Queue multiple frames ahead
- Variable refresh rate (VRR/FreeSync/G-Sync): Adjust vblank timing
- Drop frames: Skip Frame N, go to Frame N+1

## Key Takeaways

### Zero-Copy Architecture

**Traditional approach** (CPU copy):
```
Decoder → RAM → CPU copy → GPU → Display
         [50ms total]
```

**Modern approach** (DMA-BUF):
```
Decoder → Shared RAM ← GPU → Display
         [5ms total, no copy!]
```

### Critical Components

1. **V4L2**: Video device API, codec framework
   - Location: `refs/linux/drivers/media/v4l2-core/`
   - Key: Memory-to-Memory framework (`v4l2-mem2mem.c`)

2. **DMA-BUF**: Zero-copy buffer sharing
   - Location: `refs/linux/drivers/dma-buf/`
   - Key: `dma_fence` synchronization primitives

3. **DRM**: Display pipeline
   - Location: `refs/linux/drivers/gpu/drm/`
   - Key: Atomic updates (`drm_atomic_helper.c`)

### Data Flow

```
Compressed bits → V4L2 decoder → DMA-BUF → DRM framebuffer → Display
                      ↑                         ↑
                   Hardware                  GPU/Display
                   decoder                   engine
```

### Why It's Fast

- **No CPU involvement**: Data flows directly from decoder to GPU
- **No memory copies**: DMA-BUF shares same physical pages
- **Hardware synchronization**: DMA fences coordinate without CPU
- **Atomic updates**: No tearing, glitches, or partial frames

## Quick Reference

### Key File Locations

| Component | Location |
|-----------|----------|
| V4L2 core | `refs/linux/drivers/media/v4l2-core/` |
| V4L2 M2M framework | `refs/linux/drivers/media/v4l2-core/v4l2-mem2mem.c:1643` |
| VideoBuf2 | `refs/linux/drivers/media/common/videobuf2/` |
| DMA-BUF core | `refs/linux/drivers/dma-buf/dma-buf.c` |
| DMA fences | `refs/linux/drivers/dma-buf/dma-fence.c` |
| DRM atomic | `refs/linux/drivers/gpu/drm/drm_atomic_helper.c:3794` |
| DRM planes | `refs/linux/drivers/gpu/drm/drm_plane.c:1795` |
| MediaTek decoder | `refs/linux/drivers/media/platform/mediatek/vcodec/decoder/` |

### Device Nodes

| Device | Purpose |
|--------|---------|
| `/dev/video0`, `/dev/video1`, ... | V4L2 video devices (decoders, encoders) |
| `/dev/dri/card0` | DRM display device |
| `/dev/dri/renderD128` | DRM render-only device (GPU) |

### Key IOCTLs

**V4L2**:
- `VIDIOC_QUERYCAP` - Query device capabilities
- `VIDIOC_S_FMT` - Set format
- `VIDIOC_REQBUFS` - Allocate buffers
- `VIDIOC_QBUF` - Queue buffer
- `VIDIOC_DQBUF` - Dequeue buffer
- `VIDIOC_EXPBUF` - Export as DMA-BUF
- `VIDIOC_STREAMON` - Start streaming

**DRM**:
- `DRM_IOCTL_MODE_GETRESOURCES` - Get display resources
- `DRM_IOCTL_PRIME_FD_TO_HANDLE` - Import DMA-BUF
- `DRM_IOCTL_MODE_ADDFB2` - Create framebuffer
- `DRM_IOCTL_MODE_ATOMIC` - Atomic display update

### Pixel Formats

| Format | Description | Use |
|--------|-------------|-----|
| `V4L2_PIX_FMT_H264` | H.264/AVC compressed | Decoder input |
| `V4L2_PIX_FMT_VP9` | VP9 compressed | Decoder input |
| `V4L2_PIX_FMT_NV12` | YUV 4:2:0, Y + UV interleaved | Decoder output |
| `DRM_FORMAT_NV12` | Same as V4L2_PIX_FMT_NV12 | DRM framebuffer |
| `DRM_FORMAT_ARGB8888` | 32-bit RGB with alpha | Desktop/UI |

### Common Resolutions and Timing

| Resolution | Refresh | Pixel Clock | Notes |
|------------|---------|-------------|-------|
| 1920×1080 | 60 Hz | 148.5 MHz | Full HD |
| 2560×1440 | 60 Hz | 241.5 MHz | QHD |
| 3840×2160 | 60 Hz | 594 MHz | 4K UHD |
| 1920×1080 | 120 Hz | 297 MHz | High refresh |

### Useful Commands

```bash
# List video devices
v4l2-ctl --list-devices

# Query device capabilities
v4l2-ctl -d /dev/video0 --all

# List DRM devices
ls /dev/dri/

# Show display configuration
modetest -M i915

# Monitor frame timing
sudo intel_gpu_top  # Intel GPU
sudo radeontop      # AMD GPU

# Check DMA-BUF usage
cat /sys/kernel/debug/dma_buf/bufinfo
```

---

**Document Status**: Complete architectural overview of Linux video playback from decoder to display.

**Last Updated**: 2025

**Kernel Version Reference**: Linux 6.x series