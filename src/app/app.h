/*
 * The application (app.cpp): the boot and the main thread's loop. main() (start.c) calls it after
 * the HAL and the kernel are up; the virtual board's main thread calls it as well.
 */
#ifndef APP_H
#define APP_H

#ifdef __cplusplus
extern "C" {
#endif

// the boot (every thread of app_threads.h), then the main loop: never returns
void app_run(void);

#ifdef __cplusplus
}
#endif

#endif
