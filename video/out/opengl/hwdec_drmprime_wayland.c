/*
 * This file is part of mpv.
 *
 * mpv is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * mpv is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with mpv.  If not, see <http://www.gnu.org/licenses/>.
 */

#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <stdbool.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <unistd.h>
#include <wayland-client-protocol.h>

#include <libavutil/hwcontext_drm.h>

#include "common.h"
#include "video/hwdec.h"
#include "common/msg.h"
#include "options/m_config.h"
#include "libmpv/opengl_cb.h"
#include "video/out/drm_common.h"
#include "video/out/gpu/hwdec.h"
#include "video/out/wayland_common.h"
#include "video/out/wayland/linux-dmabuf-v1.h"
#include "video/mp_image.h"

#include "ra_gl.h"

struct priv;

struct dmabuf_frame {
    struct wl_buffer *buffer;
    struct mp_image *image;
};

struct priv {
    struct mp_log *log;

    // embedder parameters
    struct mpv_opengl_cb_wayland_params *wayland_params;

    // interop wayland interfaces
    struct wl_registry      *registry;
    struct zwp_linux_dmabuf_v1 *dmabuf;

    // interop video layer objects
    struct wl_subcompositor *subcompositor;
    struct wl_surface *video_surface;
    struct wl_subsurface *video_subsurface;
};

static void buffer_release(void *data, struct wl_buffer *buffer)
{
    struct dmabuf_frame *frame = data;

    if (frame) {
        talloc_free(frame->image);
        wl_buffer_destroy(buffer);
        free(frame);
    }
}

static const struct wl_buffer_listener buffer_listener = {
    buffer_release
};


static void param_create_succeeded(void *data, struct zwp_linux_buffer_params_v1 *params, struct wl_buffer *new_buffer)
{
    struct dmabuf_frame *frame = data;
    frame->buffer = new_buffer;

    wl_buffer_add_listener(new_buffer, &buffer_listener, frame);
    zwp_linux_buffer_params_v1_destroy(params);
}

static void param_create_failed(void *data, struct zwp_linux_buffer_params_v1 *params)
{
    struct dmabuf_frame *frame = data;
    frame->buffer = NULL;
    zwp_linux_buffer_params_v1_destroy(params);
}

static const struct zwp_linux_buffer_params_v1_listener params_listener = {
    param_create_succeeded,
    param_create_failed
};

static int overlay_frame(struct ra_hwdec *hw, struct mp_image *hw_image,
                         struct mp_rect *src, struct mp_rect *dst, bool newframe)
{
    struct priv *p = hw->priv;
    AVDRMFrameDescriptor *desc = NULL;
    AVDRMLayerDescriptor *layer = NULL;
    struct zwp_linux_buffer_params_v1 *params;
    struct dmabuf_frame *frame = NULL;
    int ret = 0;

    if (hw_image) {
        desc = (AVDRMFrameDescriptor *)hw_image->planes[0];
        if (desc) {

            uint64_t modifier = 0;
            uint32_t flags = 0;

            for( int l=0; l < desc->nb_layers; l++) {
                layer = &desc->layers[l];

                params = zwp_linux_dmabuf_v1_create_params(p->dmabuf);

                for (int plane = 0; plane < AV_DRM_MAX_PLANES; plane++) {
                    int fd =  desc->objects[layer->planes[plane].object_index].fd;
                    if (fd && layer->planes[plane].pitch) {
                        zwp_linux_buffer_params_v1_add(params,
                                                       fd,
                                                       plane, /* plane_idx */
                                                       layer->planes[plane].offset, /* offset */
                                                       layer->planes[plane].pitch,
                                                       modifier >> 32,
                                                       modifier & 0xffffffff);
                    }
                }

                frame = malloc(sizeof(struct dmabuf_frame));
                if (!frame) {
                    MP_ERR(hw, "Out of memory\n");
                    ret = -1; // should be OOM code here
                    goto fail;
                }

                zwp_linux_buffer_params_v1_add_listener(params, &params_listener, frame);
                zwp_linux_buffer_params_v1_create(params,
                                                  src->x1 - src->x0,
                                                  src->y1 - src->y0,
                                                  layer->format,
                                                  flags);

                wl_display_roundtrip(p->wayland_params->display);

                if (frame->buffer) {
                    frame->image = mp_image_new_ref(hw_image);
                    wl_surface_attach(p->video_surface, frame->buffer, 0, 0);
                    wl_surface_commit(p->video_surface);

                } else {
                    MP_ERR(hw, "Failed to create dmabuffer parameters\n");
                    ret = -1;
                    goto fail;
                }
            }
        }
    } else {
        // release the current attached buffer
        wl_surface_attach(p->video_surface, NULL, 0, 0);
        wl_surface_commit(p->video_surface);
    }

