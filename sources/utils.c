/**
 **
 ** (C)2024 aks
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
        asr_ctx->refs++;
    }
    switch_mutex_unlock(asr_ctx->mutex);

    return status;
}

void asr_ctx_release(wasr_ctx_t *asr_ctx) {
    switch_assert(asr_ctx);

    switch_mutex_lock(asr_ctx->mutex);
    if(asr_ctx->refs) { asr_ctx->refs--; }
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

static bool xxx_whisper_encoder_begin_callback(struct whisper_context *ctx, struct whisper_state *state, void *udata) {
    wasr_ctx_t *asr_ctx = (wasr_ctx_t *)udata;
    return(asr_ctx->fl_abort ? false : true);
}

switch_status_t transcribe(wasr_ctx_t *ast_ctx, float *audio, uint32_t samples, switch_buffer_t *text_buffer) {
    switch_status_t status = SWITCH_STATUS_SUCCESS;
    struct whisper_full_params wparams = {0};
    int segments = 0;

    if(!ast_ctx->wctx) {
        switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "(ast_ctx->wctx == NULL)\n");
        return SWITCH_STATUS_FALSE;
    }

    switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_NOTICE, "transcribe samples=%d\n", samples);

    wparams = whisper_full_default_params(WHISPER_SAMPLING_GREEDY);
    wparams.print_progress   = false;
    wparams.print_special    = false;
    wparams.print_realtime   = false;
    wparams.print_timestamps = false;
    wparams.translate        = ast_ctx->whisper_translate;
    wparams.single_segment   = ast_ctx->whisper_single_segment;
    wparams.max_tokens       = ast_ctx->whisper_max_tokens;
    wparams.language         = ast_ctx->lang ? ast_ctx->lang : "en";
    wparams.n_threads        = globals.whisper_threads;
    wparams.audio_ctx        = 0;

    wparams.encoder_begin_callback_user_data = ast_ctx;
    wparams.encoder_begin_callback = (whisper_encoder_begin_callback) xxx_whisper_encoder_begin_callback;

    if(whisper_full(ast_ctx->wctx, wparams, audio, samples) != 0) {
        switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "whisper_full()\n");
        switch_goto_status(SWITCH_STATUS_FALSE, out);
    }

    if(ast_ctx->fl_abort) {
        goto out;
    }

    if((segments = whisper_full_n_segments(ast_ctx->wctx))) {
        for(uint32_t i = 0; i < segments; ++i) {
            const char *text = whisper_full_get_segment_text(ast_ctx->wctx, i);
            if(text) {
                switch_buffer_write(text_buffer, text, strlen(text));
                switch_buffer_write(text_buffer, "\n", 1);
            }
        }
    }
out:
    return status;
}

void i2f(int16_t *in, float *out, uint32_t samples) {
    for(uint32_t i = 0; i < samples; i++) {
        out[i] = (float) ((in[i] > 0) ? (in[i] / 32767.0) : (in[i] / 32768.0));
    }
}
