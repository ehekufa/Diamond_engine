/* Diamond Engine — internal engine state (not exposed to Java). */
#ifndef DIAMOND_DENGINE_H
#define DIAMOND_DENGINE_H

#include "diamond.h"
#include "diamond/drender.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Global engine state, defined once in api/diamond_api.cpp. */
typedef struct DEngineState {
    int initialised;
    int render_mode;        /* DiamondRenderMode, selected in the splash       */
    int view_mode;          /* DiamondViewMode                                */
    int tool;               /* DiamondTool                                    */
    int play_state;         /* DiamondPlayState                               */
    int surface_attached;
    int width;
    int height;
    float density;
    DCamera camera;
    float joystick[2];
    int buttons[DIAMOND_BTN_COUNT];
    char files_dir[512];
    char cache_dir[512];
    int api_level;
    /* Frame timing, updated by the render thread. */
    float fps;
    float frame_ms;
    long frame_count;
    double last_frame_time;
    /* Cached strings handed to Java; they must stay alive while Java copies. */
    char gpu_vendor[64];
    char gpu_renderer[128];
    char stats[96];
    char banner[192];
    char log_buffer[8192];
    char *scratch;          /* dynamic scratch for export/import             */
} DEngineState;

extern DEngineState g_engine;

/* Implemented in api/diamond_api.cpp and used by the format module. */
int  dengine_describe_stats(char *out, int cap);

#ifdef __cplusplus
}
#endif
#endif /* DIAMOND_DENGINE_H */
