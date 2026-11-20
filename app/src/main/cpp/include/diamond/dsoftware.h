/* Diamond Engine — software rasteriser used by the Hybrid backend. */
#ifndef DIAMOND_DSOFTWARE_H
#define DIAMOND_DSOFTWARE_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

int  software_target_resize(int width, int height);
void software_target_release(void);
void software_clear(uint32_t abgr);
void software_set_smooth(int smooth);
void software_fill_rect(float x, float y, float w, float h, uint32_t abgr);
void software_blit_sprite(const void *rgba, int sourceWidth, int sourceHeight,
                          float dx, float dy, float dw, float dh, float rotation,
                          uint32_t tint, int alphaTest);
const void *software_target_pixels(int *width, int *height);
int  software_target_width(void);
int  software_target_height(void);

#ifdef __cplusplus
}
#endif
#endif
