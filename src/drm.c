#include "drm.h"
#include "config.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/mman.h>
#include <sys/ioctl.h>
#include <errno.h>
#include <stddef.h>
#include <math.h>

/* Define deprecated types that libdrm headers may reference but don't define */
typedef unsigned int drm_handle_t;
typedef unsigned int drm_magic_t;
typedef unsigned int drm_context_t;
typedef unsigned int drm_drawable_t;

/* Include drm_mode.h first to get drm_drawable_info_type_t definition */
#include <libdrm/drm_mode.h>

/* Now include libdrm headers */
#include <xf86drm.h>
#include <xf86drmMode.h>

/* Helper: find connector - prefers DSI (Waveshare 8.8" panel), falls back to HDMI */
static int find_display_connector(int fd, uint32_t *connector_id) {
    drmModeRes *res = drmModeGetResources(fd);
    if (!res) {
        fprintf(stderr, "drmModeGetResources failed\n");
        return -1;
    }

    printf("[DRM] Found %d connectors\n", res->count_connectors);

    uint32_t dsi_id = 0, hdmi_id = 0;

    for (int i = 0; i < res->count_connectors; i++) {
        drmModeConnector *conn = drmModeGetConnector(fd, res->connectors[i]);
        if (!conn) continue;

        printf("[DRM] Connector %d: type=%d, status=%d\n", i, conn->connector_type, conn->connection);

        if (conn->connection == DRM_MODE_CONNECTED) {
            if (conn->connector_type == DRM_MODE_CONNECTOR_DSI && !dsi_id) {
                dsi_id = res->connectors[i];
                printf("[DRM] Found connected DSI connector\n");
            } else if ((conn->connector_type == DRM_MODE_CONNECTOR_HDMIA ||
                        conn->connector_type == DRM_MODE_CONNECTOR_HDMIB) && !hdmi_id) {
                hdmi_id = res->connectors[i];
                printf("[DRM] Found connected HDMI connector\n");
            }
        }
        drmModeFreeConnector(conn);
    }

    drmModeFreeResources(res);

    /* Prefer DSI (Waveshare panel); fall back to HDMI */
    if (dsi_id) { *connector_id = dsi_id; return 0; }
    if (hdmi_id) { *connector_id = hdmi_id; return 0; }

    fprintf(stderr, "[DRM] No connected DSI or HDMI connector found\n");
    return -1;
}

static int has_exact_mode(drmModeConnector *conn, uint32_t target_width, uint32_t target_height, float target_refresh) {
    for (int i = 0; i < conn->count_modes; i++) {
        drmModeModeInfo *mode = &conn->modes[i];
        float actual_refresh = (float)mode->clock * 1000.0f / ((float)mode->htotal * (float)mode->vtotal);
        if (mode->hdisplay == target_width &&
            mode->vdisplay == target_height &&
            fabsf(actual_refresh - target_refresh) < 0.02f) {
            return 1;
        }
    }
    return 0;
}

static void make_cea_861_uhd50_mode(drmModeModeInfo *mode) {
    memset(mode, 0, sizeof(*mode));

    mode->clock = 594000;
    mode->hdisplay = 3840;
    mode->hsync_start = 4896;
    mode->hsync_end = 4984;
    mode->htotal = 5280;
    mode->hskew = 0;

    mode->vdisplay = 2160;
    mode->vsync_start = 2168;
    mode->vsync_end = 2178;
    mode->vtotal = 2250;
    mode->vscan = 0;

    mode->vrefresh = 50;
    mode->flags = DRM_MODE_FLAG_PHSYNC | DRM_MODE_FLAG_PVSYNC;
    mode->type = DRM_MODE_TYPE_DRIVER;
    strncpy(mode->name, "3840x2160", sizeof(mode->name) - 1);
}

