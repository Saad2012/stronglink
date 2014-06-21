#include <stdlib.h>
#include <uv.h>
#include "../deps/libco/libco.h"

extern uv_loop_t *loop;
extern cothread_t yield;

static void async_fs_cb(uv_fs_t *const req) {
	co_switch(req->data);
}
static void async_timer_cb(uv_timer_t *const timer) {
	co_switch(timer->data);
}
static void async_write_cb(uv_write_t *const req, int status) {
	co_switch(req->data);
}
static void async_close_cb(uv_handle_t *const handle) {
	co_switch(handle->data);
}

void async_init(void);
void async_wakeup(cothread_t const thread);

void co_terminate(void);
