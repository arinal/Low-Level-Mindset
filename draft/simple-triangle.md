# Simple Rotating Triangle: Direct DRM Edition

A minimal rotating triangle using **direct DRM/KMS** - no OpenGL, no shaders, just pure Linux graphics fundamentals. Shows exactly how pixels get from CPU to screen.

## Table of Contents
- [The Complete Code](#the-complete-code)
- [Compilation and Running](#compilation-and-running)
- [What Happens Internally](#what-happens-internally)
- [Syscall Trace](#syscall-trace)
- [Memory Layout](#memory-layout)
- [The Complete Flow](#the-complete-flow)
- [Comparison: Software vs GPU](#comparison-software-vs-gpu)

---

## The Complete Code

```c
// simple_triangle.c
// Rotating triangle using direct DRM/KMS
// CPU renders triangle, DRM displays it - no GPU rendering, no shaders!

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/mman.h>
#include <xf86drm.h>
#include <xf86drmMode.h>

// Screen dimensions (we'll get actual size from connected display)
int screen_width = 1920;
int screen_height = 1080;

// Framebuffer pointer (mmap'd memory)
uint32_t *framebuffer = NULL;

// =============================================================================
// SOFTWARE TRIANGLE RASTERIZATION (CPU does the work!)
// =============================================================================

// Draw a single pixel
void put_pixel(int x, int y, uint32_t color) {
    if (x >= 0 && x < screen_width && y >= 0 && y < screen_height) {
        // Direct memory write to framebuffer
        // Format: 0xAARRGGBB (32-bit XRGB8888)
        framebuffer[y * screen_width + x] = color;
    }
}

// Draw a line using Bresenham's algorithm
void draw_line(int x0, int y0, int x1, int y1, uint32_t color) {
    int dx = abs(x1 - x0);
    int dy = abs(y1 - y0);
    int sx = x0 < x1 ? 1 : -1;
    int sy = y0 < y1 ? 1 : -1;
    int err = dx - dy;

    while (1) {
        put_pixel(x0, y0, color);

        if (x0 == x1 && y0 == y1) break;

        int e2 = 2 * err;
        if (e2 > -dy) {
            err -= dy;
            x0 += sx;
        }
        if (e2 < dx) {
            err += dx;
            y0 += sy;
        }
    }
}

// Fill the screen with a color
void clear_screen(uint32_t color) {
    // This is a simple memset-like operation
    // Writing directly to framebuffer memory (mmap'd)
    for (int i = 0; i < screen_width * screen_height; i++) {
        framebuffer[i] = color;
    }
}

// Draw a filled triangle (simple scanline algorithm)
void draw_triangle(int x0, int y0, int x1, int y1, int x2, int y2, uint32_t color) {
    // For simplicity, just draw the outline
    // (Full triangle filling would make code longer)
    draw_line(x0, y0, x1, y1, color);
    draw_line(x1, y1, x2, y2, color);
    draw_line(x2, y2, x0, y0, color);
}

// Rotate a point around origin
void rotate_point(float *x, float *y, float angle) {
    float cos_a = cosf(angle);
    float sin_a = sinf(angle);
    float new_x = *x * cos_a - *y * sin_a;
    float new_y = *x * sin_a + *y * cos_a;
    *x = new_x;
    *y = new_y;
}

// =============================================================================
// MAIN PROGRAM
// =============================================================================

int main() {
    int ret;

    printf("=== Simple Rotating Triangle (Direct DRM) ===\n\n");

    // =========================================================================
    // STEP 1: OPEN DRM DEVICE
    // =========================================================================
    printf("Step 1: Opening DRM device...\n");
    // SYSCALL: openat(AT_FDCWD, "/dev/dri/card0", O_RDWR)
    int drm_fd = open("/dev/dri/card0", O_RDWR);
    if (drm_fd < 0) {
        perror("Cannot open /dev/dri/card0");
        printf("Hint: You may need to run as root or be in 'video' group\n");
        return 1;
    }
    printf("✓ DRM device opened (fd=%d)\n\n", drm_fd);

    // =========================================================================
    // STEP 2: GET DISPLAY RESOURCES
    // =========================================================================
    printf("Step 2: Querying display resources...\n");
    // SYSCALL: ioctl(drm_fd, DRM_IOCTL_MODE_GETRESOURCES, ...)
    // This returns: connectors, CRTCs, encoders available
    drmModeRes *resources = drmModeGetResources(drm_fd);
    if (!resources) {
        perror("Cannot get DRM resources");
        close(drm_fd);
        return 1;
    }
    printf("✓ Found %d connectors, %d CRTCs, %d encoders\n\n",
           resources->count_connectors,
           resources->count_crtcs,
           resources->count_encoders);

    // =========================================================================
    // STEP 3: FIND CONNECTED DISPLAY
    // =========================================================================
    printf("Step 3: Finding connected display...\n");
    drmModeConnector *connector = NULL;
    for (int i = 0; i < resources->count_connectors; i++) {
        // SYSCALL: ioctl(drm_fd, DRM_IOCTL_MODE_GETCONNECTOR, ...)
        // Queries each connector (HDMI, DisplayPort, etc.)
        connector = drmModeGetConnector(drm_fd, resources->connectors[i]);
        if (connector->connection == DRM_MODE_CONNECTED) {
            printf("✓ Found connected display: connector %d\n", connector->connector_id);
            break;
        }
        drmModeFreeConnector(connector);
        connector = NULL;
    }

    if (!connector) {
        fprintf(stderr, "No connected display found\n");
        drmModeFreeResources(resources);
        close(drm_fd);
        return 1;
    }

    // Get display resolution
    drmModeModeInfo mode = connector->modes[0];  // First (preferred) mode
    screen_width = mode.hdisplay;
    screen_height = mode.vdisplay;
    printf("✓ Display resolution: %dx%d @ %dHz\n\n",
           screen_width, screen_height, mode.vrefresh);

    // =========================================================================
    // STEP 4: CREATE FRAMEBUFFER
    // =========================================================================
    printf("Step 4: Creating framebuffer...\n");

    // Create a "dumb buffer" (simple buffer in system RAM or VRAM)
    struct drm_mode_create_dumb create_dumb = {
        .width = screen_width,
        .height = screen_height,
        .bpp = 32,  // 32 bits per pixel (XRGB8888)
    };

    // SYSCALL: ioctl(drm_fd, DRM_IOCTL_MODE_CREATE_DUMB, &create_dumb)
    // This allocates memory for the framebuffer
    // On integrated GPU: allocates in system RAM
    // On discrete GPU: might allocate in VRAM or system RAM
    ret = drmIoctl(drm_fd, DRM_IOCTL_MODE_CREATE_DUMB, &create_dumb);
    if (ret < 0) {
        perror("Cannot create dumb buffer");
        return 1;
    }

    uint32_t fb_handle = create_dumb.handle;
    uint32_t fb_pitch = create_dumb.pitch;  // Bytes per row
    uint64_t fb_size = create_dumb.size;    // Total size in bytes

    printf("✓ Dumb buffer created:\n");
    printf("  - Handle: %u\n", fb_handle);
    printf("  - Size: %lu bytes (%.2f MB)\n", fb_size, fb_size / 1024.0 / 1024.0);
    printf("  - Pitch: %u bytes/row\n\n", fb_pitch);

    // =========================================================================
    // STEP 5: ADD FRAMEBUFFER TO DRM
    // =========================================================================
    printf("Step 5: Registering framebuffer with DRM...\n");

    uint32_t fb_id;
    // SYSCALL: ioctl(drm_fd, DRM_IOCTL_MODE_ADDFB, ...)
    // Tells DRM: "This buffer can be used as a framebuffer for display"
    ret = drmModeAddFB(drm_fd, screen_width, screen_height, 24, 32,
                       fb_pitch, fb_handle, &fb_id);
    if (ret) {
        perror("Cannot add framebuffer");
        return 1;
    }
    printf("✓ Framebuffer registered (fb_id=%u)\n\n", fb_id);

    // =========================================================================
    // STEP 6: MAP FRAMEBUFFER TO USER SPACE
    // =========================================================================
    printf("Step 6: Mapping framebuffer to userspace...\n");

    // First, get the mmap offset
    struct drm_mode_map_dumb map_dumb = {
        .handle = fb_handle,
    };

    // SYSCALL: ioctl(drm_fd, DRM_IOCTL_MODE_MAP_DUMB, &map_dumb)
    // Returns: offset we can use with mmap()
    ret = drmIoctl(drm_fd, DRM_IOCTL_MODE_MAP_DUMB, &map_dumb);
    if (ret) {
        perror("Cannot get mmap offset");
        return 1;
    }

    // Now map it to our process address space
    // SYSCALL: mmap(NULL, fb_size, PROT_READ|PROT_WRITE, MAP_SHARED, drm_fd, map_dumb.offset)
    // This gives us direct memory access to the framebuffer!
    framebuffer = mmap(NULL, fb_size, PROT_READ | PROT_WRITE, MAP_SHARED,
                       drm_fd, map_dumb.offset);
    if (framebuffer == MAP_FAILED) {
        perror("Cannot mmap framebuffer");
        return 1;
    }

    printf("✓ Framebuffer mapped to address: %p\n", framebuffer);
    printf("  - Now CPU can write directly to display memory!\n\n");

    // =========================================================================
    // STEP 7: SET DISPLAY MODE (MODESETTING)
    // =========================================================================
    printf("Step 7: Setting display mode...\n");

    // Find a CRTC (display controller) to use
    uint32_t crtc_id = resources->crtcs[0];  // Use first CRTC

    // SYSCALL: ioctl(drm_fd, DRM_IOCTL_MODE_SETCRTC, ...)
    // This configures the display controller:
    // - Which connector to use (HDMI, DP, etc.)
    // - What resolution/refresh rate
    // - Which framebuffer to scan out
    ret = drmModeSetCrtc(drm_fd, crtc_id, fb_id, 0, 0,
                         &connector->connector_id, 1, &mode);
    if (ret) {
        perror("Cannot set CRTC");
        return 1;
    }

    printf("✓ Display mode set!\n");
    printf("  - Display controller is now scanning out our framebuffer\n");
    printf("  - Whatever we write to framebuffer appears on screen!\n\n");

    // =========================================================================
    // STEP 8: RENDER LOOP
    // =========================================================================
    printf("Step 8: Starting render loop...\n\n");

    // Triangle vertices (in screen space, centered)
    float cx = screen_width / 2.0f;   // Center X
    float cy = screen_height / 2.0f;  // Center Y
    float size = 200.0f;              // Triangle size

    // Original triangle vertices (relative to center)
    float v0x = 0.0f, v0y = -size;      // Top
    float v1x = -size, v1y = size;      // Bottom-left
    float v2x = size, v2y = size;       // Bottom-right

    for (int frame = 0; frame < 360; frame++) {
        float angle = frame * M_PI / 180.0f;  // Degrees to radians

        // =====================================================================
        // CPU RENDERING (Software rasterization)
        // =====================================================================

        // Clear screen (write black pixels to entire framebuffer)
        // This is a simple memory write operation - NO GPU involved!
        clear_screen(0x00000000);  // Black

        // Rotate triangle vertices
        float r0x = v0x, r0y = v0y;
        float r1x = v1x, r1y = v1y;
        float r2x = v2x, r2y = v2y;

        rotate_point(&r0x, &r0y, angle);
        rotate_point(&r1x, &r1y, angle);
        rotate_point(&r2x, &r2y, angle);

        // Convert to screen coordinates
        int x0 = (int)(cx + r0x);
        int y0 = (int)(cy + r0y);
        int x1 = (int)(cx + r1x);
        int y1 = (int)(cy + r1y);
        int x2 = (int)(cx + r2x);
        int y2 = (int)(cy + r2y);

        // Draw triangle (CPU draws lines pixel by pixel)
        // Colors: Red, Green, Blue for each edge
        draw_line(x0, y0, x1, y1, 0x00FF0000);  // Red
        draw_line(x1, y1, x2, y2, 0x0000FF00);  // Green
        draw_line(x2, y2, x0, y0, 0x000000FF);  // Blue

        // =====================================================================
        // DISPLAY UPDATE
        // =====================================================================

        // The framebuffer is already mapped to display memory!
        // The display controller continuously reads from it
        // So the triangle appears IMMEDIATELY - no page flip needed

        // Optional: We could use page flipping for smoother animation:
        // drmModePageFlip(drm_fd, crtc_id, fb_id, DRM_MODE_PAGE_FLIP_EVENT, NULL);

        if (frame % 60 == 0) {
            printf("Frame %d rendered (angle=%.1f°)\n", frame, frame * 1.0f);
        }

        usleep(16666);  // ~60 FPS
    }

    printf("\n=== Cleanup ===\n");

    // =========================================================================
    // CLEANUP
    // =========================================================================

    // Unmap framebuffer
    munmap(framebuffer, fb_size);

    // Destroy framebuffer
    drmModeRmFB(drm_fd, fb_id);

    // Free resources
    drmModeFreeConnector(connector);
    drmModeFreeResources(resources);

    // Close DRM device
    close(drm_fd);

    printf("✓ All resources cleaned up\n");

    return 0;
}
```

---

## Compilation and Running

```bash
# Install dependencies
sudo apt install libdrm-dev

# Compile
gcc simple_triangle.c -o simple_triangle -ldrm -lm

# Run (needs DRM access)
sudo ./simple_triangle

# OR: Add yourself to video group to avoid sudo
sudo usermod -a -G video $USER
# Log out and back in, then:
./simple_triangle
```

---

## What Happens Internally

### Phase 1: Setup (Multiple Syscalls)

```
open("/dev/dri/card0")
    ↓
ioctl(DRM_IOCTL_MODE_GETRESOURCES)      # Query display hardware
    ↓
ioctl(DRM_IOCTL_MODE_GETCONNECTOR)      # Check if monitor connected
    ↓
ioctl(DRM_IOCTL_MODE_CREATE_DUMB)       # Allocate framebuffer memory
    ↓
ioctl(DRM_IOCTL_MODE_ADDFB)             # Register framebuffer with DRM
    ↓
ioctl(DRM_IOCTL_MODE_MAP_DUMB)          # Get mmap offset
    ↓
mmap(drm_fd, offset, size)              # Map framebuffer to userspace
    ↓
ioctl(DRM_IOCTL_MODE_SETCRTC)           # Configure display controller
```

**Total: ~8-10 syscalls**

### Phase 2: Rendering (NO Syscalls!)

```
For each frame:
    clear_screen():
        for (i = 0; i < width*height; i++)
            framebuffer[i] = 0x000000;   // Direct memory write!
                                          // No syscall!

    draw_line():
        for each pixel in line:
            framebuffer[y*width + x] = color;  // Direct memory write!
                                                // No syscall!

    (Display controller reads from framebuffer automatically)
```

**Syscalls per frame: 0**

---

## Syscall Trace

```bash
# Trace the program
strace -e openat,ioctl,mmap,munmap ./simple_triangle 2>&1 | grep -E "open|ioctl|mmap"

# Example output:
openat(AT_FDCWD, "/dev/dri/card0", O_RDWR) = 3
ioctl(3, DRM_IOCTL_MODE_GETRESOURCES, ...) = 0
ioctl(3, DRM_IOCTL_MODE_GETCONNECTOR, ...) = 0
ioctl(3, DRM_IOCTL_MODE_CREATE_DUMB, ...) = 0
ioctl(3, DRM_IOCTL_MODE_ADDFB, ...) = 0
ioctl(3, DRM_IOCTL_MODE_MAP_DUMB, ...) = 0
mmap(NULL, 8294400, PROT_READ|PROT_WRITE, MAP_SHARED, 3, 0) = 0x7f8b2c000000
ioctl(3, DRM_IOCTL_MODE_SETCRTC, ...) = 0
# ... (rendering happens with NO syscalls) ...
munmap(0x7f8b2c000000, 8294400) = 0
```

### Breakdown

| Phase | Syscalls | What Happens |
|-------|----------|--------------|
| **Initialization** | 8-10 | Open device, query hardware, allocate/map framebuffer, configure display |
| **Rendering (360 frames)** | 0 | CPU writes to mapped memory - display controller reads automatically |
| **Cleanup** | 3-5 | Unmap, free resources, close device |

**Key insight**: After setup, rendering needs **ZERO syscalls** because the framebuffer is memory-mapped!

---

## Memory Layout

```
Physical Memory:
┌─────────────────────────────────────────────────────────────┐
│ System RAM (or VRAM on discrete GPU)                       │
│                                                             │
│ 0xC0000000 - 0xC07E9000: Framebuffer (8,294,400 bytes)     │
│   ↑                                                         │
│   │ This is the actual framebuffer!                        │
│   │ 1920 × 1080 × 4 bytes = 8,294,400 bytes               │
│   │                                                         │
│   │ Memory layout:                                          │
│   │   Offset 0:     Pixel (0,0) = 0xAARRGGBB              │
│   │   Offset 4:     Pixel (1,0) = 0xAARRGGBB              │
│   │   Offset 8:     Pixel (2,0) = 0xAARRGGBB              │
│   │   ...                                                  │
│   │   Offset 7680:  Pixel (0,1) = 0xAARRGGBB              │
│   │   ...                                                  │
└─────────────────────────────────────────────────────────────┘
         ↕ (mmap creates mapping)
Process Virtual Memory:
┌─────────────────────────────────────────────────────────────┐
│ 0x7f8b2c000000: Framebuffer (mapped)                       │
│   ↑                                                         │
│   │ CPU can write here directly!                           │
│   │                                                         │
│   │ framebuffer[0] = 0x00FF0000;  ← Writes to physical RAM │
│   │ framebuffer[1] = 0x0000FF00;  ← No syscall needed!    │
└─────────────────────────────────────────────────────────────┘
```

### What Happens During Rendering

```c
// When you do this:
framebuffer[y * width + x] = 0x00FF0000;  // Write red pixel

// It translates to:
1. CPU calculates address: base + (y * width + x) * 4
2. CPU writes 0x00FF0000 to that address
3. MMU translates virtual address → physical address
4. Memory controller writes to RAM/VRAM
5. Display controller reads that memory location
6. Pixel appears on screen!

// NO KERNEL INVOLVEMENT!
```

---

## The Complete Flow

### Detailed Step-by-Step

```
=== INITIALIZATION ===

1. APPLICATION: open("/dev/dri/card0")
   ↓ SYSCALL: openat()
   ↓ KERNEL: VFS routes to DRM character device
   ↓ KERNEL: DRM opens i915 driver
   ✓ Returns: fd=3

2. APPLICATION: drmModeGetResources(fd)
   ↓ SYSCALL: ioctl(fd, DRM_IOCTL_MODE_GETRESOURCES)
   ↓ KERNEL: drm_mode_getresources() in drivers/gpu/drm/drm_mode_config.c
   ↓ KERNEL: Queries hardware, returns list of CRTCs, connectors, encoders
   ✓ Returns: {crtcs=[1], connectors=[47,48], encoders=[...]}

3. APPLICATION: drmModeGetConnector(fd, connector_id)
   ↓ SYSCALL: ioctl(fd, DRM_IOCTL_MODE_GETCONNECTOR)
   ↓ KERNEL: drm_mode_getconnector() in drivers/gpu/drm/drm_mode_config.c
   ↓ KERNEL: Reads EDID from monitor (via i2c)
   ↓ KERNEL: Returns supported modes (resolutions/refresh rates)
   ✓ Returns: {connection=CONNECTED, modes=[1920x1080@60, ...]}

4. APPLICATION: drmIoctl(fd, DRM_IOCTL_MODE_CREATE_DUMB)
   ↓ SYSCALL: ioctl(fd, DRM_IOCTL_MODE_CREATE_DUMB, {width=1920, height=1080, bpp=32})
   ↓ KERNEL: drm_mode_create_dumb() in drivers/gpu/drm/drm_dumb_buffers.c
   ↓ KERNEL: Calls driver-specific allocation (i915_gem_dumb_create)
   ↓ KERNEL (i915): i915_gem_create() in drivers/gpu/drm/i915/gem/i915_gem_create.c
   ↓ KERNEL (i915): Allocates 8,294,400 bytes in system RAM (or VRAM)
   ✓ Returns: {handle=1, pitch=7680, size=8294400}

5. APPLICATION: drmModeAddFB(fd, width, height, handle)
   ↓ SYSCALL: ioctl(fd, DRM_IOCTL_MODE_ADDFB)
   ↓ KERNEL: drm_mode_addfb() in drivers/gpu/drm/drm_framebuffer.c
   ↓ KERNEL: Creates framebuffer object, associates with buffer handle
   ✓ Returns: fb_id=42

6. APPLICATION: drmIoctl(fd, DRM_IOCTL_MODE_MAP_DUMB, {handle=1})
   ↓ SYSCALL: ioctl(fd, DRM_IOCTL_MODE_MAP_DUMB)
   ↓ KERNEL: drm_mode_mmap_dumb() in drivers/gpu/drm/drm_dumb_buffers.c
   ↓ KERNEL: Creates fake offset for mmap (doesn't actually map yet)
   ✓ Returns: offset=0x100000000

7. APPLICATION: mmap(NULL, size, PROT_READ|PROT_WRITE, MAP_SHARED, fd, offset)
   ↓ SYSCALL: mmap()
   ↓ KERNEL: VFS calls DRM's mmap handler
   ↓ KERNEL: drm_gem_mmap() in drivers/gpu/drm/drm_gem.c
   ↓ KERNEL: Maps framebuffer memory into process address space
   ↓ KERNEL: Page table entries created pointing to physical RAM/VRAM
   ✓ Returns: virtual address = 0x7f8b2c000000

8. APPLICATION: drmModeSetCrtc(fd, crtc_id, fb_id, ...)
   ↓ SYSCALL: ioctl(fd, DRM_IOCTL_MODE_SETCRTC)
   ↓ KERNEL: drm_mode_setcrtc() in drivers/gpu/drm/drm_crtc.c
   ↓ KERNEL: Calls driver-specific function (intel_crtc_set_config)
   ↓ KERNEL (i915): intel_crtc_set_config() in drivers/gpu/drm/i915/display/intel_display.c
   ↓ KERNEL (i915): Programs display controller registers:
   │    MMIO_WRITE(DSPSURF, framebuffer_physical_address)
   │    MMIO_WRITE(DSPSTRIDE, 7680)  // bytes per row
   │    MMIO_WRITE(PIPESRC, (1920 << 16) | 1080)  // resolution
   ↓ HARDWARE: Display controller starts scanning framebuffer
   ✓ Display shows whatever is in framebuffer!

=== RENDERING LOOP ===

9. APPLICATION: clear_screen(0x00000000)
   ├─ for (i = 0; i < 2073600; i++)
   │      framebuffer[i] = 0x00000000;
   │
   ├─ CPU writes directly to mapped memory (0x7f8b2c000000)
   ├─ MMU translates to physical address (0xC0000000)
   ├─ Memory write goes to system RAM (or VRAM)
   │
   └─ NO SYSCALL! Pure memory write!

10. APPLICATION: draw_line(x0, y0, x1, y1, color)
    ├─ Bresenham's algorithm calculates pixel positions
    ├─ for each pixel:
    │      framebuffer[y * width + x] = color;
    │
    ├─ Direct memory writes (same as above)
    │
    └─ NO SYSCALL!

11. HARDWARE: Display controller (running in parallel)
    ├─ Every 16.67ms (@ 60Hz):
    │   ├─ Read framebuffer from address in DSPSURF register
    │   ├─ Send pixels to HDMI/DP encoder
    │   └─ Encoder sends to monitor
    │
    └─ CPU and display controller work independently!
        CPU writes → RAM → Display controller reads → Monitor

=== CLEANUP ===

12. APPLICATION: munmap(framebuffer, size)
    ↓ SYSCALL: munmap()
    ↓ KERNEL: Removes page table mappings
    ✓ Memory no longer accessible

13. APPLICATION: drmModeRmFB(fd, fb_id)
    ↓ SYSCALL: ioctl(fd, DRM_IOCTL_MODE_RMFB)
    ↓ KERNEL: Removes framebuffer object
    ✓ Done

14. APPLICATION: close(fd)
    ↓ SYSCALL: close()
    ↓ KERNEL: Releases DRM resources
    ✓ Device closed
```

---

## Comparison: Software vs GPU

### This Program (CPU Software Rendering)

```
CPU renders:
    ├─ clear_screen(): Write 2,073,600 pixels (~8 MB)
    │    Speed: ~50 GB/s (DDR4 bandwidth)
    │    Time: ~0.16 ms
    │
    ├─ draw_line() × 3: Write ~1000 pixels each
    │    Speed: ~100 MB/s (algorithm overhead)
    │    Time: ~0.03 ms
    │
    └─ Total CPU time: ~0.2 ms per frame

Display controller reads:
    ├─ Scans framebuffer at 60Hz
    ├─ Reads ~8 MB per frame
    └─ Sends to monitor

Pros:
    ✓ Simple - no shaders, no GPU complexity
    ✓ Zero syscalls during rendering
    ✓ Direct memory access

Cons:
    ✗ CPU does all the work (slow for complex scenes)
    ✗ Can't do 3D transformations efficiently
    ✗ Limited to simple 2D graphics
```

### GPU Rendering (from rotating-triangle.md)

```
GPU renders:
    ├─ Vertex shader runs 3 times (GPU parallel units)
    ├─ Rasterizer generates ~100,000 fragments
    ├─ Fragment shader runs 100,000 times (massively parallel)
    │    Speed: ~500 GB/s VRAM bandwidth
    │    Time: ~0.5 ms (GPU has thousands of cores)
    │
    └─ Total GPU time: ~1 ms per frame

CPU during rendering:
    ├─ Writes ~256 bytes (command buffer)
    ├─ One syscall (ioctl) to submit
    └─ CPU is free to do other work!

Pros:
    ✓ Massively parallel (thousands of GPU cores)
    ✓ Fast for complex 3D scenes
    ✓ CPU freed for other tasks

Cons:
    ✗ Complex (shaders, OpenGL/Vulkan API)
    ✗ Requires GPU drivers
    ✗ More syscalls during setup
```

---

## Key Takeaways

1. **DRM KMS is the fundamental Linux display API**
   - Modern, replaces legacy framebuffer (`/dev/fb0`)
   - Works on all GPUs (Intel, AMD, NVIDIA)
   - Used by Wayland, X11, and direct applications

2. **mmap() enables zero-copy rendering**
   - Framebuffer mapped directly to process memory
   - CPU writes appear on screen immediately
   - No syscalls needed during rendering!

3. **Display controller works independently**
   - Continuously scans framebuffer memory
   - Sends pixels to monitor at refresh rate
   - CPU can write while display reads (tearing possible)

4. **Syscalls only needed for setup**
   - ~10 syscalls to initialize
   - 0 syscalls per frame during rendering
   - mmap makes it possible!

5. **Software rendering is simple but limited**
   - Good for learning fundamentals
   - Good for simple 2D graphics
   - Not suitable for complex 3D (too slow)

6. **This is what SDL/GTK use internally**
   - High-level libraries wrap these low-level calls
   - Same DRM/KMS underneath
   - Same mmap-based approach for software rendering

**The magic**: After mmap(), framebuffer is just normal memory. Write to it, pixels appear on screen. No GPU, no shaders, no complexity - just pure fundamentals!