/* Find best matching mode for target resolution and refresh rate */
static drmModeModeInfo *find_mode(drmModeConnector *conn, uint32_t target_width, uint32_t target_height, float target_refresh) {
    printf("[DRM] Looking for mode: %ux%u @ %.2f Hz. Available modes:\n", target_width, target_height, target_refresh);
    fflush(stdout);

    drmModeModeInfo *best_mode = NULL;
    float best_diff = 1000.0f;

    for (int i = 0; i < conn->count_modes; i++) {
        drmModeModeInfo *mode = &conn->modes[i];
        float actual_refresh = (float)mode->clock * 1000.0f / ((float)mode->htotal * (float)mode->vtotal);
        int is_target_resolution = (mode->hdisplay == target_width && mode->vdisplay == target_height);
        float diff = fabsf(actual_refresh - target_refresh);
        int is_exact_refresh = (diff < 0.02f);

        printf("[DRM]   Mode %d: %ux%u @ %.2f Hz (checking: res=%d, hz=%d)\n",
               i, mode->hdisplay, mode->vdisplay, actual_refresh,
               is_target_resolution, is_exact_refresh);
        fflush(stdout);

        if (is_target_resolution && is_exact_refresh) {
            printf("[DRM] Found exact match at mode %d: %ux%u @ %.2f Hz!\n",
                   i, mode->hdisplay, mode->vdisplay, actual_refresh);
            fflush(stdout);
            return mode;
        }

        if (is_target_resolution && diff < best_diff) {
            best_diff = diff;
            best_mode = mode;
        }
    }

    printf("[DRM] No exact match found, trying closest refresh fallback\n");
    fflush(stdout);

    if (best_mode) {
        float best_refresh = (float)best_mode->clock * 1000.0f / ((float)best_mode->htotal * (float)best_mode->vtotal);
        printf("[DRM] Fallback: using closest mode %ux%u @ %.2f Hz (requested %.2f Hz)\n",
               best_mode->hdisplay, best_mode->vdisplay, best_refresh, target_refresh);
        fflush(stdout);
        return best_mode;
    }
    
    /* Last resort: use first mode */
    if (conn->count_modes > 0) {
        printf("[DRM] No resolution match, using first available mode: %ux%u @ %u Hz\n",
               conn->modes[0].hdisplay, conn->modes[0].vdisplay, conn->modes[0].vrefresh);
        fflush(stdout);
        return &conn->modes[0];
    }
    printf("[DRM] No modes available at all!\n");
    fflush(stdout);
    return NULL;
}

/* Allocate a dumb buffer (CPU-rendered) */
static int allocate_dumb_buffer(int fd, ltc_drm_framebuffer_t *fb, uint32_t width, uint32_t height) {
    struct drm_mode_create_dumb create = {
        .width = width,
        .height = height,
        .bpp = 32,
    };

    if (ioctl(fd, DRM_IOCTL_MODE_CREATE_DUMB, &create)) {
        fprintf(stderr, "Failed to create dumb buffer\n");
        return -1;
    }

    fb->pitch = create.pitch;
    fb->buffer_size = create.size;

    /* Map buffer to userspace */
    struct drm_mode_map_dumb map = {
        .handle = create.handle,
    };

    if (ioctl(fd, DRM_IOCTL_MODE_MAP_DUMB, &map)) {
        fprintf(stderr, "Failed to map dumb buffer\n");
        return -1;
    }

    fb->buffer = mmap(NULL, fb->buffer_size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, map.offset);
    if (fb->buffer == MAP_FAILED) {
        fprintf(stderr, "Failed to mmap dumb buffer\n");
        return -1;
    }

    /* Create framebuffer object */
    uint32_t handles[4] = {create.handle, 0, 0, 0};
    uint32_t pitches[4] = {fb->pitch, 0, 0, 0};
    uint32_t offsets[4] = {0, 0, 0, 0};

    if (drmModeAddFB2(fd, width, height, DRM_FORMAT_ARGB8888,
                      handles, pitches, offsets, &fb->fb_id, 0)) {
        fprintf(stderr, "Failed to add FB\n");
        munmap(fb->buffer, fb->buffer_size);
        return -1;
    }

    fb->hdisplay = width;
    fb->vdisplay = height;

    return 0;
}

