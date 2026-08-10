#ifndef NUCLEUS_TAO_HDR_SCENE_H
#define NUCLEUS_TAO_HDR_SCENE_H

#include <windows.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct NucleusHdrScene NucleusHdrScene;

NucleusHdrScene *nucleus_tao_hdr_scene_create(
    HWND hwnd,
    void *eglDisplay,
    void *shareContext,
    int widthPx,
    int heightPx);
void nucleus_tao_hdr_scene_destroy(NucleusHdrScene *scene);
BOOL nucleus_tao_hdr_scene_make_current(NucleusHdrScene *scene);
BOOL nucleus_tao_hdr_scene_resize(NucleusHdrScene *scene, int widthPx, int heightPx);
BOOL nucleus_tao_hdr_scene_present(NucleusHdrScene *scene);
void nucleus_tao_hdr_scene_set_vsync_enabled(NucleusHdrScene *scene, BOOL enabled);

void *nucleus_tao_hdr_scene_egl_surface(NucleusHdrScene *scene);
void *nucleus_tao_hdr_scene_egl_context(NucleusHdrScene *scene);
void *nucleus_tao_hdr_scene_egl_config(NucleusHdrScene *scene);

BOOL nucleus_tao_hdr_scene_is_hdr_output(NucleusHdrScene *scene);
float nucleus_tao_hdr_scene_sdr_white_nits(NucleusHdrScene *scene);
float nucleus_tao_hdr_scene_max_luminance_nits(NucleusHdrScene *scene);
float nucleus_tao_hdr_scene_headroom(NucleusHdrScene *scene);
long long nucleus_tao_hdr_scene_generation(NucleusHdrScene *scene);
long long nucleus_tao_hdr_scene_presented_frames(NucleusHdrScene *scene);
long long nucleus_tao_hdr_scene_adapter_luid(NucleusHdrScene *scene);

#ifdef __cplusplus
}
#endif

#endif
