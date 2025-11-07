// simple_video_player.c
// Minimal video player using FFmpeg + Direct DRM/KMS
// Decodes video with CPU, displays via framebuffer - educational, not optimized!
//
// Compile:
//   gcc -o simple_video_player simple_video_player.c \
//       -lavformat -lavcodec -lavutil -lswscale -ldrm -lm
//
// Run:
//   sudo ./simple_video_player video.mp4

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <time.h>
#include <xf86drm.h>
#include <xf86drmMode.h>

// FFmpeg headers
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libavutil/imgutils.h>
#include <libswscale/swscale.h>

// =============================================================================
// GLOBAL STATE
// =============================================================================

// DRM/Display state
int drm_fd = -1;
uint32_t *framebuffer = NULL;
uint32_t fb_handle = 0;
uint32_t fb_id = 0;
uint64_t fb_size = 0;
int screen_width = 1920;
int screen_height = 1080;

// FFmpeg state
AVFormatContext *format_ctx = NULL;
AVCodecContext *codec_ctx = NULL;
AVFrame *frame = NULL;
AVFrame *frame_rgb = NULL;
AVPacket *packet = NULL;
struct SwsContext *sws_ctx = NULL;
int video_stream_index = -1;

// Timing
double video_start_time = 0.0;

// =============================================================================
// UTILITY FUNCTIONS
// =============================================================================

// Get current time in seconds
double get_time() {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec / 1e9;
}

// Sleep for a specific duration
void sleep_ms(int milliseconds) {
    usleep(milliseconds * 1000);
}

// =============================================================================
// DRM/KMS SETUP (Similar to simple_triangle.c)
// =============================================================================

int setup_drm() {
    printf("=== Setting up DRM/KMS ===\n");

    // Step 1: Open DRM device
    printf("Opening DRM device...\n");
    drm_fd = open("/dev/dri/card1", O_RDWR);
    if (drm_fd < 0) {
        perror("Cannot open /dev/dri/card1");
        return -1;
    }
    printf("✓ DRM device opened (fd=%d)\n", drm_fd);

    // Step 2: Get display resources
    drmModeRes *resources = drmModeGetResources(drm_fd);
    if (!resources) {
        perror("Cannot get DRM resources");
        return -1;
    }
    printf("✓ Found %d connectors\n", resources->count_connectors);

    // Step 3: Find connected display
    drmModeConnector *connector = NULL;
    for (int i = 0; i < resources->count_connectors; i++) {
        connector = drmModeGetConnector(drm_fd, resources->connectors[i]);
        if (connector->connection == DRM_MODE_CONNECTED) {
            printf("✓ Found connected display\n");
            break;
        }
        drmModeFreeConnector(connector);
        connector = NULL;
    }

    if (!connector) {
        fprintf(stderr, "No connected display\n");
        return -1;
    }

    // Get display resolution
    drmModeModeInfo mode = connector->modes[0];
    screen_width = mode.hdisplay;
    screen_height = mode.vdisplay;
    printf("✓ Display: %dx%d @ %dHz\n", screen_width, screen_height, mode.vrefresh);

    // Step 4: Create framebuffer
    struct drm_mode_create_dumb create_dumb = {
        .width = screen_width,
        .height = screen_height,
        .bpp = 32,
    };

    if (drmIoctl(drm_fd, DRM_IOCTL_MODE_CREATE_DUMB, &create_dumb) < 0) {
        perror("Cannot create dumb buffer");
        return -1;
    }

    fb_handle = create_dumb.handle;
    fb_size = create_dumb.size;
    printf("✓ Framebuffer created: %.2f MB\n", fb_size / 1024.0 / 1024.0);

    // Step 5: Add framebuffer
    uint32_t pitch = create_dumb.pitch;
    if (drmModeAddFB(drm_fd, screen_width, screen_height, 24, 32,
                     pitch, fb_handle, &fb_id)) {
        perror("Cannot add framebuffer");
        return -1;
    }
    printf("✓ Framebuffer registered (fb_id=%u)\n", fb_id);

    // Step 6: Map framebuffer to userspace
    struct drm_mode_map_dumb map_dumb = {.handle = fb_handle};
    if (drmIoctl(drm_fd, DRM_IOCTL_MODE_MAP_DUMB, &map_dumb)) {
        perror("Cannot get mmap offset");
        return -1;
    }

    framebuffer = mmap(NULL, fb_size, PROT_READ | PROT_WRITE, MAP_SHARED,
                       drm_fd, map_dumb.offset);
    if (framebuffer == MAP_FAILED) {
        perror("Cannot mmap framebuffer");
        return -1;
    }
    printf("✓ Framebuffer mapped at %p\n", framebuffer);

    // Step 7: Set display mode
    uint32_t crtc_id = resources->crtcs[0];
    if (drmModeSetCrtc(drm_fd, crtc_id, fb_id, 0, 0,
                       &connector->connector_id, 1, &mode)) {
        perror("Cannot set CRTC");
        return -1;
    }
    printf("✓ Display mode set - ready to render!\n\n");

    drmModeFreeConnector(connector);
    drmModeFreeResources(resources);
    return 0;
}

