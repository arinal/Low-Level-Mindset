# Rotating Triangle: The 3D "Hello World"

A complete, working example of a rotating 3D triangle with deep dive into what happens internally—from OpenGL calls to syscalls to GPU execution.

## Table of Contents
- [The Complete Code](#the-complete-code)
- [Compilation and Running](#compilation-and-running)
- [What Happens Internally](#what-happens-internally)
- [Syscall Trace Analysis](#syscall-trace-analysis)
- [Command Buffer Contents](#command-buffer-contents)
- [Memory Layout](#memory-layout)
- [The Complete Flow](#the-complete-flow)
- [Performance Analysis](#performance-analysis)

---

## The Complete Code

```c
// rotating_triangle.c
// Rotating triangle using EGL + OpenGL ES 2.0
// Demonstrates modern Linux graphics stack: Mesa → DRM → GPU

#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#include <EGL/egl.h>
#include <GLES2/gl2.h>
#include <gbm.h>
#include <fcntl.h>
#include <unistd.h>
#include <xf86drm.h>
#include <xf86drmMode.h>

// =============================================================================
// VERTEX SHADER
// Runs on GPU for each vertex (3 times per triangle)
// =============================================================================
const char *vertex_shader_src =
    "#version 100\n"
    "attribute vec3 position;\n"           // Input: vertex position
    "attribute vec3 color;\n"              // Input: vertex color
    "varying vec3 vColor;\n"               // Output: pass color to fragment shader
    "uniform mat4 rotation;\n"             // Uniform: rotation matrix
    "void main() {\n"
    "    gl_Position = rotation * vec4(position, 1.0);\n"
    "    vColor = color;\n"
    "}\n";

// =============================================================================
// FRAGMENT SHADER
// Runs on GPU for each pixel inside the triangle
// =============================================================================
const char *fragment_shader_src =
    "#version 100\n"
    "precision mediump float;\n"
    "varying vec3 vColor;\n"               // Input: color from vertex shader
    "void main() {\n"
    "    gl_FragColor = vec4(vColor, 1.0);\n"  // Output: pixel color
    "}\n";

// =============================================================================
// SHADER COMPILATION
// =============================================================================
GLuint compile_shader(GLenum type, const char *source) {
    // INTERNAL: This allocates GPU memory for shader bytecode
    // Triggers: ioctl(DRM_IOCTL_I915_GEM_CREATE) to allocate GPU memory
    GLuint shader = glCreateShader(type);

    // INTERNAL: Copies shader source to driver's internal buffer (userspace)
    // No syscall yet - just memory copy
    glShaderSource(shader, 1, &source, NULL);

    // INTERNAL: Compiles GLSL → GPU machine code
    // This is complex! Mesa compiler runs in userspace:
    //   1. Parse GLSL
    //   2. Optimize (SSA, dead code elimination, etc.)
    //   3. Generate GPU instructions (Intel GEN assembly, AMD GCN, etc.)
    //   4. Write to GPU memory buffer
    // Triggers: ioctl(DRM_IOCTL_I915_GEM_PWRITE) to write bytecode to GPU
    glCompileShader(shader);

    // Check compilation
    GLint success;
    glGetShaderiv(shader, GL_COMPILE_STATUS, &success);
    if (!success) {
        char log[512];
        glGetShaderInfoLog(shader, 512, NULL, log);
        fprintf(stderr, "Shader compilation failed: %s\n", log);
        exit(1);
    }

    return shader;
}

// =============================================================================
// MAIN PROGRAM
// =============================================================================
int main() {
    printf("=== Opening DRM Device ===\n");

    // =========================================================================
    // STEP 1: OPEN DRM DEVICE
    // =========================================================================
    // SYSCALL: openat(AT_FDCWD, "/dev/dri/card0", O_RDWR)
    // This is the FIRST syscall - opens the GPU device
    int drm_fd = open("/dev/dri/card0", O_RDWR);
    if (drm_fd < 0) {
        perror("Cannot open /dev/dri/card0");
        return 1;
    }
    printf("✓ DRM device opened (fd=%d)\n", drm_fd);

    // =========================================================================
    // STEP 2: INITIALIZE GBM (Generic Buffer Manager)
    // =========================================================================
    // INTERNAL: Sets up buffer management for GPU
    // SYSCALL: ioctl(drm_fd, DRM_IOCTL_VERSION, ...) - check driver version
    struct gbm_device *gbm = gbm_create_device(drm_fd);
    printf("✓ GBM device created\n");

    // =========================================================================
    // STEP 3: CREATE GBM SURFACE (rendering target)
    // =========================================================================
    // INTERNAL: Allocates framebuffer memory
    // On integrated GPU: allocates in system RAM
    // On discrete GPU: allocates in VRAM
    // SYSCALL: ioctl(drm_fd, DRM_IOCTL_I915_GEM_CREATE, ...) - allocate GPU memory
    struct gbm_surface *gbm_surface = gbm_surface_create(
        gbm,
        1920, 1080,                        // Resolution
        GBM_FORMAT_XRGB8888,               // 32-bit RGB
        GBM_BO_USE_SCANOUT | GBM_BO_USE_RENDERING  // For display + rendering
    );
    printf("✓ GBM surface created (1920x1080)\n");

    // =========================================================================
    // STEP 4: INITIALIZE EGL (Embedded Graphics Library)
    // =========================================================================
    // INTERNAL: Sets up OpenGL context management
    // SYSCALL: Multiple ioctls to query GPU capabilities
    EGLDisplay display = eglGetDisplay((EGLNativeDisplayType)gbm);
    eglInitialize(display, NULL, NULL);
    printf("✓ EGL display initialized\n");

    // Choose EGL configuration
    const EGLint config_attribs[] = {
        EGL_SURFACE_TYPE, EGL_WINDOW_BIT,
        EGL_RED_SIZE, 8,
        EGL_GREEN_SIZE, 8,
        EGL_BLUE_SIZE, 8,
        EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT,
        EGL_NONE
    };
    EGLConfig config;
    EGLint num_configs;
    eglChooseConfig(display, config_attribs, &config, 1, &num_configs);

    // Create EGL context
    // INTERNAL: Allocates GPU context state (registers, shader cache, etc.)
    // SYSCALL: ioctl(DRM_IOCTL_I915_GEM_CONTEXT_CREATE, ...)
    const EGLint context_attribs[] = {
        EGL_CONTEXT_CLIENT_VERSION, 2,
        EGL_NONE
    };
    EGLContext context = eglCreateContext(display, config, EGL_NO_CONTEXT,
                                          context_attribs);
    printf("✓ EGL context created\n");

    // Create EGL surface
    EGLSurface surface = eglCreateWindowSurface(display, config,
                                                 (EGLNativeWindowType)gbm_surface,
                                                 NULL);

    // Make context current
    // INTERNAL: Binds this context to the current thread
    eglMakeCurrent(display, surface, surface, context);
    printf("✓ EGL context made current\n\n");

    // =========================================================================
    // STEP 5: COMPILE SHADERS
    // =========================================================================
    printf("=== Compiling Shaders ===\n");

    GLuint vs = compile_shader(GL_VERTEX_SHADER, vertex_shader_src);
    printf("✓ Vertex shader compiled\n");

    GLuint fs = compile_shader(GL_FRAGMENT_SHADER, fragment_shader_src);
    printf("✓ Fragment shader compiled\n");

    // Link shaders into program
    // INTERNAL: Links vertex + fragment shaders together
    // Creates final GPU program with input/output mappings
    GLuint program = glCreateProgram();
    glAttachShader(program, vs);
    glAttachShader(program, fs);
    glLinkProgram(program);

    GLint link_success;
    glGetProgramiv(program, GL_LINK_STATUS, &link_success);
    if (!link_success) {
        char log[512];
        glGetProgramInfoLog(program, 512, NULL, log);
        fprintf(stderr, "Program linking failed: %s\n", log);
        return 1;
    }

    // Activate the shader program
    // INTERNAL: Tells GPU "use this program for rendering"
    glUseProgram(program);
    printf("✓ Shader program linked and activated\n\n");

    // =========================================================================
    // STEP 6: PREPARE VERTEX DATA
    // =========================================================================
    printf("=== Setting Up Vertex Data ===\n");

    // Triangle vertices (x, y, z)
    float vertices[] = {
         0.0f,  0.5f, 0.0f,    // Top vertex
        -0.5f, -0.5f, 0.0f,    // Bottom-left vertex
         0.5f, -0.5f, 0.0f     // Bottom-right vertex
    };

    // Vertex colors (r, g, b)
    float colors[] = {
        1.0f, 0.0f, 0.0f,      // Red
        0.0f, 1.0f, 0.0f,      // Green
        0.0f, 0.0f, 1.0f       // Blue
    };

    // Create Vertex Buffer Object (VBO) for positions
    // INTERNAL: Allocates GPU buffer for vertex data
    // SYSCALL: ioctl(DRM_IOCTL_I915_GEM_CREATE, {size=36}) - allocate 36 bytes
    GLuint vbo_pos;
    glGenBuffers(1, &vbo_pos);
    glBindBuffer(GL_ARRAY_BUFFER, vbo_pos);

    // Upload vertex data to GPU
    // INTERNAL: Copies data from CPU (vertices array) to GPU buffer
    // On integrated GPU: memcpy to system RAM
    // On discrete GPU: writes to system RAM, GPU will DMA it later
    // SYSCALL: ioctl(DRM_IOCTL_I915_GEM_PWRITE, ...) - write to GPU buffer
    glBufferData(GL_ARRAY_BUFFER, sizeof(vertices), vertices, GL_STATIC_DRAW);
    printf("✓ Vertex position buffer created and uploaded\n");

    // Create VBO for colors
    GLuint vbo_color;
    glGenBuffers(1, &vbo_color);
    glBindBuffer(GL_ARRAY_BUFFER, vbo_color);
    glBufferData(GL_ARRAY_BUFFER, sizeof(colors), colors, GL_STATIC_DRAW);
    printf("✓ Vertex color buffer created and uploaded\n");

    // Get attribute locations from shader
    GLint pos_attrib = glGetAttribLocation(program, "position");
    GLint color_attrib = glGetAttribLocation(program, "color");
    printf("✓ Shader attribute locations: position=%d, color=%d\n",
           pos_attrib, color_attrib);

    // Get uniform location for rotation matrix
    GLint rotation_uniform = glGetUniformLocation(program, "rotation");
    printf("✓ Uniform location: rotation=%d\n\n", rotation_uniform);

    // =========================================================================
    // STEP 7: RENDERING LOOP
    // =========================================================================
    printf("=== Starting Render Loop ===\n");

    for (int frame = 0; frame < 360; frame++) {
        // Calculate rotation angle
        float angle = frame * M_PI / 180.0f;  // Degrees to radians

        // Create rotation matrix (rotating around Z-axis)
        float rotation[16] = {
            cosf(angle), -sinf(angle), 0, 0,
            sinf(angle),  cosf(angle), 0, 0,
            0,            0,           1, 0,
            0,            0,           0, 1
        };

        // =====================================================================
        // COMMAND BUFFER BUILDING STARTS HERE
        // Everything below writes to command buffer in userspace memory
        // NO SYSCALLS until eglSwapBuffers()!
        // =====================================================================

        // Clear screen
        // INTERNAL: Writes "CLEAR" command to command buffer
        //   Command buffer entry:
        //     {OPCODE: CLEAR_COLOR, COLOR: [0,0,0,1]}
        glClear(GL_COLOR_BUFFER_BIT);

        // Set rotation matrix
        // INTERNAL: Writes "LOAD_UNIFORM" command to command buffer
        //   Command buffer entry:
        //     {OPCODE: LOAD_UNIFORM_MAT4, LOCATION: rotation_uniform,
        //      DATA: [16 floats]}
        glUniformMatrix4fv(rotation_uniform, 1, GL_FALSE, rotation);

        // Bind vertex position buffer
        glBindBuffer(GL_ARRAY_BUFFER, vbo_pos);
        glVertexAttribPointer(pos_attrib, 3, GL_FLOAT, GL_FALSE, 0, 0);
        glEnableVertexAttribArray(pos_attrib);
        // INTERNAL: Writes commands:
        //   {OPCODE: BIND_VERTEX_BUFFER, BUFFER: vbo_pos, ATTRIB: 0}
        //   {OPCODE: SET_VERTEX_FORMAT, ATTRIB: 0, SIZE: 3, TYPE: FLOAT}

        // Bind vertex color buffer
        glBindBuffer(GL_ARRAY_BUFFER, vbo_color);
        glVertexAttribPointer(color_attrib, 3, GL_FLOAT, GL_FALSE, 0, 0);
        glEnableVertexAttribArray(color_attrib);
        // INTERNAL: Same as above for color attribute

        // Draw the triangle!
        // INTERNAL: Writes "DRAW" command to command buffer
        //   Command buffer entry:
        //     {OPCODE: DRAW_ARRAYS, PRIMITIVE: TRIANGLES,
        //      FIRST: 0, COUNT: 3}
        glDrawArrays(GL_TRIANGLES, 0, 3);

        // =====================================================================
        // COMMAND BUFFER SUBMISSION
        // =====================================================================

        // Present the frame
        // INTERNAL: This is where the MAGIC happens!
        //
        // 1. Flush CPU cache (if needed)
        // 2. Finalize command buffer
        // 3. SYSCALL: ioctl(DRM_IOCTL_I915_GEM_EXECBUFFER2, ...)
        //    - Submits command buffer to GPU
        //    - GPU starts executing commands asynchronously
        // 4. SYSCALL: ioctl(DRM_IOCTL_MODE_ADDFB2, ...)
        //    - Create framebuffer object
        // 5. SYSCALL: ioctl(DRM_IOCTL_MODE_SETCRTC, ...)
        //    - Tell display controller to scan out this framebuffer
        eglSwapBuffers(display, surface);

        // At this point:
        // - CPU continues immediately (doesn't wait for GPU)
        // - GPU is executing commands in parallel
        // - Display controller will show the result on next vsync

        if (frame % 60 == 0) {
            printf("Frame %d rendered\n", frame);
        }

        usleep(16666);  // ~60 FPS (16.666ms per frame)
    }

    printf("\n=== Cleanup ===\n");

    // Cleanup
    glDeleteBuffers(1, &vbo_pos);
    glDeleteBuffers(1, &vbo_color);
    glDeleteProgram(program);
    glDeleteShader(vs);
    glDeleteShader(fs);

    eglDestroySurface(display, surface);
    eglDestroyContext(display, context);
    eglTerminate(display);

    gbm_surface_destroy(gbm_surface);
    gbm_device_destroy(gbm);
    close(drm_fd);

    printf("✓ All resources cleaned up\n");

    return 0;
}
```

---

## Compilation and Running

```bash
# Install dependencies (Ubuntu/Debian)
sudo apt install libgbm-dev libegl1-mesa-dev libgles2-mesa-dev libdrm-dev

# Compile
gcc rotating_triangle.c -o rotating_triangle \
    -lGLESv2 -lEGL -lgbm -ldrm -lm

# Run (may need root for DRM access)
sudo ./rotating_triangle

# OR: Add yourself to 'video' group to avoid sudo
sudo usermod -a -G video $USER
# Log out and back in, then:
./rotating_triangle
```

---

## What Happens Internally

### Phase 1: Initialization (Lots of Syscalls)

```
Application starts
    ↓
open("/dev/dri/card0")                    ← SYSCALL: openat()
    ↓
ioctl(DRM_IOCTL_VERSION)                  ← SYSCALL: ioctl() - check driver
    ↓
ioctl(DRM_IOCTL_GET_CAP)                  ← SYSCALL: ioctl() - query capabilities
    ↓
ioctl(DRM_IOCTL_I915_GEM_CONTEXT_CREATE)  ← SYSCALL: ioctl() - create GPU context
    ↓
ioctl(DRM_IOCTL_I915_GEM_CREATE) × 3      ← SYSCALL: ioctl() - allocate buffers:
    - Command buffer (4-64 KB)                   • Command buffer
    - Vertex position buffer (36 bytes)          • Vertex buffers
    - Vertex color buffer (36 bytes)             • Shader bytecode
    ↓
mmap() × 2                                 ← SYSCALL: mmap() - map buffers to userspace
    ↓
Shader compilation (in userspace)          ← No syscalls! Pure CPU work
    ↓
ioctl(DRM_IOCTL_I915_GEM_PWRITE)          ← SYSCALL: ioctl() - upload shader bytecode
```

**Total syscalls in initialization: ~15-25**

### Phase 2: Rendering Loop (Almost NO Syscalls!)

```
For each frame:
    Build command buffer (userspace):
        glClear()          → Write to cmd_buf[0..15]    (no syscall)
        glUniform()        → Write to cmd_buf[16..79]   (no syscall)
        glBindBuffer()     → Write to cmd_buf[80..95]   (no syscall)
        glDrawArrays()     → Write to cmd_buf[96..127]  (no syscall)
        ... (all commands batched in memory)

    eglSwapBuffers():
        ioctl(DRM_IOCTL_I915_GEM_EXECBUFFER2)  ← SYSCALL! Submit batch
        ioctl(DRM_IOCTL_MODE_SETCRTC)          ← SYSCALL! Display frame
```

**Syscalls per frame: 2-3**
**GL calls per frame: ~20**
**Syscall ratio: ~0.1 syscalls per GL call (90% reduction!)**

---

## Syscall Trace Analysis

Let's trace what actually happens:

```bash
strace -e openat,ioctl,mmap,munmap -c ./rotating_triangle 2>&1 | head -30
```

**Example output:**

```
% time     seconds  usecs/call     calls    errors syscall
------ ----------- ----------- --------- --------- ----------------
 35.2    0.000422          42        10           openat
 28.4    0.000340          17        20           ioctl
 15.3    0.000183          61         3           mmap
  8.2    0.000098          49         2           munmap
 ...
```

### Breakdown by Operation

| Operation | Syscalls | What Happens |
|-----------|----------|--------------|
| **Initialization** | 15-25 | Open device, query caps, create context, allocate buffers |
| **Per Frame** | 2-3 | Submit command buffer, update display |
| **Cleanup** | 5-10 | Free buffers, close device |

### The Key ioctl() Calls

```c
// During initialization:
ioctl(fd, DRM_IOCTL_I915_GEM_CREATE, &create);
// Creates: struct {size=65536, handle=1} → Command buffer

ioctl(fd, DRM_IOCTL_I915_GEM_CREATE, &create);
// Creates: struct {size=36, handle=2} → Vertex buffer

ioctl(fd, DRM_IOCTL_I915_GEM_MMAP, &mmap_arg);
// Maps buffer to userspace address

// During rendering:
ioctl(fd, DRM_IOCTL_I915_GEM_EXECBUFFER2, &exec);
// Submits: {batch_buffer=handle_1, batch_len=128,
//           buffers=[handle_2, handle_3]}
```

---

## Command Buffer Contents

After all the `glXXX()` calls, the command buffer (in system RAM) looks like this:

```
Command Buffer (64 KB in system RAM):
┌──────────────────────────────────────────────────────────┐
│ Offset | Instruction             | Parameters            │
├──────────────────────────────────────────────────────────┤
│ 0x0000 | MI_BATCH_BUFFER_START   | shader_addr           │
│ 0x0004 | 3DSTATE_BINDING_TABLE   | table_ptr             │
│ 0x0008 | 3DSTATE_VS              | vertex_shader_ptr     │
│ 0x000C | 3DSTATE_PS              | fragment_shader_ptr   │
│ 0x0010 | 3DSTATE_VIEWPORT        | 0,0,1920,1080         │
│ 0x0014 | 3DSTATE_SCISSOR         | 0,0,1920,1080         │
│        |                         |                       │
│ 0x0020 | COLOR_BLT               | dest=framebuffer      │  ← glClear()
│ 0x0024 |   parameters            | color=0x00000000      │
│        |                         |                       │
│ 0x0030 | 3DSTATE_CONSTANT_VS     | uniform_buffer_ptr    │  ← glUniform()
│ 0x0034 |   rotation matrix       | [16 floats = 64 bytes]│
│        |                         |                       │
│ 0x0080 | 3DSTATE_VERTEX_BUFFERS  | buffer=vbo_pos        │  ← glBindBuffer()
│ 0x0084 |   format                | stride=12, format=R32G32B32_FLOAT │
│        |                         |                       │
│ 0x0090 | 3DSTATE_VERTEX_ELEMENTS | attrib[0]=position    │
│ 0x0094 |   attributes            | attrib[1]=color       │
│        |                         |                       │
│ 0x00A0 | 3DPRIMITIVE             | TRIANGLES             │  ← glDrawArrays()
│ 0x00A4 |   parameters            | vertex_count=3        │
│ 0x00A8 |                         | start_vertex=0        │
│        |                         |                       │
│ 0x00B0 | PIPE_CONTROL            | flush_caches          │
│ 0x00B4 | MI_BATCH_BUFFER_END     |                       │
└──────────────────────────────────────────────────────────┘

Total command buffer size: ~180 bytes (padded to 256 bytes)
```

**These are actual Intel GPU machine instructions!** (For Intel HD/Iris)

On AMD, it would be different (GCN/RDNA instructions).
On NVIDIA, different again (but nouveau driver follows similar pattern).

---

## Memory Layout

```
┌─────────────────────────────────────────────────────────────┐
│                    Physical Memory Layout                    │
└─────────────────────────────────────────────────────────────┘

System RAM (DDR4):
┌────────────────────────────────────────────────────────┐
│ 0x00000000 - 0x00100000: Kernel                       │
│ 0x00100000 - 0x40000000: Userspace processes          │
│     ...                                                │
│ 0x80000000 - 0x80010000: Command buffer (64 KB)       │  ← Mesa writes here
│ 0x80010000 - 0x80010024: Vertex buffer (36 bytes)     │  ← Our triangle data
│ 0x80010024 - 0x80010048: Color buffer (36 bytes)      │  ← Our color data
│ 0x80010048 - 0x80012000: Shader bytecode (~8 KB)      │  ← Compiled shaders
│     ...                                                │
└────────────────────────────────────────────────────────┘
          ↑                                    ↑
          │                                    │
    CPU can write                         GPU can read
    (it's mapped to                      (via DMA or
     our process)                         direct access)


VRAM / Stolen Memory (Integrated GPU):
┌────────────────────────────────────────────────────────┐
│ 0xC0000000 - 0xC0800000: Framebuffer (8 MB)           │  ← Where GPU renders
│                          1920×1080×4 bytes = 8,294,400│  ← Scanout to display
└────────────────────────────────────────────────────────┘
          ↑                                    ↑
          │                                    │
    GPU renders here                     Display controller
                                          reads from here
```

---

## The Complete Flow

### Step-by-Step: From API Call to Pixels

```
1. APPLICATION CALLS glDrawArrays(GL_TRIANGLES, 0, 3)
   ├─ Mesa (libGLESv2.so) intercepts the call
   ├─ Writes GPU command to command buffer:
   │     cmd_buf[offset++] = 0x7B000002;  // 3DPRIMITIVE opcode
   │     cmd_buf[offset++] = 0x00000003;  // Triangle count
   ├─ NO SYSCALL - just memory write
   └─ Returns immediately

2. APPLICATION CALLS eglSwapBuffers()
   ├─ Mesa finalizes command buffer
   ├─ Adds MI_BATCH_BUFFER_END
   ├─ Flushes CPU cache (if needed)
   │
   ├─ SYSCALL: ioctl(fd, DRM_IOCTL_I915_GEM_EXECBUFFER2, {
   │       .buffers_ptr = &buffer_list,        // Vertex buffers, shaders
   │       .buffer_count = 4,
   │       .batch_start_offset = 0,
   │       .batch_len = 256,
   │       .flags = I915_EXEC_RENDER,          // Use 3D engine
   │   })
   │
   └─ Kernel (i915 driver) processes this:

3. KERNEL RECEIVES EXECBUFFER IOCTL
   ├─ Validates command buffer (security check!)
   ├─ Resolves buffer handles to GPU addresses
   ├─ Adds to GPU submission queue
   ├─ Writes to GPU ring buffer:
   │     Ring[head++] = CMD_BUFFER_ADDR;
   │     Ring[head++] = 0x80000000;  // Our command buffer address
   │     MMIO_WRITE(RING_HEAD, head);  // Tell GPU to start
   └─ Returns to userspace (doesn't wait for GPU!)

4. GPU RECEIVES COMMAND
   ├─ Command processor reads from ring buffer
   ├─ Fetches command buffer from 0x80000000
   ├─ Parses commands sequentially:
   │
   ├─ 3DSTATE_VS: Load vertex shader
   │   └─ Fetch shader bytecode from 0x80010048
   │
   ├─ 3DSTATE_PS: Load fragment shader
   │   └─ Fetch shader bytecode from 0x80012000
   │
   ├─ 3DSTATE_VERTEX_BUFFERS: Bind vertex data
   │   └─ Fetch vertices from 0x80010000
   │
   ├─ 3DPRIMITIVE: Draw triangle!
   │   ├─ Vertex Shader runs 3 times (once per vertex):
   │   │   Input: position[0] = [0.0, 0.5, 0.0]
   │   │   Output: gl_Position = rotation * position
   │   │   Output: vColor = color[0] = [1.0, 0.0, 0.0]
   │   │
   │   ├─ Rasterizer: Convert triangle to pixels
   │   │   Determines which pixels are inside the triangle
   │   │   Generates ~100,000 fragments (for 1920×1080)
   │   │
   │   └─ Fragment Shader runs ~100,000 times:
   │       Input: vColor (interpolated from vertex colors)
   │       Output: gl_FragColor = [r, g, b, 1.0]
   │       Writes to framebuffer at 0xC0000000
   │
   └─ PIPE_CONTROL: Flush caches
       └─ Ensure framebuffer writes are visible

5. DISPLAY CONTROLLER (every 16.67ms @ 60Hz)
   ├─ Reads framebuffer from 0xC0000000
   ├─ Converts to display signals (HDMI/DisplayPort)
   └─ Sends to monitor
       └─ Monitor displays the rotating triangle!
```

### Timeline

```
Time (ms) | CPU                        | GPU                    | Display
──────────┼────────────────────────────┼────────────────────────┼─────────
0.0       | glClear()                  |                        |
0.1       | glUniform()                |                        |
0.2       | glDrawArrays()             |                        |
          | ↓ (write to cmd buffer)    |                        |
0.3       | eglSwapBuffers()           |                        |
          | ↓ ioctl(EXECBUFFER)        |                        |
0.4       | → returns immediately      | ← command received     |
0.5       | (CPU is free now!)         | Vertex shader × 3      |
0.6       | (can prepare next frame)   | Rasterization          |
0.7       |                            | Fragment shader × 100K |
1.0       |                            | ← rendering done       |
          |                            | (GPU waits for vsync)  |
16.67     |                            |                        | ← Vsync!
          |                            |                        | Scanout
          |                            |                        | Display!
```

**Key insight:** CPU and GPU work in parallel! CPU prepares next frame while GPU renders current frame.

---

## Performance Analysis

### Why This is Fast

**1. Batched Commands**
- 20+ GL calls per frame
- Only 2-3 syscalls per frame
- Command buffer size: ~256 bytes
- PCIe overhead: ~1 microsecond

**2. Parallel Execution**
```
CPU timeline:  ═════[Build Frame 1]═════╗ ═════[Build Frame 2]═════╗
                                        ↓                          ↓
                                     Submit                     Submit
GPU timeline:                          ╚═[Render Frame 1]════╗
                                                               ↓
Display:                                                    [Show Frame 1]
```

**3. Zero-Copy Memory**
- Vertex buffers in GPU-accessible memory
- No copying during rendering
- GPU DMAs directly from system RAM

### Syscall Overhead

```
Without batching (hypothetical):
    glClear()       → ioctl() → 500ns
    glUniform()     → ioctl() → 500ns
    glBindBuffer()  → ioctl() → 500ns
    glDrawArrays()  → ioctl() → 500ns
    × 20 calls/frame = 10,000ns = 10μs overhead

With batching (actual):
    [20 GL calls in userspace]
    eglSwapBuffers() → ioctl() → 500ns
    × 1 call/frame = 500ns overhead

Speedup: 20×
```

### Memory Bandwidth

```
Per frame data transfer:
- Command buffer: 256 bytes (read once by GPU)
- Vertex data: 72 bytes (read 3 times by GPU = 216 bytes)
- Uniform data: 64 bytes (read ~100K times = cached on GPU!)
- Framebuffer write: 8.3 MB (1920×1080×4 bytes)

Total CPU→GPU: ~8.3 MB/frame
At 60 FPS: ~500 MB/s
PCIe Gen3 x16 can do: 16,000 MB/s
Utilization: 3% (plenty of headroom!)
```

---

## Key Takeaways

1. **Command buffers eliminate syscall overhead**
   - Batch 20+ GL calls → 1 syscall
   - Commands written to userspace memory (fast!)
   - Submitted to GPU with single ioctl()

2. **GPU execution is asynchronous**
   - CPU doesn't wait for GPU to finish
   - Parallel execution: CPU builds next frame while GPU renders
   - Vsync coordinates display updates

3. **Modern GPUs are programmable**
   - Vertex shader: Transform vertices (math operations)
   - Fragment shader: Calculate pixel colors (runs millions of times!)
   - All code runs on GPU, not CPU

4. **Memory is carefully managed**
   - Command buffers: System RAM (CPU writes, GPU reads once)
   - Vertex data: System RAM or VRAM (uploaded once, read many times)
   - Framebuffer: VRAM or stolen memory (GPU writes, display reads)

5. **The Linux graphics stack is efficient**
   - Userspace: Mesa (libGL/libEGL) builds commands
   - Kernel: DRM validates and submits to GPU
   - Hardware: GPU executes asynchronously

**This is why modern games can render millions of triangles at 60+ FPS!**