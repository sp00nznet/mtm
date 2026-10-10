/* The presenter: present.c. docs/presenter.md. */
#pragma once
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdint.h>

void present_init(const char* ini, int scale);   /* load [video]; scale > 0 overrides it */
void present_start(HWND game);                   /* the game's window: menu, size, render thread */
void present_frame(const uint32_t* bgra);        /* a new 640x480 picture */
int  present_pause_in_background(void);
void present_unmap(int* x, int* y);             /* 640x480 -> the window's client area */
/* The game window's subclass offers every message here first: 1 = handled
 * (*r is the result). Mouse messages come back with their position mapped
 * to 640x480. */
int  present_message(HWND h, UINT m, WPARAM w, LPARAM* l, LRESULT* r);