int drm_init(ltc_drm_context_t *ctx, uint32_t target_width, uint32_t target_height, float target_vrefresh) {
    printf("[DRM] drm_init called with target=%ux%u@%.2f\n", target_width, target_height, target_vrefresh);
    fflush(stdout);
    memset(ctx, 0, sizeof(*ctx));
    ctx->fd = -1;

    /* Open DRM device - try /dev/dri/card0, card1, etc. with resource validation */
    for (int i = 0; i < 16; i++) {
        char path[64];
        snprintf(path, sizeof(path), "/dev/dri/card%d", i);
        
        int fd = open(path, O_RDWR);
        if (fd < 0) {
            continue;  /* Device doesn't exist, try next */
        }

        /* Test if this device has DRM resources */
        drmModeRes *res = drmModeGetResources(fd);
        if (!res) {
            close(fd);
            continue;  /* Device has no mode resources, try next */
        }

        drmModeFreeResources(res);
        ctx->fd = fd;
        break;
    }

    if (ctx->fd < 0) {
        fprintf(stderr, "Failed to open DRM device with resources\n");
        return -1;
    }

    printf("[DRM] Opened device (fd=%d)\n", ctx->fd);

    /* Find display connector (DSI preferred, HDMI fallback) */
    if (find_display_connector(ctx->fd, &ctx->connector_id)) {
        fprintf(stderr, "No display connector found\n");
        close(ctx->fd);
        return -1;
    }

    /* Get connector info */
    drmModeConnector *conn = drmModeGetConnector(ctx->fd, ctx->connector_id);
    if (!conn) {
        fprintf(stderr, "Failed to get connector\n");
        close(ctx->fd);
        return -1;
    }

    printf("[DRM] Got connector with %d modes. Searching for target=%.2f Hz...\n", conn->count_modes, target_vrefresh);
    fflush(stdout);

    /* Find best mode */
    drmModeModeInfo *mode = find_mode(conn, target_width, target_height, target_vrefresh);
    fflush(stdout);

    if (!mode) {
        /* No exact match - use first available mode */
        if (conn->count_modes > 0) {
            mode = &conn->modes[0];
            printf("[DRM] No mode matching %.2f Hz, using first available: %ux%u@%u\n",
                   target_vrefresh, mode->hdisplay, mode->vdisplay, mode->vrefresh);
        } else {
            fprintf(stderr, "No modes available on connector\n");
            drmModeFreeConnector(conn);
            close(ctx->fd);
            return -1;
        }
    }

    int request_uhd50 = (target_width == 3840 && target_height == 2160 && fabsf(target_vrefresh - 50.0f) < 0.02f);
    int have_exact_uhd50 = has_exact_mode(conn, target_width, target_height, target_vrefresh);
    int forced_uhd50 = 0;

    drmModeModeInfo target_mode = *mode;
    if (request_uhd50 && !have_exact_uhd50) {
        printf("[DRM] No native 3840x2160@50.00 found; trying forced CEA-861 timing (VIC 96)\n");
        fflush(stdout);
        make_cea_861_uhd50_mode(&target_mode);
        forced_uhd50 = 1;
    }

    ctx->mode_vrefresh = target_mode.vrefresh;
    ctx->mode_refresh_hz = (float)target_mode.clock * 1000.0f / ((float)target_mode.htotal * (float)target_mode.vtotal);
    
    /* Allocate and store mode for later use in page flips (malloc unlikely to fail here) */
    ctx->current_mode = malloc(sizeof(drmModeModeInfo));
    if (ctx->current_mode) {
        memcpy(ctx->current_mode, &target_mode, sizeof(drmModeModeInfo));
    }
    printf("[DRM] Using mode: %ux%u@%u Hz\n", target_mode.hdisplay, target_mode.vdisplay, target_mode.vrefresh);

    /* Find CRTC */
    drmModeRes *res = drmModeGetResources(ctx->fd);
    if (!res) {
        fprintf(stderr, "drmModeGetResources failed\n");
        drmModeFreeConnector(conn);
        close(ctx->fd);
        return -1;
    }

    printf("[DRM] Found %d CRTCs\n", res->count_crtcs);

    /* Allocate framebuffers BEFORE trying to set CRTC */
    if (allocate_dumb_buffer(ctx->fd, &ctx->front, target_mode.hdisplay, target_mode.vdisplay)) {
        fprintf(stderr, "Failed to allocate front buffer\n");
        drmModeFreeResources(res);
        drmModeFreeConnector(conn);
        close(ctx->fd);
        return -1;
    }

    if (allocate_dumb_buffer(ctx->fd, &ctx->back, target_mode.hdisplay, target_mode.vdisplay)) {
        fprintf(stderr, "Failed to allocate back buffer\n");
        drmModeFreeResources(res);
        drmModeFreeConnector(conn);
        close(ctx->fd);
        return -1;
    }

    printf("[DRM] Buffers allocated: front FB=%u, back FB=%u\n", ctx->front.fb_id, ctx->back.fb_id);
    
    /* Try each CRTC until one works */
    int crtc_found = 0;
    for (int i = 0; i < res->count_crtcs && !crtc_found; i++) {
        ctx->crtc_id = res->crtcs[i];
        ctx->crtc_index = i;
        
        printf("[DRM] Trying CRTC %d (ID %u) with FB %u...\n", i, ctx->crtc_id, ctx->front.fb_id);
        
        if (drmModeSetCrtc(ctx->fd, ctx->crtc_id, ctx->front.fb_id, 0, 0,
                           &ctx->connector_id, 1, &target_mode) == 0) {
            printf("[DRM] CRTC %d configured successfully\n", i);
            crtc_found = 1;
            break;
        } else {
            printf("[DRM] CRTC %d failed: %s (errno %d)\n", i, strerror(errno), errno);
        }
    }

    if (!crtc_found && forced_uhd50) {
        printf("[DRM] Forced CEA-861 4K50 rejected by sink/driver, falling back to closest native mode\n");
        fflush(stdout);

        target_mode = *mode;
        ctx->mode_vrefresh = target_mode.vrefresh;
        ctx->mode_refresh_hz = (float)target_mode.clock * 1000.0f / ((float)target_mode.htotal * (float)target_mode.vtotal);
        if (ctx->current_mode) {
            memcpy(ctx->current_mode, &target_mode, sizeof(drmModeModeInfo));
        }

        for (int i = 0; i < res->count_crtcs && !crtc_found; i++) {
            ctx->crtc_id = res->crtcs[i];
            ctx->crtc_index = i;

            printf("[DRM] Retry CRTC %d (ID %u) with native mode %ux%u@%u...\n",
                   i, ctx->crtc_id, target_mode.hdisplay, target_mode.vdisplay, target_mode.vrefresh);

            if (drmModeSetCrtc(ctx->fd, ctx->crtc_id, ctx->front.fb_id, 0, 0,
                               &ctx->connector_id, 1, &target_mode) == 0) {
                printf("[DRM] Native fallback mode accepted on CRTC %d\n", i);
                crtc_found = 1;
                break;
            } else {
                printf("[DRM] Retry CRTC %d failed: %s (errno %d)\n", i, strerror(errno), errno);
            }
        }
    }

    if (!crtc_found) {
        fprintf(stderr, "Failed to configure any CRTC\n");
        drmModeFreeResources(res);
        drmModeFreeConnector(conn);
        close(ctx->fd);
        return -1;
    }

    printf("[DRM] Initialized %ux%u @ %u Hz\n", target_mode.hdisplay, target_mode.vdisplay, ctx->mode_vrefresh);

    drmModeFreeResources(res);
    drmModeFreeConnector(conn);

    return 0;
}

