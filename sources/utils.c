/**
 * (C)2023 aks
 * https://akscf.me/
 * https://github.com/akscf/
 **/
#include "mod_whisper_asr.h"

extern globals_t globals;

void thread_finished() {
    switch_mutex_lock(globals.mutex);
    if(globals.active_threads > 0) { globals.active_threads--; }
    switch_mutex_unlock(globals.mutex);
}

void thread_launch(switch_memory_pool_t *pool, switch_thread_start_t fun, void *data) {
    switch_threadattr_t *attr = NULL;
    switch_thread_t *thread = NULL;

    switch_mutex_lock(globals.mutex);
    globals.active_threads++;
    switch_mutex_unlock(globals.mutex);

    switch_threadattr_create(&attr, pool);
    switch_threadattr_detach_set(attr, 1);
    switch_threadattr_stacksize_set(attr, SWITCH_THREAD_STACKSIZE);
    switch_thread_create(&thread, attr, fun, data, pool);

    return;
}

uint32_t asr_ctx_take(wasr_ctx_t *asr_ctx) {
    uint32_t status = false;

    if(!asr_ctx) { return false; }

    switch_mutex_lock(asr_ctx->mutex);
    if(asr_ctx->fl_destroyed == false) {
        status = true;
        asr_ctx->deps++;
    }
    switch_mutex_unlock(asr_ctx->mutex);

    return status;
}

void asr_ctx_release(wasr_ctx_t *asr_ctx) {
    switch_assert(asr_ctx);

    switch_mutex_lock(asr_ctx->mutex);
    if(asr_ctx->deps) { asr_ctx->deps--; }
    switch_mutex_unlock(asr_ctx->mutex);
}

switch_status_t xdata_buffer_alloc(xdata_buffer_t **out, switch_byte_t *data, uint32_t data_len) {
    xdata_buffer_t *buf = NULL;

    switch_zmalloc(buf, sizeof(xdata_buffer_t));

    if(data_len) {
        switch_malloc(buf->data, data_len);
        switch_assert(buf->data);

        buf->len = data_len;
        memcpy(buf->data, data, data_len);
    }

    *out = buf;
    return SWITCH_STATUS_SUCCESS;
}

void xdata_buffer_free(xdata_buffer_t **buf) {
    if(buf && *buf) {
        switch_safe_free((*buf)->data);
        free(*buf);
    }
}

void xdata_buffer_queue_clean(switch_queue_t *queue) {
    xdata_buffer_t *data = NULL;

    if(!queue || !switch_queue_size(queue)) { return; }

    while(switch_queue_trypop(queue, (void *) &data) == SWITCH_STATUS_SUCCESS) {
        if(data) { xdata_buffer_free(&data); }
    }
}

switch_status_t xdata_buffer_push(switch_queue_t *queue, switch_byte_t *data, uint32_t data_len) {
    xdata_buffer_t *buff = NULL;

    if(xdata_buffer_alloc(&buff, data, data_len) == SWITCH_STATUS_SUCCESS) {
        if(switch_queue_trypush(queue, buff) == SWITCH_STATUS_SUCCESS) {
            return SWITCH_STATUS_SUCCESS;
        }
        xdata_buffer_free(&buff);
    }
    return SWITCH_STATUS_FALSE;
}