void cleanup_drm() {
    if (framebuffer) munmap(framebuffer, fb_size);
    if (fb_id) drmModeRmFB(drm_fd, fb_id);
    if (drm_fd >= 0) close(drm_fd);
}

// =============================================================================
// FFMPEG VIDEO DECODING SETUP
// =============================================================================

int setup_ffmpeg(const char *filename) {
    printf("=== Setting up FFmpeg ===\n");

    // Step 1: Open video file
    printf("Opening video file: %s\n", filename);
    if (avformat_open_input(&format_ctx, filename, NULL, NULL) < 0) {
        fprintf(stderr, "Cannot open video file\n");
        return -1;
    }
    printf("✓ Video file opened\n");

    // Step 2: Read stream info
    if (avformat_find_stream_info(format_ctx, NULL) < 0) {
        fprintf(stderr, "Cannot find stream info\n");
        return -1;
    }

    // Step 3: Find video stream
    video_stream_index = -1;
    for (int i = 0; i < format_ctx->nb_streams; i++) {
        if (format_ctx->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_VIDEO) {
            video_stream_index = i;
            break;
        }
    }

    if (video_stream_index == -1) {
        fprintf(stderr, "No video stream found\n");
        return -1;
    }
    printf("✓ Found video stream #%d\n", video_stream_index);

    // Step 4: Get codec parameters
    AVCodecParameters *codecpar = format_ctx->streams[video_stream_index]->codecpar;
    printf("  - Codec: %s\n", avcodec_get_name(codecpar->codec_id));
    printf("  - Resolution: %dx%d\n", codecpar->width, codecpar->height);
    printf("  - FPS: %.2f\n",
           av_q2d(format_ctx->streams[video_stream_index]->r_frame_rate));

    // Step 5: Find decoder
    const AVCodec *codec = avcodec_find_decoder(codecpar->codec_id);
    if (!codec) {
        fprintf(stderr, "Codec not supported\n");
        return -1;
    }
    printf("✓ Decoder found: %s\n", codec->long_name);

    // Step 6: Allocate codec context
    codec_ctx = avcodec_alloc_context3(codec);
    if (!codec_ctx) {
        fprintf(stderr, "Cannot allocate codec context\n");
        return -1;
    }

    if (avcodec_parameters_to_context(codec_ctx, codecpar) < 0) {
        fprintf(stderr, "Cannot copy codec parameters\n");
        return -1;
    }

    // Step 7: Open codec
    if (avcodec_open2(codec_ctx, codec, NULL) < 0) {
        fprintf(stderr, "Cannot open codec\n");
        return -1;
    }
    printf("✓ Codec opened\n");

    // Step 8: Allocate frames
    frame = av_frame_alloc();
    frame_rgb = av_frame_alloc();
    packet = av_packet_alloc();

    if (!frame || !frame_rgb || !packet) {
        fprintf(stderr, "Cannot allocate frames\n");
        return -1;
    }

    // Step 9: Allocate buffer for RGB frame
    int num_bytes = av_image_get_buffer_size(AV_PIX_FMT_RGB32,
                                             screen_width, screen_height, 1);
    uint8_t *buffer = (uint8_t *)av_malloc(num_bytes);
    av_image_fill_arrays(frame_rgb->data, frame_rgb->linesize, buffer,
                        AV_PIX_FMT_RGB32, screen_width, screen_height, 1);

    // Step 10: Create scaler context (converts YUV -> RGB, scales to screen size)
    sws_ctx = sws_getContext(
        codec_ctx->width, codec_ctx->height, codec_ctx->pix_fmt,
        screen_width, screen_height, AV_PIX_FMT_RGB32,
        SWS_BILINEAR, NULL, NULL, NULL);

    if (!sws_ctx) {
        fprintf(stderr, "Cannot create scaler\n");
        return -1;
    }
    printf("✓ Scaler created (will scale %dx%d -> %dx%d)\n",
           codec_ctx->width, codec_ctx->height, screen_width, screen_height);

    printf("✓ FFmpeg setup complete!\n\n");
    return 0;
}

