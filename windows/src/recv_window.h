/* recv_window.h - "摄像头接收": phone -> PC with the webcam (tray menu / F3). */
#ifndef CPW_RECV_WINDOW_H
#define CPW_RECV_WINDOW_H

#include <windows.h>

BOOL recv_window_register(HINSTANCE hi);
/* Opens the window and starts the camera, or brings an open one to the front. */
void recv_window_show(void);
/* Closes it (camera off); used at exit. */
void recv_window_close(void);
BOOL recv_window_open(void);

#endif
