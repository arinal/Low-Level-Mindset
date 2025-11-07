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
#include <signal.h>
#include <xf86drm.h>
#include <xf86drmMode.h>

// Screen dimensions (we'll get actual size from connected display)
int screen_width = 1920;
int screen_height = 1080;

// Framebuffer pointer (mmap'd memory)
uint32_t *framebuffer = NULL;

// Global flag for graceful exit
volatile int keep_running = 1;

// Signal handler for Ctrl+C
void signal_handler(int signum) {
    printf("\n\nReceived signal %d (Ctrl+C), exiting gracefully...\n", signum);
    keep_running = 0;  // Tell main loop to stop
}

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

    // Register signal handlers for graceful exit
    signal(SIGINT, signal_handler);   // Ctrl+C
    signal(SIGTERM, signal_handler);  // kill command

    // =========================================================================
    // STEP 1: OPEN DRM DEVICE
    // =========================================================================
    printf("Step 1: Opening DRM device...\n");
    // SYSCALL: openat(AT_FDCWD, "/dev/dri/card1", O_RDWR)
    int drm_fd = open("/dev/dri/card1", O_RDWR);
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
    printf("Step 8: Starting render loop...\n");
    printf("Press Ctrl+C to exit gracefully...\n\n");

    // Triangle vertices (in screen space, centered)
    float cx = screen_width / 2.0f;   // Center X
    float cy = screen_height / 2.0f;  // Center Y
    float size = 200.0f;              // Triangle size

    // Original triangle vertices (relative to center)
    float v0x = 0.0f, v0y = -size;      // Top
    float v1x = -size, v1y = size;      // Bottom-left
    float v2x = size, v2y = size;       // Bottom-right

    for (int frame = 0; keep_running; frame++) {
        float angle = (frame % 360) * M_PI / 180.0f;  // Degrees to radians, wrap at 360

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
