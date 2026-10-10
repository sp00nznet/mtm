/*
 * The host's DirectDraw (ddraw.c): the game's DirectDrawCreate gets an
 * IDirectDraw implemented here over plain memory surfaces, shown in an
 * ordinary window, captured for --record, or shown nowhere (--headless).
 * docs/display.md has the reasoning.
 */
#ifndef DDRAW_HOST_H
#define DDRAW_HOST_H
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include "native32.h"

typedef struct {
    int headless;                /* cloak the window; never take focus */
    int scale;                   /* --scale: window client = 640x480 * scale, this run only */
    int trace;                   /* one line per DirectDraw call */
    const char* record;          /* --record: ffmpeg pipe, 30 fps against the wall clock */
    long stop_after;             /* --frames N: exit after N presents */
    long shot_frame;             /* --shot N:file.bmp */
    const char* shot_path;
    long diff_a, diff_b;         /* --diff A,B: ms after start */
} ddh_opts_t;

void ddh_init(const ddh_opts_t* o);
void ddh_finish(void);           /* close the recording; safe to call twice */
HWND ddh_window(void);           /* the game's window, once it exists */
int  ddh_focused(void);          /* the real keyboard is meant for the game */
long ddh_frames(void);

void ddh_shim_DirectDrawCreate(void);
void ddh_shim_CreateWindowExA(void);
void ddh_shim_ShowWindow(void);
#define DDH_SHIMS \
    { "DirectDrawCreate", ddh_shim_DirectDrawCreate }, \
    { "CreateWindowExA", ddh_shim_CreateWindowExA }, \
    { "ShowWindow", ddh_shim_ShowWindow }

#endif
