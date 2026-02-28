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

/* Helper: find connector by type (HDMI preferred) */
static int find_hdmi_connector(int fd, uint32_t *connector_id) {
    drmModeRes *res = drmModeGetResources(fd);
    if (!res) {
        fprintf(stderr, "drmModeGetResources failed\n");
        return -1;
    }

    printf("[DRM] Found %d connectors\n", res->count_connectors);

    for (int i = 0; i < res->count_connectors; i++) {
        drmModeConnector *conn = drmModeGetConnector(fd, res->connectors[i]);
        if (!conn) continue;

        printf("[DRM] Connector %d: type=%d, status=%d\n", i, conn->connector_type, conn->connection);

        /* Look for HDMI connector that is connected */
        if ((conn->connector_type == DRM_MODE_CONNECTOR_HDMIA ||
             conn->connector_type == DRM_MODE_CONNECTOR_HDMIB) &&
            conn->connection == DRM_MODE_CONNECTED) {
            printf("[DRM] Found connected HDMI connector\n");
            *connector_id = res->connectors[i];
            drmModeFreeConnector(conn);
            drmModeFreeResources(res);
            return 0;
        }
        drmModeFreeConnector(conn);
    }

    printf("[DRM] No connected HDMI connector found\n");
    drmModeFreeResources(res);
    return -1;
}

/* Find best matching mode for target refresh rate */
static drmModeModeInfo *find_mode_by_refresh(drmModeConnector *conn, uint32_t target_refresh) {
    for (int i = 0; i < conn->count_modes; i++) {
        drmModeModeInfo *mode = &conn->modes[i];
        if (mode->vrefresh == target_refresh &&
            mode->hdisplay == DISPLAY_WIDTH &&
            mode->vdisplay == DISPLAY_HEIGHT) {
            return mode;
        }
    }
    /* Fallback to first mode if exact match not found */
    return conn->count_modes > 0 ? &conn->modes[0] : NULL;
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

int drm_init(ltc_drm_context_t *ctx, uint32_t target_vrefresh) {
    memset(ctx, 0, sizeof(*ctx));

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

    /* Find HDMI connector */
    if (find_hdmi_connector(ctx->fd, &ctx->connector_id)) {
        fprintf(stderr, "No HDMI connector found\n");
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

    /* Find best mode */
    drmModeModeInfo *mode = find_mode_by_refresh(conn, target_vrefresh);
    if (!mode) {
        /* No exact match - use first available mode */
        if (conn->count_modes > 0) {
            mode = &conn->modes[0];
            printf("[DRM] No mode matching %u Hz, using first available: %ux%u@%u\n",
                   target_vrefresh, mode->hdisplay, mode->vdisplay, mode->vrefresh);
        } else {
            fprintf(stderr, "No modes available on connector\n");
            drmModeFreeConnector(conn);
            close(ctx->fd);
            return -1;
        }
    }

    drmModeModeInfo target_mode = *mode;
    ctx->mode_vrefresh = mode->vrefresh;
    
    /* Allocate and store mode for later use in page flips (malloc unlikely to fail here) */
    ctx->current_mode = malloc(sizeof(drmModeModeInfo));
    if (ctx->current_mode) {
        memcpy(ctx->current_mode, mode, sizeof(drmModeModeInfo));
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
    if (allocate_dumb_buffer(ctx->fd, &ctx->front, DISPLAY_WIDTH, DISPLAY_HEIGHT)) {
        fprintf(stderr, "Failed to allocate front buffer\n");
        drmModeFreeResources(res);
        drmModeFreeConnector(conn);
        close(ctx->fd);
        return -1;
    }

    if (allocate_dumb_buffer(ctx->fd, &ctx->back, DISPLAY_WIDTH, DISPLAY_HEIGHT)) {
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

    if (!crtc_found) {
        fprintf(stderr, "Failed to configure any CRTC\n");
        drmModeFreeResources(res);
        drmModeFreeConnector(conn);
        close(ctx->fd);
        return -1;
    }

    printf("[DRM] Initialized %ux%u @ %u Hz\n", DISPLAY_WIDTH, DISPLAY_HEIGHT, ctx->mode_vrefresh);

    drmModeFreeResources(res);
    drmModeFreeConnector(conn);

    return 0;
}

uint8_t *drm_get_back_buffer(ltc_drm_context_t *ctx) {
    return ctx->back.buffer;
}

int drm_page_flip_sync(ltc_drm_context_t *ctx) {
    /* Page flip: update CRTC with new framebuffer using stored mode */
    if (!ctx->current_mode) {
        fprintf(stderr, "[DRM] Page flip: current_mode not initialized\n");
        return -1;
    }
    
    drmModeModeInfo *mode = (drmModeModeInfo *)ctx->current_mode;
    
    int result = drmModeSetCrtc(ctx->fd, ctx->crtc_id, ctx->back.fb_id, 0, 0,
                                &ctx->connector_id, 1, mode);
    if (result != 0) {
        fprintf(stderr, "[DRM] drmModeSetCrtc (flip) failed: %s (errno=%d)\n", strerror(errno), errno);
        /* Don't fail entirely, just log and continue */
        return 0;  /* Return success anyway to keep loop going */
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
    }
}