void cleanup_ffmpeg() {
    if (sws_ctx) sws_freeContext(sws_ctx);
    if (frame) av_frame_free(&frame);
    if (frame_rgb) av_frame_free(&frame_rgb);
    if (packet) av_packet_free(&packet);
    if (codec_ctx) avcodec_free_context(&codec_ctx);
    if (format_ctx) avformat_close_input(&format_ctx);
}

// =============================================================================
// VIDEO RENDERING
// =============================================================================

void render_frame_to_framebuffer(AVFrame *rgb_frame) {
    // Copy RGB frame to framebuffer
    // RGB frame is in RGB32 format (XRGB8888), same as framebuffer
    uint32_t *src = (uint32_t *)rgb_frame->data[0];
    int src_stride = rgb_frame->linesize[0] / 4;  // Convert bytes to pixels

    // Center the video on screen if it's smaller
    int offset_x = (screen_width - codec_ctx->width) / 2;
    int offset_y = (screen_height - codec_ctx->height) / 2;

    // Copy line by line
    for (int y = 0; y < screen_height && y < codec_ctx->height; y++) {
        for (int x = 0; x < screen_width && x < codec_ctx->width; x++) {
            int dst_idx = (y + offset_y) * screen_width + (x + offset_x);
            int src_idx = y * src_stride + x;

            if (dst_idx >= 0 && dst_idx < screen_width * screen_height) {
                framebuffer[dst_idx] = src[src_idx];
            }
        }
    }
}

// =============================================================================
// MAIN PLAYBACK LOOP
// =============================================================================

int play_video() {
    printf("=== Starting Playback ===\n");

    video_start_time = get_time();
    int frame_count = 0;

    AVRational time_base = format_ctx->streams[video_stream_index]->time_base;
    AVRational frame_rate = format_ctx->streams[video_stream_index]->r_frame_rate;
    double frame_duration = av_q2d(av_inv_q(frame_rate));  // Seconds per frame

    printf("Frame duration: %.3f ms (%.2f fps)\n",
           frame_duration * 1000, 1.0 / frame_duration);
    printf("Starting playback...\n\n");

    while (av_read_frame(format_ctx, packet) >= 0) {
        // Only process video packets
        if (packet->stream_index != video_stream_index) {
            av_packet_unref(packet);
            continue;
        }

        // Send packet to decoder
        if (avcodec_send_packet(codec_ctx, packet) < 0) {
            fprintf(stderr, "Error sending packet\n");
            av_packet_unref(packet);
            continue;
        }

        // Receive decoded frame
        while (avcodec_receive_frame(codec_ctx, frame) == 0) {
            frame_count++;

            // Calculate presentation time
            double pts = frame->pts * av_q2d(time_base);
            double current_time = get_time() - video_start_time;

            // Wait until it's time to display this frame
            double sleep_time = pts - current_time;
            if (sleep_time > 0) {
                sleep_ms((int)(sleep_time * 1000));
            }

            // Convert frame from YUV to RGB and scale to screen size
            sws_scale(sws_ctx,
                     (const uint8_t * const*)frame->data,
                     frame->linesize,
                     0,
                     codec_ctx->height,
                     frame_rgb->data,
                     frame_rgb->linesize);

            // Render to framebuffer (direct memory write to display!)
            render_frame_to_framebuffer(frame_rgb);

            // Progress indicator
            if (frame_count % 60 == 0) {
                printf("Frame %d rendered (PTS: %.2fs, drift: %.3fms)\n",
                       frame_count, pts, (current_time - pts) * 1000);
            }
        }

        av_packet_unref(packet);
    }

    printf("\n✓ Playback complete (%d frames)\n", frame_count);
    return 0;
}

// =============================================================================
// MAIN
// =============================================================================

int main(int argc, char *argv[]) {
    if (argc < 2) {
        printf("Usage: %s <video_file>\n", argv[0]);
        printf("\nExample:\n");
        printf("  sudo %s video.mp4\n", argv[0]);
        printf("\nNote: Requires root or video group for DRM access\n");
        return 1;
    }

    printf("=== Simple Video Player ===\n");
    printf("Educational video player using FFmpeg + Direct DRM\n\n");

    // Setup DRM display
    if (setup_drm() < 0) {
        fprintf(stderr, "DRM setup failed\n");
        return 1;
    }

    // Setup FFmpeg decoder
    if (setup_ffmpeg(argv[1]) < 0) {
        fprintf(stderr, "FFmpeg setup failed\n");
        cleanup_drm();
        return 1;
    }

    // Play the video
    play_video();

    // Cleanup
    printf("\n=== Cleanup ===\n");
    cleanup_ffmpeg();
    cleanup_drm();
    printf("✓ All resources cleaned up\n");

    return 0;
}