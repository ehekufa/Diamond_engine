/* Diamond Engine — render thread + surface ownership (internal). */
#ifndef DIAMOND_DWINDOW_H
#define DIAMOND_DWINDOW_H

#include <pthread.h>

#include "diamond/drender.h"

#ifdef __cplusplus
extern "C" {
#endif

extern DRenderStats g_render_stats;

int  dwindow_start(void);
void dwindow_stop(void);
int  dwindow_attach(void *native_window, int width, int height);
void dwindow_detach(void);
void dwindow_resize(int width, int height);
void dwindow_set_paused(int paused);
int  dwindow_attached(void);
const char *dwindow_last_error(void);

#ifdef __cplusplus
}
#endif
#endif
