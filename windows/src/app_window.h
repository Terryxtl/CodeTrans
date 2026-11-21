/* app_window.h - entry points of the main window used by main.c */
#ifndef CPW_APP_WINDOW_H
#define CPW_APP_WINDOW_H

#include <windows.h>

/* Creates the main window and starts the producer / pacer / listeners. */
BOOL app_create(HINSTANCE hi, BOOL start_hidden);
/* Message-loop hook for the keyboard shortcuts (§6.1). TRUE = handled. */
BOOL app_pretranslate(MSG *m);
/* Stops the worker threads (after the message loop ended). */
void app_shutdown(void);

#endif