    return 0;
    
 fail:
    if (frame)
        free(frame);

    return ret;
}

static void uninit(struct ra_hwdec *hw)
{
    struct priv *p = hw->priv;

    if (p->video_surface)
        wl_surface_destroy(p->video_surface);

    if (p->video_subsurface)
        wl_subsurface_destroy(p->video_subsurface);

    wl_display_roundtrip(p->wayland_params->display);

}

static void dmabuf_format(void *data, struct zwp_linux_dmabuf_v1 *zwp_linux_dmabuf,
              uint32_t format)
{
}

static const struct zwp_linux_dmabuf_v1_listener dmabuf_listener = {
        dmabuf_format
};


static void registry_handle_add(void *data, struct wl_registry *reg, uint32_t id,
                                const char *interface, uint32_t ver)
{
    struct priv *p = data;
    int found = 1;

    if (!strcmp(interface, wl_subcompositor_interface.name) && found++) {
        p->subcompositor = wl_registry_bind(reg, id, &wl_subcompositor_interface, 1);
    }

    if (!strcmp(interface, "zwp_linux_dmabuf_v1") && found++) {
        p->dmabuf = wl_registry_bind(reg, id, &zwp_linux_dmabuf_v1_interface, 1);
        zwp_linux_dmabuf_v1_add_listener(p->dmabuf, &dmabuf_listener, p);
    }

    if (found > 1)
        MP_VERBOSE(p, "Registered for protocol %s\n", interface);
}

static void registry_handle_remove(void *data, struct wl_registry *reg, uint32_t id)
{
}

static const struct wl_registry_listener registry_listener = {
    registry_handle_add,
    registry_handle_remove,
};

static int init(struct ra_hwdec *hw)
{
    struct priv *p = hw->priv;
    GL *gl = ra_gl_get(hw->ra);

    p->log = hw->log;
    p->wayland_params = gl ? (struct mpv_opengl_cb_wayland_params *)
                             mpgl_get_native_display(gl, "opengl-cb-wayland-params") : NULL;
    if (!p->wayland_params) {
        MP_ERR(hw, "Unable to get Wayland interop parameters\n");
        goto err;
    }

    p->registry = wl_display_get_registry(p->wayland_params->display);
    wl_registry_add_listener(p->registry, &registry_listener, p);
    wl_display_roundtrip(p->wayland_params->display);

    p->video_surface = wl_compositor_create_surface(p->wayland_params->compositor);
    if (!p->video_surface) {
        MP_ERR(hw, "Failed to create video surface\n");
        goto err;
    }

    p->video_subsurface = wl_subcompositor_get_subsurface(p->subcompositor,
                                                          p->video_surface,
                                                          p->wayland_params->surface);
    if (!p->video_subsurface) {
        MP_ERR(hw, "Failed to create video subsurface\n");
        goto err;
    }

    wl_subsurface_place_below(p->video_subsurface, p->wayland_params->surface);

    return 0;

err:
    uninit(hw);
    return -1;
}

const struct ra_hwdec_driver ra_hwdec_drmprime_wayland = {
    .name = "drmprime-wayland",
    .priv_size = sizeof(struct priv),
    .imgfmts = {IMGFMT_DRMPRIME, 0},
    .init = init,
    .overlay_frame = overlay_frame,
    .uninit = uninit,
};
