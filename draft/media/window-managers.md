# Linux Window Managers: X11 vs Wayland Architecture

This document explains how window managers work on Linux, comparing the classic X11 architecture with the modern Wayland approach. We'll trace the complete path from application code through the display server/compositor down to the kernel's DRM subsystem.

## Table of Contents

- [High-Level Overview](#high-level-overview)
- [The Problem: Multiple Apps, One Screen](#the-problem-multiple-apps-one-screen)
- [X11 Architecture](#x11-architecture)
  - [X11 Component Roles](#x11-component-roles)
  - [X11 Client-Server Interaction](#x11-client-server-interaction)
  - [X11 Code Example](#x11-code-example)
  - [X11 Protocol Flow](#x11-protocol-flow)
  - [X11 Problems](#x11-problems)
- [Wayland Architecture](#wayland-architecture)
  - [Wayland Component Roles](#wayland-component-roles)
  - [Wayland Client-Compositor Interaction](#wayland-client-compositor-interaction)
  - [Wayland Code Example](#wayland-code-example)
  - [Wayland Protocol Flow](#wayland-protocol-flow)
  - [Wayland Advantages](#wayland-advantages)
- [Key Data Structures](#key-data-structures)
- [Kernel Integration: DRM and Input Subsystems](#kernel-integration-drm-and-input-subsystems)
- [Complete Rendering Paths](#complete-rendering-paths)
- [Comparison Table](#comparison-table)
- [Visual Architecture Diagrams](#visual-architecture-diagrams)
- [Key Takeaways](#key-takeaways)
- [Quick Reference](#quick-reference)

---

## High-Level Overview

When you run multiple applications on Linux, something needs to coordinate:
- **Window placement** - Where does each window appear?
- **Input routing** - Which app receives keyboard/mouse events?
- **Compositing** - How do we combine all windows into one final image?
- **Decorations** - Who draws title bars, borders, close buttons?

Two approaches evolved:
1. **X11 (X Window System)**: Separate X Server and Window Manager, network-transparent protocol
2. **Wayland**: Unified compositor, simpler protocol, client-side rendering

Both ultimately use the same kernel subsystems (DRM/KMS for display, input subsystem for devices), but their architectures differ dramatically.

---

## The Problem: Multiple Apps, One Screen

### What Needs to Happen

```
┌─────────────┐  ┌─────────────┐  ┌─────────────┐
│   Firefox   │  │  Terminal   │  │    Editor   │
│  (renders)  │  │  (renders)  │  │  (renders)  │
└──────┬──────┘  └──────┬──────┘  └──────┬──────┘
       │                │                │
       └────────────────┼────────────────┘
                        │
                   SOMETHING needs to:
                   - Arrange windows
                   - Composite them
                   - Route input events
                   - Manage the display
                        │
                        ↓
                ┌───────────────┐
                │  Display      │
                │  Hardware     │
                └───────────────┘
```

**Two philosophies**:
- **X11**: Central server does most work, apps send drawing commands
- **Wayland**: Apps render themselves, compositor just arranges buffers

---

## X11 Architecture

### X11 Component Roles

```
┌────────────────────────────────────────────────────┐
│              Applications (X Clients)              │
│         Firefox, Terminal, Editor, etc.            │
│                                                    │
│  - Send drawing commands (draw rectangle, text)    │
│  - Receive events (mouse clicks, key presses)      │
│  - Use Xlib or XCB library                         │
└────────────────┬───────────────────────────────────┘
                 │ X11 Protocol over socket
                 │ /tmp/.X11-unix/X0
                 ↓
┌────────────────────────────────────────────────────┐
│               X Server (Xorg)                      │
│                                                    │
│  - Manages display hardware via DRM                │
│  - Maintains window hierarchy                      │
│  - Executes drawing commands from clients          │
│  - Routes input events to appropriate windows      │
│  - Manages resources (pixmaps, fonts, cursors)     │
└────────────────┬───────────────────────────────────┘
                 │ X11 Protocol (same as clients!)
                 │ Window Manager is a special client
                 ↓
┌────────────────────────────────────────────────────┐
│        Window Manager (i3, dwm, Openbox)           │
│                                                    │
│  - Intercepts window creation (MapRequest)         │
│  - Decides actual window position/size             │
│  - Draws decorations by sending commands to X      │
│    Server (title bars, borders - X Server          │
│    actually rasterizes the pixels)                 │
│  - Handles tiling, floating, workspaces            │
│  - Just another X client with special privileges!  │
└────────────────────────────────────────────────────┘

         (Only X Server talks to kernel, not WM!)
                 ↓
         ┌──────────────┐
         │   Kernel     │
         │  DRM/KMS     │
         │  Input Core  │
         └──────────────┘
```

**Key insight**: The Window Manager is just another X client! It talks to the X Server using the same X11 protocol as regular applications. The X Server gives it special privileges (like intercepting MapRequest events).

### X11 Client-Server Interaction

#### Connection Establishment

```c
// Application opens connection to X Server
// This creates a socket connection to /tmp/.X11-unix/X0
Display *display = XOpenDisplay(NULL);

// Under the hood:
// 1. Parse DISPLAY environment variable (e.g., ":0")
// 2. Connect to Unix socket /tmp/.X11-unix/X0
// 3. Send authentication (MIT-MAGIC-COOKIE-1)
// 4. Receive server capabilities
```

**Socket path**: `/tmp/.X11-unix/X0` for display `:0`

#### Window Creation Flow

```
App (X Client)            X Server              Window Manager
     |                       |                         |
     |--XCreateWindow------->|                         |
     |  (position, size)     |                         |
     |<---window ID----------|                         |
     |                       |                         |
     |--XMapWindow---------->|                         |
     |  (show window)        |                         |
     |                       |--MapRequest------------>|
     |                       |  "App wants to show     |
     |                       |   window at 100,100"    |
     |                       |                         |
     |                       |<--ConfigureWindow-------|
     |                       |  "No, put it at 50,50   |
     |                       |   and make it 1000x800" |
     |                       |                         |
     |                       | Apply configuration     |
     |                       |                         |
     |<--ConfigureNotify-----|                         |
     |  "Your window is      |                         |
     |   now at 50,50"       |                         |
     |                       |                         |
     |<--Expose--------------|                         |
     |  "Please redraw"      |                         |
     |                       |                         |
     |--XDrawRectangle------>|                         |
     |--XDrawString--------->|                         |
     |  (X Server executes   |                         |
     |   these commands)     |                         |
```

**Key points**:
1. App requests window creation
2. Window Manager intercepts and decides actual placement
3. App receives Expose event and must redraw
4. **X Server executes drawing commands** - app doesn't render directly

### X11 Code Example

#### Simple X11 Application

```c
#include <X11/Xlib.h>
#include <stdio.h>
#include <stdlib.h>

int main() {
    // STEP 1: Connect to X Server
    // Opens Unix socket: /tmp/.X11-unix/X0
    Display *display = XOpenDisplay(NULL);
    if (!display) {
        fprintf(stderr, "Cannot open display\n");
        return 1;
    }

    int screen = DefaultScreen(display);
    Window root = RootWindow(display, screen);

    // STEP 2: Create window
    // This just creates the window data structure in X Server
    Window window = XCreateSimpleWindow(
        display,
        root,           // Parent window
        100, 100,       // x, y (Window Manager will override this!)
        800, 600,       // width, height
        1,              // border width
        BlackPixel(display, screen),  // border color
        WhitePixel(display, screen)   // background color
    );

    // STEP 3: Request events we want to receive
    XSelectInput(display, window,
                 ExposureMask |       // Window needs redrawing
                 KeyPressMask |       // Keyboard input
                 ButtonPressMask);    // Mouse clicks

    // STEP 4: Map (show) the window
    // X Server will send MapRequest to Window Manager
    XMapWindow(display, window);

    // STEP 5: Create Graphics Context for drawing
    GC gc = XCreateGC(display, window, 0, NULL);
    XSetForeground(display, gc, BlackPixel(display, screen));

    // STEP 6: Event loop
    XEvent event;
    while (1) {
        // BLOCKING call - waits for X Server to send event
        XNextEvent(display, &event);

        switch (event.type) {
            case Expose:
                // X Server says: "Your window is visible, please redraw"
                // NOTE: App must track what to draw - X Server doesn't remember!

                // Send drawing commands to X Server
                XDrawString(display, window, gc,
                           50, 50,                    // x, y
                           "Hello X11!", 11);         // text, length

                XDrawRectangle(display, window, gc,
                              100, 100,               // x, y
                              200, 150);              // width, height

                // X Server executes these commands and draws to window
                break;

            case KeyPress:
                // User pressed a key in our window
                printf("Key pressed: keycode %d\n", event.xkey.keycode);
                break;

            case ButtonPress:
                // Mouse clicked in our window
                printf("Mouse click at %d,%d\n",
                       event.xbutton.x, event.xbutton.y);
                break;
        }
    }

    // Cleanup
    XDestroyWindow(display, window);
    XCloseDisplay(display);
    return 0;
}
```

**Compile**: `gcc -o x11_example x11_example.c -lX11`

### X11 Protocol Flow

#### Drawing Commands

```
App sends:                X Server receives:           X Server does:
---------                 ------------------           --------------
XDrawRectangle()    →     opcode: PolyRectangle   →   Execute:
  x=100, y=100            window_id: 0x1234567        - Clip to window
  w=200, h=150            gc_id: 0x8901234            - Apply GC settings
                          rectangles: [(100,100,       - Rasterize
                                        200,150)]      - Write to framebuffer
```

**Problem**: Network latency! Each drawing command is a round-trip.

**Modern solution**: Extensions like MIT-SHM (shared memory) and DRI (Direct Rendering Infrastructure) bypass X Server for performance.

### X11 Problems

1. **Security**: Any X client can:
   - Read window contents of other apps (`XGetImage`)
   - Capture keyboard input globally (keyloggers!)
   - Take screenshots of entire screen
   - No isolation between applications

2. **Complexity**:
   - 500+ protocol messages
   - 30+ years of accumulated extensions
   - Complex drawing model with multiple coordinate spaces

3. **Compositing**:
   - Originally no compositing support
   - Composite extension added later (circa 2004)
   - Window managers had to be retrofitted

4. **Performance**:
   - Drawing commands go over socket
   - X Server must execute all drawing
   - Modern apps need direct GPU access (games, video)

5. **Tearing**:
   - Apps using DRI bypass X Server's synchronization
   - Screen tearing during vsync

6. **Network transparency rarely used**:
   - Modern apps need GPU acceleration
   - High bandwidth required
   - Latency issues

---

## Wayland Architecture

### Wayland Component Roles

```
┌───────────────────────────────────────────────────┐
│         Applications (Wayland Clients)            │
│         Firefox, Terminal, Editor, etc.           │
│                                                   │
│  - Render to their own buffers (client-side!)     │
│  - Use EGL/OpenGL/Vulkan for GPU acceleration     │
│  - Pass buffer handles to compositor              │
│  - Use libwayland-client library                  │
└────────────────┬──────────────────────────────────┘
                 │ Wayland Protocol over socket
                 │ /run/user/1000/wayland-0
                 │ Much simpler protocol (~50 messages)
                 ↓
┌───────────────────────────────────────────────────┐
│           Wayland Compositor                      │
│     (Sway, GNOME Shell, KWin, wlroots-based)      │
│                                                   │
│  COMBINES THREE ROLES:                            │
│  1. Display Server - manages display hardware     │
│  2. Window Manager - arranges windows             │
│  3. Compositor - combines buffers                 │
│                                                   │
│  - Receives buffer handles from clients           │
│  - Composites using GPU                           │
│  - Manages input devices                          │
│  - Talks directly to DRM/KMS                      │
│  - No drawing commands - just buffer handles!     │
└────────────────┬──────────────────────────────────┘
                 │ Direct Rendering Manager (DRM)
                 │ ioctl() calls
                 ↓
         ┌──────────────┐
         │   Kernel     │
         │  DRM/KMS     │
         │  Input Core  │
         └──────────────┘
                 ↓
         ┌──────────────┐
         │  GPU/Display │
         └──────────────┘
```

**Key insight**: The compositor is everything! There's no separate display server and window manager. Apps render themselves, compositor just arranges and displays the buffers.

### Wayland Client-Compositor Interaction

#### Connection Establishment

```c
// Application connects to Wayland compositor
// Opens socket: $XDG_RUNTIME_DIR/$WAYLAND_DISPLAY
//   Usually: /run/user/1000/wayland-0
struct wl_display *display = wl_display_connect(NULL);

// Under the hood:
// 1. Parse WAYLAND_DISPLAY environment variable
// 2. Connect to Unix socket
// 3. Receive registry of available interfaces
```

**Socket path**: `/run/user/1000/wayland-0` (typically)

#### Window Creation and Rendering Flow

```
App (Wayland Client)                    Wayland Compositor
     |                                          |
     |--wl_compositor.create_surface()--------->|
     |<--surface object (ID: 123)---------------|
     |                                          |
     |--xdg_wm_base.get_xdg_surface()---------->|
     |<--xdg_surface object---------------------|
     |                                          |
     |--xdg_surface.get_toplevel()------------->|
     |<--toplevel object------------------------|
     |                                          |
     |<--xdg_surface.configure()----------------|
     |  "Suggested size: 1000x800"              |
     |                                          |
     |--xdg_surface.ack_configure()------------>|
     |  "OK, I'll use that size"                |
     |                                          |
     | APP RENDERS TO ITS OWN BUFFER            |
     | (using CPU, OpenGL, Vulkan - app choice!)|
     | Buffer lives in GPU memory (dmabuf)      |
     | or shared memory                         |
     |                                          |
     |--wl_surface.attach(buffer_handle)------->|
     |  "Display this buffer"                   |
     |                                          |
     |--wl_surface.commit()-------------------->|
     |  "I'm done, show it now"                 |
     |                                          |
     |                                     Compositor:
     |                                     - Reads buffer
     |                                     - Composites with
     |                                       other windows
     |                                     - Presents to display
     |                                     - Handles vsync
     |                                          |
     |<--wl_buffer.release()---------------------|
     |  "I'm done with buffer, you can reuse"   |
```

**Key points**:
1. App creates surface (window abstraction)
2. Compositor suggests size via configure event
3. **App renders to its own buffer** (not sending drawing commands!)
4. App passes **buffer handle** to compositor
5. Compositor composites and displays

### Wayland Code Example

#### Simple Wayland Application

```c
#include <wayland-client.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>
#include <fcntl.h>

// Global objects
struct wl_display *display = NULL;
struct wl_compositor *compositor = NULL;
struct wl_shm *shm = NULL;

// Registry listener - compositor advertises available features
static void registry_global(void *data, struct wl_registry *registry,
                           uint32_t name, const char *interface,
                           uint32_t version)
{
    if (strcmp(interface, "wl_compositor") == 0) {
        // Compositor interface - for creating surfaces
        compositor = wl_registry_bind(registry, name,
                                     &wl_compositor_interface, 1);
    } else if (strcmp(interface, "wl_shm") == 0) {
        // Shared memory interface - for creating buffers
        shm = wl_registry_bind(registry, name,
                              &wl_shm_interface, 1);
    }
}

static void registry_global_remove(void *data, struct wl_registry *registry,
                                   uint32_t name)
{
    // Handle removal of global objects
}

static const struct wl_registry_listener registry_listener = {
    registry_global,
    registry_global_remove
};

int main() {
    // STEP 1: Connect to Wayland compositor
    // Opens socket: /run/user/1000/wayland-0
    display = wl_display_connect(NULL);
    if (!display) {
        fprintf(stderr, "Cannot connect to Wayland compositor\n");
        return 1;
    }

    // STEP 2: Get registry and bind to interfaces
    struct wl_registry *registry = wl_display_get_registry(display);
    wl_registry_add_listener(registry, &registry_listener, NULL);

    // Wait for registry events
    wl_display_roundtrip(display);

    if (!compositor || !shm) {
        fprintf(stderr, "Compositor doesn't support required interfaces\n");
        return 1;
    }

    // STEP 3: Create surface (window)
    struct wl_surface *surface = wl_compositor_create_surface(compositor);

    // STEP 4: Create buffer for rendering
    int width = 800;
    int height = 600;
    int stride = width * 4;  // 4 bytes per pixel (ARGB8888)
    int size = stride * height;

    // Create shared memory file
    int fd = memfd_create("wayland-buffer", 0);
    ftruncate(fd, size);

    // Map it to our address space - THIS IS WHERE WE RENDER!
    uint32_t *pixels = mmap(NULL, size, PROT_READ | PROT_WRITE,
                           MAP_SHARED, fd, 0);

    // Create Wayland shared memory pool
    struct wl_shm_pool *pool = wl_shm_create_pool(shm, fd, size);
    struct wl_buffer *buffer = wl_shm_pool_create_buffer(pool, 0,
        width, height, stride, WL_SHM_FORMAT_ARGB8888);

    // STEP 5: RENDER TO BUFFER (client-side rendering!)
    // Fill with red color
    for (int i = 0; i < width * height; i++) {
        pixels[i] = 0xFFFF0000;  // ARGB: fully opaque red
    }

    // Draw a simple pattern
    for (int y = 0; y < height; y++) {
        for (int x = 0; x < width; x++) {
            if ((x / 50 + y / 50) % 2 == 0) {
                pixels[y * width + x] = 0xFF0000FF;  // Blue
            }
        }
    }

    // STEP 6: Attach buffer to surface and commit
    wl_surface_attach(surface, buffer, 0, 0);
    wl_surface_commit(surface);
    // Compositor now displays our buffer!

    // STEP 7: Event loop
    while (wl_display_dispatch(display) != -1) {
        // Process Wayland events
        // In real app: handle configure, input events, etc.
    }

    // Cleanup
    munmap(pixels, size);
    close(fd);
    wl_display_disconnect(display);

    return 0;
}
```

**Compile**: `gcc -o wayland_example wayland_example.c -lwayland-client`

**Key difference from X11**: We render to our own buffer (`pixels` array), then give the compositor a reference to it. No drawing commands sent over the wire!

### Wayland Protocol Flow

#### Rendering Path

```
Client                                  Compositor
  |                                         |
  | Allocate buffer (dmabuf or shm)         |
  | Render using CPU/OpenGL/Vulkan          |
  | (compositor never sees drawing!)        |
  |                                         |
  |--wl_surface.attach(buffer_id)---------->|
  |                                         |
  |--wl_surface.damage(x,y,w,h)------------>|
  |  "This region changed"                  |
  |                                         |
  |--wl_surface.commit()-------------------->|
  |  "Display this frame"                   |
  |                                         |
  |                                    Compositor:
  |                                    - Takes buffer handle
  |                                    - Imports to GPU
  |                                    - Composites with GPU
  |                                    - Scans out to display
  |                                         |
  |<--wl_buffer.release()-------------------|
  |  "Done with buffer, reuse it"           |
  |                                         |
  | Render next frame to released buffer    |
  |                                         |
```

**Zero-copy path**: With `dmabuf`, the buffer lives in GPU memory. Compositor and client both access the same GPU memory - no CPU copy needed!

### Wayland Advantages

1. **Security**:
   - Apps isolated from each other
   - Can't read other app's windows
   - Can't capture global keyboard input
   - Compositor controls everything

2. **Simplicity**:
   - ~50 core protocol messages (vs X11's 500+)
   - Clean, minimal design
   - Extensions are optional

3. **Performance**:
   - Client-side rendering (direct GPU access)
   - Zero-copy with dmabuf
   - No drawing commands over socket
   - Built-in vsync support

4. **Compositing**:
   - Built-in from day one
   - Compositor has full control
   - Smooth animations, effects

5. **Modern graphics**:
   - Native OpenGL/Vulkan support via EGL
   - HDR support being added
   - Variable refresh rate (VRR)

---

## Key Data Structures

### X11 Data Structures (Xorg)

#### Window Structure
```c
// Located in X Server code (not in kernel)
typedef struct _Window {
    DrawableRec drawable;         // Drawable properties
    WindowPtr parent;             // Parent window
    WindowPtr nextSib;            // Next sibling
    WindowPtr firstChild;         // First child window

    RegionPtr clipList;           // Clip region
    RegionPtr borderClip;         // Border clip region

    unsigned int deliverableEvents;  // Event mask
    Atom *properties;             // Window properties

    PixUnion background;          // Background pixmap/pixel
    PixUnion border;              // Border pixmap/pixel

    pointer devPrivates;          // Extension private data
} WindowRec, *WindowPtr;
```

**Location**: X Server memory space (not kernel)

#### Graphics Context
```c
typedef struct _GC {
    unsigned long serialNumber;
    int depth;                    // Drawable depth

    // Drawing attributes
    int alu;                      // Raster operation
    unsigned long fgPixel;        // Foreground color
    unsigned long bgPixel;        // Background color
    unsigned long planemask;      // Plane mask

    // Line attributes
    int lineWidth;
    int lineStyle;
    int capStyle;
    int joinStyle;

    PixmapPtr tile;              // Fill tile
    PixmapPtr stipple;           // Stipple pattern

    FontPtr font;                // Font for text
} GC, *GCPtr;
```

### Wayland Data Structures

#### Surface Structure
```c
// In compositor (e.g., wlroots)
struct wlr_surface {
    struct wl_resource *resource;  // Wayland protocol object

    struct wlr_buffer *buffer;     // Current buffer
    int32_t sx, sy;                // Surface-local offset

    struct {
        int32_t x, y;
        int32_t width, height;
    } current, pending;            // Double-buffered state

    pixman_region32_t damage;      // Damaged region
    pixman_region32_t opaque_region;
    pixman_region32_t input_region;

    bool mapped;                   // Is surface visible?

    struct wl_list subsurfaces;    // Child surfaces
    struct wl_listener destroy;
};
```

#### Buffer Structure
```c
struct wlr_buffer {
    const struct wlr_buffer_impl *impl;

    int width, height;

    bool dropped;                  // Compositor released it
    size_t n_locks;               // Reference count

    struct {
        struct wl_signal destroy;
        struct wl_signal release;  // Ready for reuse
    } events;
};

// DMA-BUF (GPU memory buffer)
struct wlr_dmabuf_buffer {
    struct wlr_buffer base;

    struct wlr_dmabuf_attributes dmabuf;
    int drm_fd;                    // DRM file descriptor
};
```

### Kernel Data Structures

#### DRM Framebuffer (`include/drm/drm_framebuffer.h`)
```c
struct drm_framebuffer {
    struct drm_device *dev;        // DRM device
    struct list_head head;         // List of framebuffers

    struct drm_mode_object base;   // Base object

    const struct drm_framebuffer_funcs *funcs;

    unsigned int pitches[4];       // Pitch for each plane
    unsigned int offsets[4];       // Offset for each plane
    uint64_t modifier;             // Format modifier
    unsigned int width;
    unsigned int height;
    int flags;

    uint32_t format;              // FOURCC format (XRGB8888, etc)
    struct drm_format_info *format_info;

    struct list_head filp_head;
};
```

**Location**: `drivers/gpu/drm/drm_framebuffer.c`

#### DRM CRTC (`include/drm/drm_crtc.h`)
```c
struct drm_crtc {
    struct drm_device *dev;
    struct list_head head;

    char *name;

    struct drm_modeset_lock mutex;

    struct drm_mode_object base;   // Object ID

    struct drm_plane *primary;     // Primary plane (main content)
    struct drm_plane *cursor;      // Cursor plane

    unsigned index;                // CRTC index

    int cursor_x;
    int cursor_y;

    bool enabled;
    struct drm_display_mode mode;  // Current mode

    int x, y;                      // Position in framebuffer

    const struct drm_crtc_funcs *funcs;
    const struct drm_crtc_helper_funcs *helper_private;
};
```

**Location**: `drivers/gpu/drm/drm_crtc.c`

---

## Kernel Integration: DRM and Input Subsystems

Both X11 and Wayland ultimately use the same kernel interfaces.

### DRM/KMS (Direct Rendering Manager / Kernel Mode Setting)

Located in: `drivers/gpu/drm/`

#### Opening DRM Device

```c
// Both X Server and Wayland compositor do this
int drm_fd = open("/dev/dri/card0", O_RDWR);

// In kernel: drivers/gpu/drm/drm_file.c
int drm_open(struct inode *inode, struct file *filp)
{
    struct drm_device *dev;
    struct drm_minor *minor;
    struct drm_file *priv;

    // Allocate per-file-descriptor state
    priv = kzalloc(sizeof(*priv), GFP_KERNEL);

    // Initialize file private data
    INIT_LIST_HEAD(&priv->fbs);
    INIT_LIST_HEAD(&priv->blobs);
    INIT_LIST_HEAD(&priv->pending_event_list);

    // Attach to device
    filp->private_data = priv;
    priv->filp = filp;

    return 0;
}
```

**Path**: `drivers/gpu/drm/drm_file.c:156`

#### Setting Display Mode

```c
// Application calls (X Server or Wayland compositor):
drmModeSetCrtc(drm_fd, crtc_id, fb_id, 0, 0,
               &connector_id, 1, &mode);

// In kernel: drivers/gpu/drm/drm_ioctl.c
long drm_ioctl(struct file *filp, unsigned int cmd, unsigned long arg)
{
    struct drm_file *file_priv = filp->private_data;
    struct drm_device *dev = file_priv->minor->dev;

    switch (cmd) {
        case DRM_IOCTL_MODE_SETCRTC:
            return drm_mode_setcrtc(dev, data, file_priv);
        // ... many other ioctls
    }
}

// drivers/gpu/drm/drm_crtc.c:666
int drm_mode_setcrtc(struct drm_device *dev, void *data,
                     struct drm_file *file_priv)
{
    struct drm_mode_crtc *crtc_req = data;
    struct drm_crtc *crtc;
    struct drm_framebuffer *fb = NULL;
    struct drm_display_mode *mode = NULL;

    // Find CRTC object
    crtc = drm_crtc_find(dev, file_priv, crtc_req->crtc_id);

    // Find framebuffer
    if (crtc_req->fb_id) {
        fb = drm_framebuffer_lookup(dev, file_priv, crtc_req->fb_id);
    }

    // Convert mode
    if (crtc_req->mode_valid) {
        drm_mode_convert_umode(dev, mode, &crtc_req->mode);
    }

    // Call driver-specific function
    ret = crtc->funcs->set_config(&set, &ctx);

    return ret;
}
```

**Path**: `drivers/gpu/drm/drm_crtc.c:666`

#### Page Flipping (Vsync-synchronized buffer swap)

```c
// Application calls:
drmModePageFlip(drm_fd, crtc_id, fb_id,
                DRM_MODE_PAGE_FLIP_EVENT, user_data);

// In kernel: drivers/gpu/drm/drm_plane.c
int drm_mode_page_flip_ioctl(struct drm_device *dev,
                              void *data,
                              struct drm_file *file_priv)
{
    struct drm_mode_crtc_page_flip *page_flip = data;
    struct drm_crtc *crtc;
    struct drm_framebuffer *fb;
    struct drm_pending_vblank_event *e = NULL;

    // Find CRTC and framebuffer
    crtc = drm_crtc_find(dev, file_priv, page_flip->crtc_id);
    fb = drm_framebuffer_lookup(dev, file_priv, page_flip->fb_id);

    // Allocate vsync event
    if (page_flip->flags & DRM_MODE_PAGE_FLIP_EVENT) {
        e = kzalloc(sizeof(*e), GFP_KERNEL);
        e->event.base.type = DRM_EVENT_FLIP_COMPLETE;
        e->event.user_data = page_flip->user_data;
    }

    // Schedule page flip at next vblank
    ret = crtc->funcs->page_flip(crtc, fb, e, page_flip->flags, &ctx);

    return ret;
}
```

**Path**: `drivers/gpu/drm/drm_plane.c:1104`

### Input Subsystem

Located in: `drivers/input/`

#### Input Event Structure

```c
// include/uapi/linux/input.h
struct input_event {
    struct timeval time;    // Event timestamp
    __u16 type;            // EV_KEY, EV_REL, EV_ABS, etc.
    __u16 code;            // Key code, axis code
    __s32 value;           // 1=press, 0=release, or axis value
};
```

#### Input Device Registration

```c
// drivers/input/input.c:2090
int input_register_device(struct input_dev *dev)
{
    struct input_devres *devres = NULL;
    struct input_handler *handler;
    unsigned int packet_size;

    // Validate device capabilities
    if (!dev->setkeycode)
        dev->setkeycode = input_default_setkeycode;
    if (!dev->getkeycode)
        dev->getkeycode = input_default_getkeycode;

    // Calculate packet size for event batching
    packet_size = input_estimate_events_per_packet(dev);

    // Register with device core
    error = device_add(&dev->dev);

    // Attach to handlers (evdev, mousedev, etc.)
    list_for_each_entry(handler, &input_handler_list, node)
        input_attach_handler(dev, handler);

    return 0;
}
```

**Path**: `drivers/input/input.c:2090`

#### Event Distribution

```c
// drivers/input/input.c:367
void input_event(struct input_dev *dev,
                 unsigned int type, unsigned int code, int value)
{
    struct input_handle *handle;

    // Timestamp the event
    struct timeval time;
    do_gettimeofday(&time);

    // Send to all handlers (evdev, mousedev, etc.)
    rcu_read_lock();
    list_for_each_entry_rcu(handle, &dev->h_list, d_node) {
        if (handle->open)
            handle->handler->event(handle, type, code, value);
    }
    rcu_read_unlock();
}
```

**Path**: `drivers/input/input.c:367`

#### Reading Events (evdev)

```c
// drivers/input/evdev.c:510
static ssize_t evdev_read(struct file *file, char __user *buffer,
                          size_t count, loff_t *ppos)
{
    struct evdev_client *client = file->private_data;
    struct evdev *evdev = client->evdev;
    struct input_event event;

    // Wait for events
    if (client->head == client->tail && evdev->exist &&
        (file->f_flags & O_NONBLOCK))
        return -EAGAIN;

    retval = wait_event_interruptible(evdev->wait,
        client->head != client->tail || !evdev->exist);

    // Copy events to userspace
    while (retval + input_event_size() <= count &&
           evdev_fetch_next_event(client, &event)) {
        if (copy_to_user(buffer + retval, &event,
                        input_event_size()))
            return -EFAULT;
        retval += input_event_size();
    }

    return retval;
}
```

**Path**: `drivers/input/evdev.c:510`

#### X11 Input Reading

```c
// X Server reads from /dev/input/eventX
int fd = open("/dev/input/event0", O_RDONLY | O_NONBLOCK);

struct input_event ev;
while (read(fd, &ev, sizeof(ev)) == sizeof(ev)) {
    if (ev.type == EV_KEY) {
        // Key press/release
        // X Server converts to X11 KeyPress/KeyRelease event
        // Sends to appropriate X client
    } else if (ev.type == EV_REL) {
        // Mouse movement
        // X Server updates cursor position
        // Sends MotionNotify to X client under cursor
    }
}
```

#### Wayland Input Reading

```c
// Wayland compositor reads from /dev/input/eventX (via libinput)
struct libinput *li = libinput_udev_create_context(...);

while (1) {
    libinput_dispatch(li);

    while ((event = libinput_get_event(li)) != NULL) {
        switch (libinput_event_get_type(event)) {
            case LIBINPUT_EVENT_KEYBOARD_KEY:
                // Get keyboard event
                struct libinput_event_keyboard *key_event =
                    libinput_event_get_keyboard_event(event);

                // Determine which surface has focus
                struct wlr_surface *surface = get_focused_surface();

                // Send to Wayland client
                wl_keyboard_send_key(surface->keyboard,
                    serial, time, key, state);
                break;

            case LIBINPUT_EVENT_POINTER_MOTION:
                // Mouse moved
                // Update cursor position
                // Send to focused surface
                break;
        }
        libinput_event_destroy(event);
    }
}
```

---

## Complete Rendering Paths

### X11 Rendering Path

```
Application
    │
    │ XDrawRectangle(display, window, gc, x, y, w, h)
    │
    ↓
Xlib (libX11.so)
    │
    │ Serialize command to X11 protocol
    │ Protocol: opcode=PolyRectangle, window_id, gc_id, rect[]
    │
    ↓
Unix Socket (/tmp/.X11-unix/X0)
    │
    │ Send over socket
    │
    ↓
X Server (Xorg)
    │
    │ Receive command from socket
    │ Parse X11 protocol
    │ Validate window, GC exist
    │
    ↓
X Server Rendering
    │
    │ Apply GC attributes (color, line width, etc.)
    │ Clip to window boundaries
    │ Rasterize rectangle
    │
    ↓
Framebuffer (X Server memory)
    │
    │ Write pixels to framebuffer
    │
    ↓
DRM/KMS Kernel Driver
    │
    │ ioctl(DRM_IOCTL_MODE_SETCRTC, ...)
    │ or DRI extension for direct rendering
    │
    ↓
GPU Driver (i915, amdgpu, nouveau)
    │
    │ Program GPU scanout
    │
    ↓
Display Hardware
```

**Total latency**: Userspace → socket → X Server → DRM → GPU
**Copies**: Multiple (app → socket buffer → X Server → framebuffer → GPU)

### Wayland Rendering Path

```
Application
    │
    │ Render to own buffer using CPU/OpenGL/Vulkan
    │ (Compositor doesn't see drawing commands!)
    │
    ↓
EGL / OpenGL / Vulkan
    │
    │ Allocate buffer in GPU memory (dmabuf)
    │ Render directly using GPU
    │
    ↓
Application Buffer (GPU memory)
    │
    │ Buffer contains finished frame
    │
    ↓
Wayland Protocol
    │
    │ wl_surface_attach(buffer_handle)
    │ wl_surface_commit()
    │ (Just passing reference, not pixel data!)
    │
    ↓
Unix Socket (/run/user/1000/wayland-0)
    │
    │ Send dmabuf file descriptor over socket (SCM_RIGHTS)
    │
    ↓
Wayland Compositor
    │
    │ Receive dmabuf handle
    │ Import to compositor's GPU context
    │ Composite with other windows (GPU-accelerated)
    │
    ↓
Compositor Output Buffer (GPU memory)
    │
    │ Final composited frame
    │
    ↓
DRM/KMS Kernel Driver
    │
    │ drmModePageFlip(drm_fd, crtc_id, fb_id, ...)
    │ Schedule flip at next vblank
    │
    ↓
GPU Driver
    │
    │ Scanout from compositor buffer
    │
    ↓
Display Hardware
```

**Total latency**: App GPU render → compositor GPU composite → scanout
**Copies**: Zero-copy! Buffer stays in GPU memory throughout.

---

## Comparison Table

| Aspect | X11 | Wayland |
|--------|-----|---------|
| **Architecture** | Client-Server (separate WM) | Unified Compositor |
| **Who renders** | X Server executes commands | Client renders to buffer |
| **Protocol** | Network transparent (socket) | Local only (socket) |
| **Protocol size** | ~500 messages | ~50 core messages |
| **Security** | Apps can spy on each other | Isolated applications |
| **Compositing** | Extension (bolted on) | Built-in from start |
| **GPU access** | Via GLX/DRI extensions | Native EGL |
| **Rendering path** | Commands over socket | Buffer handles over socket |
| **Copies** | Multiple | Zero-copy (with dmabuf) |
| **Latency** | Higher (round-trips) | Lower (direct rendering) |
| **Tearing** | Common with DRI | Vsync built-in |
| **Window decorations** | WM sends draw commands to X Server | Server-side or client-side |
| **Input routing** | X Server decides | Compositor decides |
| **Screen capture** | Any app can do it | Compositor-mediated only |
| **Remote display** | Built-in | Requires extensions (RDP, VNC) |
| **Maturity** | 35+ years old | ~15 years old |
| **Adoption** | Legacy, maintenance mode | Modern Linux desktops |

---

## Visual Architecture Diagrams

### X11 Architecture

```
┌──────────────────────────────────────────────────────────────┐
│                        USERSPACE                             │
│                                                              │
│  ┌──────────────┐  ┌──────────────┐  ┌──────────────┐      │
│  │ Firefox      │  │ Terminal     │  │ Editor       │      │
│  │              │  │              │  │              │      │
│  │ - Xlib calls │  │ - Xlib calls │  │ - Xlib calls │      │
│  │ - Send draw  │  │ - Send draw  │  │ - Send draw  │      │
│  │   commands   │  │   commands   │  │   commands   │      │
│  └──────┬───────┘  └──────┬───────┘  └──────┬───────┘      │
│         │                 │                 │               │
│         └─────────────────┼─────────────────┘               │
│                           │                                 │
│            X11 Protocol (over Unix socket)                  │
│         /tmp/.X11-unix/X0                                   │
│                           ↓                                 │
│  ┌────────────────────────────────────────────────────┐    │
│  │              X Server (Xorg)                       │    │
│  │                                                    │    │
│  │  - Receives drawing commands                      │    │
│  │  - Executes rasterization                         │    │
│  │  - Manages windows, input, resources              │    │
│  │  - Composites windows (with Composite ext)        │    │
│  └──────────────────┬─────────────────────────────────┘    │
│                     │                                       │
│       X11 Protocol (Window Manager is special client)      │
│                     │                                       │
│  ┌──────────────────┴───────────────────┐                  │
│  │   Window Manager (i3, dwm)           │                  │
│  │                                      │                  │
│  │  - Intercepts MapRequest events      │                  │
│  │  - Decides window placement          │                  │
│  │  - Draws decorations                 │                  │
│  └──────────────────────────────────────┘                  │
│                                                              │
└───────────────────────────┬──────────────────────────────────┘
                            │
                    ioctl() system calls
                    (DRM_IOCTL_*)
                            │
┌───────────────────────────┴──────────────────────────────────┐
│                     KERNEL SPACE                             │
│                                                              │
│  ┌────────────────────────────────────────────────────┐    │
│  │         DRM (Direct Rendering Manager)             │    │
│  │                                                    │    │
│  │  - Modesetting (KMS)                              │    │
│  │  - Memory management (GEM)                        │    │
│  │  - GPU command submission                         │    │
│  └──────────────────┬─────────────────────────────────┘    │
│                     │                                       │
│  ┌──────────────────┴─────────────────────────────────┐    │
│  │    GPU Driver (i915, amdgpu, nouveau)              │    │
│  │                                                    │    │
│  │  - Hardware-specific operations                   │    │
│  │  - Command submission                             │    │
│  │  - Memory management                              │    │
│  └──────────────────┬─────────────────────────────────┘    │
│                     │                                       │
└─────────────────────┼───────────────────────────────────────┘
                      │
                      ↓
            ┌─────────────────┐
            │   GPU Hardware  │
            │                 │
            │  Display Engine │
            └────────┬────────┘
                     │
                     ↓
               ┌─────────┐
               │ Monitor │
               └─────────┘
```

### Wayland Architecture

```
┌──────────────────────────────────────────────────────────────┐
│                        USERSPACE                             │
│                                                              │
│  ┌──────────────┐  ┌──────────────┐  ┌──────────────┐      │
│  │ Firefox      │  │ Terminal     │  │ Editor       │      │
│  │              │  │              │  │              │      │
│  │ - Renders    │  │ - Renders    │  │ - Renders    │      │
│  │   own buffer │  │   own buffer │  │   own buffer │      │
│  │ - OpenGL/    │  │ - Cairo/     │  │ - Vulkan     │      │
│  │   Vulkan     │  │   software   │  │              │      │
│  └──────┬───────┘  └──────┬───────┘  └──────┬───────┘      │
│         │                 │                 │               │
│         │  GPU Memory     │  Shared Memory  │  GPU Memory   │
│         │  (dmabuf)       │  (wl_shm)       │  (dmabuf)     │
│         │                 │                 │               │
│         └─────────────────┼─────────────────┘               │
│                           │                                 │
│            Wayland Protocol (over Unix socket)              │
│         /run/user/1000/wayland-0                            │
│                           │                                 │
│     (Pass buffer handles, NOT pixel data!)                  │
│                           ↓                                 │
│  ┌────────────────────────────────────────────────────┐    │
│  │         Wayland Compositor                         │    │
│  │          (Sway, GNOME, KDE)                        │    │
│  │                                                    │    │
│  │  THREE ROLES COMBINED:                             │    │
│  │  1. Display Server - manages hardware             │    │
│  │  2. Window Manager - arranges windows             │    │
│  │  3. Compositor - combines buffers                 │    │
│  │                                                    │    │
│  │  - Imports client buffers                         │    │
│  │  - Composites using GPU                           │    │
│  │  - Handles input routing                          │    │
│  │  - Direct DRM access                              │    │
│  └──────────────────┬─────────────────────────────────┘    │
│                     │                                       │
└─────────────────────┼───────────────────────────────────────┘
                      │
              ioctl() system calls
              (DRM_IOCTL_*, input events)
                      │
┌─────────────────────┴─────────────────────────────────────┐
│                     KERNEL SPACE                           │
│                                                            │
│  ┌────────────────────────────────────────────────────┐  │
│  │         DRM (Direct Rendering Manager)             │  │
│  │                                                    │  │
│  │  - KMS (Kernel Mode Setting)                      │  │
│  │  - GEM (Graphics Execution Manager)               │  │
│  │  - PRIME (buffer sharing)                         │  │
│  └──────────────────┬─────────────────────────────────┘  │
│                     │                                     │
│  ┌──────────────────┴─────────────────────────────────┐  │
│  │    GPU Driver (i915, amdgpu, nouveau)              │  │
│  │                                                    │  │
│  │  - Scanout from compositor buffer                 │  │
│  │  - Zero-copy operation                            │  │
│  └──────────────────┬─────────────────────────────────┘  │
│                     │                                     │
└─────────────────────┼─────────────────────────────────────┘
                      │
                      ↓
            ┌─────────────────┐
            │   GPU Hardware  │
            │                 │
            │  Display Engine │
            └────────┬────────┘
                     │
                     ↓
               ┌─────────┐
               │ Monitor │
               └─────────┘
```

### Data Flow Comparison

```
X11 Data Flow:
──────────────

App: XDrawRectangle()
  │
  ├─→ Serialize to X11 protocol message
  │
  ↓
Socket: /tmp/.X11-unix/X0
  │
  ├─→ Send drawing command
  │
  ↓
X Server: Parse command
  │
  ├─→ Rasterize rectangle
  ├─→ Write to framebuffer
  │
  ↓
DRM: ioctl(MODE_SETCRTC)
  │
  ↓
GPU: Scanout framebuffer
  │
  ↓
Display


Wayland Data Flow:
──────────────────

App: OpenGL render
  │
  ├─→ Direct GPU rendering
  ├─→ Result in GPU buffer (dmabuf)
  │
  ↓
Wayland Protocol: wl_surface_attach(dmabuf_fd)
  │
  ├─→ Pass buffer HANDLE (not pixels!)
  │
  ↓
Socket: /run/user/1000/wayland-0
  │
  ├─→ Send file descriptor via SCM_RIGHTS
  │
  ↓
Compositor: Import dmabuf
  │
  ├─→ GPU composite with other windows
  ├─→ Zero-copy operation!
  │
  ↓
DRM: drmModePageFlip()
  │
  ↓
GPU: Scanout compositor buffer
  │
  ↓
Display
```

---

## Key Takeaways

### X11 Philosophy
- **Centralized display server** that executes drawing commands
- **Network transparency** was a core design goal (1980s requirement)
- **Window manager is just another client** with special privileges
- **Pull model**: Server tells app to redraw (Expose events)
- Works well for simple applications, but...
- Modern GPU-accelerated apps need DRI extensions to bypass X Server
- Security model is broken (any app can spy on others)

### Wayland Philosophy
- **Unified compositor** combines display server + window manager + compositor
- **Client-side rendering** - apps render to their own buffers
- **Zero-copy** with dmabuf (GPU buffer sharing)
- **Push model**: App commits frames when ready
- **Security by design** - apps are isolated
- Simpler protocol, better performance
- Native GPU acceleration without extensions

### Both Use Same Kernel Layer
- DRM/KMS for display management
- Input subsystem for keyboard/mouse
- The difference is the userspace architecture

### Why Wayland is Winning
1. **Better security**: Apps can't spy on each other
2. **Better performance**: Zero-copy, direct GPU access
3. **Simpler code**: Less legacy cruft
4. **Modern features**: HDR, VRR, multi-DPI native
5. **Smooth compositing**: Built-in from day one

### X11 Still Exists Because
1. **Network transparency** (though VNC/RDP work on Wayland)
2. **Legacy applications** (XWayland provides compatibility)
3. **Mature ecosystem** (35+ years of development)
4. **Some niche use cases** (remote thin clients, etc.)

---

## Quick Reference

### X11 Commands and Paths

```bash
# X Server socket
/tmp/.X11-unix/X0          # Display :0
/tmp/.X11-unix/X1          # Display :1

# Environment variable
echo $DISPLAY              # :0, :1, or hostname:0

# List windows
xwininfo -tree -root

# Monitor X protocol
xscope -i127.0.0.1:6001

# X Server log
~/.local/share/xorg/Xorg.0.log
/var/log/Xorg.0.log

# Resources
xdpyinfo                   # Display info
xrandr                     # Monitor configuration
xprop                      # Window properties
xev                        # Monitor events
```

### Wayland Commands and Paths

```bash
# Wayland compositor socket
/run/user/1000/wayland-0   # Usually
$XDG_RUNTIME_DIR/$WAYLAND_DISPLAY

# Environment variable
echo $WAYLAND_DISPLAY      # wayland-0, wayland-1, etc.

# List surfaces (compositor-specific)
swaymsg -t get_tree        # For Sway

# Wayland debugging
WAYLAND_DEBUG=1 <app>      # Show protocol messages

# Compositor log (depends on compositor)
journalctl --user -u sway  # For Sway
```

### DRM/KMS Paths

```bash
# DRM devices
/dev/dri/card0             # Primary GPU
/dev/dri/card1             # Secondary GPU
/dev/dri/renderD128        # Render-only node

# Device info
ls -l /dev/dri/
drwxr-xr-x  2 root root       100 Nov  5 10:00 by-path
crw-rw----+ 1 root video 226,   0 Nov  5 10:00 card0
crw-rw----+ 1 root video 226,   1 Nov  5 10:00 card1
crw-rw-rw-  1 root render 226, 128 Nov  5 10:00 renderD128

# Query GPU
lspci -v -s $(lspci | grep VGA | cut -d' ' -f1)

# DRM info
cat /sys/kernel/debug/dri/0/name
cat /sys/class/drm/card0/device/uevent
```

### Input Paths

```bash
# Input devices
/dev/input/event0          # Keyboard
/dev/input/event1          # Mouse
/dev/input/event2          # Touchpad
# ... etc

# List input devices
cat /proc/bus/input/devices

# Monitor input events
evtest /dev/input/event0

# libinput (used by Wayland)
libinput debug-events
```

### Kernel Source Locations

```bash
# DRM core
drivers/gpu/drm/drm_crtc.c          # CRTC management
drivers/gpu/drm/drm_framebuffer.c   # Framebuffer handling
drivers/gpu/drm/drm_plane.c         # Plane handling
drivers/gpu/drm/drm_ioctl.c         # ioctl dispatch

# Intel GPU driver
drivers/gpu/drm/i915/

# AMD GPU driver
drivers/gpu/drm/amd/

# Input subsystem
drivers/input/input.c               # Input core
drivers/input/evdev.c               # Event device interface
drivers/input/keyboard/             # Keyboard drivers
drivers/input/mouse/                # Mouse drivers
```

### Compile Examples

```bash
# X11 example
gcc -o x11_example x11_example.c -lX11

# Wayland example
gcc -o wayland_example wayland_example.c -lwayland-client

# Direct DRM example (like simple_triangle.c)
gcc -o drm_example drm_example.c $(pkg-config --cflags --libs libdrm) -lm
```

---

## Summary

**X11** is a 35-year-old client-server architecture where applications send drawing commands to a centralized X Server, which executes them and manages the display. A separate Window Manager (itself an X client) handles window placement. Network transparency was a core design goal, but this leads to complexity and security issues in modern desktop use.

**Wayland** is a modern unified compositor architecture where applications render to their own buffers and pass buffer handles to the compositor, which arranges and displays them. The compositor combines the roles of display server, window manager, and compositor. This enables zero-copy rendering, better security through isolation, and simpler code.

Both ultimately use the kernel's DRM/KMS subsystem for display management and the input subsystem for device handling. The architectural differences are purely in userspace.

The Linux desktop is transitioning from X11 to Wayland because Wayland provides better security, performance, and support for modern GPU-accelerated compositing—requirements that didn't exist when X11 was designed in the 1980s.