uint8_t *drm_get_back_buffer(ltc_drm_context_t *ctx) {
    return ctx->back.buffer;
}

int drm_page_flip_sync(ltc_drm_context_t *ctx) {
    /* Page flip: update CRTC with new framebuffer using stored mode */
    if (!ctx || ctx->fd < 0) {
        fprintf(stderr, "[DRM] Page flip: invalid DRM context or fd=%d\n", ctx ? ctx->fd : -9999);
        return -1;
    }
    if (!ctx->current_mode) {
        fprintf(stderr, "[DRM] Page flip: current_mode not initialized\n");
        return -1;
    }
    
    drmModeModeInfo *mode = (drmModeModeInfo *)ctx->current_mode;
    
    int result = drmModeSetCrtc(ctx->fd, ctx->crtc_id, ctx->back.fb_id, 0, 0,
                                &ctx->connector_id, 1, mode);
    if (result != 0) {
        fprintf(stderr, "[DRM] drmModeSetCrtc (flip) failed: %s (errno=%d, fd=%d, crtc=%u, fb=%u)\n",
                strerror(errno), errno, ctx->fd, ctx->crtc_id, ctx->back.fb_id);
        return -1;
    }

    /* Swap front/back */
    ltc_drm_framebuffer_t tmp = ctx->front;
    ctx->front = ctx->back;
    ctx->back = tmp;

    return 0;
}

uint32_t drm_detect_vrefresh(ltc_drm_context_t *ctx) {
    return ctx->mode_vrefresh;
}

void drm_cleanup(ltc_drm_context_t *ctx) {
    if (ctx->current_mode) {
        free(ctx->current_mode);
        ctx->current_mode = NULL;
    }
    if (ctx->back.buffer) {
        munmap(ctx->back.buffer, ctx->back.buffer_size);
    }
    if (ctx->front.buffer) {
        munmap(ctx->front.buffer, ctx->front.buffer_size);
    }
    if (ctx->fd >= 0) {
        close(ctx->fd);
        ctx->fd = -1;
    }
}
