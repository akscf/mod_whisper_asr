/*
 * FreeSWITCH Modular Media Switching Software Library / Soft-Switch Application
 * Copyright (C) 2005-2014, Anthony Minessale II <anthm@freeswitch.org>
 *
 * Version: MPL 1.1
 *
 * The contents of this file are subject to the Mozilla Public License Version
 * 1.1 (the "License"); you may not use this file except in compliance with
 * the License. You may obtain a copy of the License at
 * http://www.mozilla.org/MPL/
 *
 * Software distributed under the License is distributed on an "AS IS" basis,
 * WITHOUT WARRANTY OF ANY KIND, either express or implied. See the License
 * for the specific language governing rights and limitations under the
 * License.
 *
 * Module Contributor(s):
 *  Konstantin Alexandrin <akscfx@gmail.com>
 *
 *
 */
#include "mod_whisper_asr.h"

static globals_t globals;

SWITCH_MODULE_LOAD_FUNCTION(mod_whisper_asr_load);
SWITCH_MODULE_SHUTDOWN_FUNCTION(mod_whisper_asr_shutdown);
SWITCH_MODULE_DEFINITION(mod_whisper_asr, mod_whisper_asr_load, mod_whisper_asr_shutdown, NULL);


static void *SWITCH_THREAD_FUNC whisper_transcribe_thread(switch_thread_t *thread, void *obj) {
    volatile wasr_ctx_t *_ref = (wasr_ctx_t *) obj;
    wasr_ctx_t *asr_ctx = (wasr_ctx_t *) _ref;
    switch_memory_pool_t *pool = NULL;
    switch_byte_t *chunk_bufferfer = NULL;
    switch_buffer_t *text_buffer = NULL;
    switch_byte_t *rsmp_buffer = NULL;
    switch_byte_t *float_buffer = NULL;
    uint32_t rsmp_buffer_size = 0, float_buffer_size = 0;
    uint32_t chunk_buffer_offset = 0, chunk_buffer_size = 0;
    uint8_t fl_transcribe = SWITCH_FALSE;
    void *pop = NULL;

    switch_mutex_lock(asr_ctx->mutex);
    asr_ctx->refs++;
    switch_mutex_unlock(asr_ctx->mutex);

    if(switch_core_new_memory_pool(&pool) != SWITCH_STATUS_SUCCESS) {
        switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_CRIT, "switch_core_new_memory_pool()\n");
        goto out;
    }
    if(switch_buffer_create_dynamic(&text_buffer, 1024, 1024, 16384) != SWITCH_STATUS_SUCCESS) {
        switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "switch_buffer_create_dynamic()\n");
        asr_ctx->fl_abort = SWITCH_TRUE;
        goto out;
    }

    while(SWITCH_TRUE) {
        if(globals.fl_shutdown || asr_ctx->fl_destroyed || asr_ctx->fl_abort) {
            break;
        }

        if(chunk_buffer_size == 0) {
            switch_mutex_lock(asr_ctx->mutex);
            chunk_buffer_size = asr_ctx->chunk_buffer_size;

            if((chunk_bufferfer = switch_core_alloc(pool, chunk_buffer_size)) == NULL) {
                switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "switch_core_alloc()\n");
                asr_ctx->fl_abort = SWITCH_TRUE;
            }
            if(asr_ctx->resampler) {
                rsmp_buffer_size = (WHISPER_SAMPLE_RATE * chunk_buffer_size) / asr_ctx->samplerate;
                float_buffer_size = (rsmp_buffer_size / sizeof(int16_t)) * sizeof(float);

                if((rsmp_buffer = (switch_byte_t *)switch_core_alloc(pool, rsmp_buffer_size)) == NULL) {
                    switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "switch_core_alloc()\n");
                    asr_ctx->fl_abort = SWITCH_TRUE;
                }
                if((float_buffer = (switch_byte_t *) switch_core_alloc(pool, float_buffer_size)) == NULL) {
                    switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "switch_core_alloc()\n");
                    asr_ctx->fl_abort = SWITCH_TRUE;
                }
            } else {
                float_buffer_size = (chunk_buffer_size / sizeof(int16_t)) * sizeof(float);
                if((float_buffer = (switch_byte_t *) switch_core_alloc(pool, float_buffer_size)) == NULL) {
                    switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "switch_core_alloc()\n");
                    asr_ctx->fl_abort = SWITCH_TRUE;
                }
            }

            switch_mutex_unlock(asr_ctx->mutex);
            goto timer_next;
        }

        fl_transcribe = SWITCH_FALSE;
        while(switch_queue_trypop(asr_ctx->q_audio, &pop) == SWITCH_STATUS_SUCCESS) {
            xdata_buffer_t *audio_buffer = (xdata_buffer_t *)pop;
            if(globals.fl_shutdown || asr_ctx->fl_destroyed ) { break; }
            if(audio_buffer && audio_buffer->len) {
                memcpy((chunk_bufferfer + chunk_buffer_offset), audio_buffer->data, audio_buffer->len);
                chunk_buffer_offset += audio_buffer->len;
                if(chunk_buffer_offset >= chunk_buffer_size) {
                    chunk_buffer_offset = chunk_buffer_size;
                    fl_transcribe = SWITCH_TRUE;
                    break;
                }
            }
            xdata_buffer_free(&audio_buffer);
        }

        if(!fl_transcribe) {
            fl_transcribe = (chunk_buffer_offset > 0 && asr_ctx->vad_state == SWITCH_VAD_STATE_STOP_TALKING);
        }

        if(fl_transcribe) {
            spx_uint32_t in_smps = (chunk_buffer_offset / sizeof(int16_t));  // to samples
            spx_uint32_t out_smps = 0;

            if(asr_ctx->resampler) {
                out_smps = (rsmp_buffer_size / sizeof(int16_t));
                speex_resampler_process_interleaved_int(asr_ctx->resampler, (const spx_int16_t *)chunk_bufferfer, (spx_uint32_t *)&in_smps, (spx_int16_t *)rsmp_buffer, &out_smps);
                i2f((int16_t *)rsmp_buffer, (float *)float_buffer, out_smps);
            } else {
                out_smps = in_smps;
                i2f((int16_t *)chunk_bufferfer, (float *)float_buffer, out_smps);
            }

            switch_buffer_zero(text_buffer);

            if(transcribe(asr_ctx, (float *)float_buffer, out_smps, text_buffer, &globals) == SWITCH_STATUS_SUCCESS) {
                const void *ptr = NULL; uint32_t tlen = 0;
                if((tlen = switch_buffer_peek_zerocopy(text_buffer, &ptr)) > 0) {
                    if(xdata_buffer_push(asr_ctx->q_text, (switch_byte_t *)ptr, tlen) == SWITCH_STATUS_SUCCESS) {
                        switch_mutex_lock(asr_ctx->mutex);
                        asr_ctx->transcript_results++;
                        switch_mutex_unlock(asr_ctx->mutex);
                    }
                }
            }
            chunk_buffer_offset = 0;
        }

        timer_next:
        switch_yield(10000);
    }
out:
    if(text_buffer) {
        switch_buffer_destroy(&text_buffer);
    }
    if(pool) {
        switch_core_destroy_memory_pool(&pool);
    }

    switch_mutex_lock(asr_ctx->mutex);
    if(asr_ctx->refs > 0) asr_ctx->refs--;
    switch_mutex_unlock(asr_ctx->mutex);

    switch_mutex_lock(globals.mutex);
    if(globals.active_threads > 0) { globals.active_threads--; }
    switch_mutex_unlock(globals.mutex);

    return NULL;
}

// ---------------------------------------------------------------------------------------------------------------------------------------------
// asr interface
// ---------------------------------------------------------------------------------------------------------------------------------------------
static switch_status_t asr_open(switch_asr_handle_t *ah, const char *codec, int samplerate, const char *dest, switch_asr_flag_t *flags) {
    switch_status_t status = SWITCH_STATUS_SUCCESS;
    struct whisper_context_params cparams = {0};
    switch_threadattr_t *attr = NULL;
    switch_thread_t *thread = NULL;
    wasr_ctx_t *asr_ctx = NULL;
    int err = 0;

    if(strcmp(codec, "L16") !=0) {
        switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "Unsupported encoding: %s\n", codec);
        switch_goto_status(SWITCH_STATUS_FALSE, out);
    }

    asr_ctx = switch_core_alloc(ah->memory_pool, sizeof(wasr_ctx_t));
    asr_ctx->samplerate = samplerate;
    asr_ctx->channels = 1;
    asr_ctx->lang = NULL;

    ah->private_info = asr_ctx;

   if((status = switch_mutex_init(&asr_ctx->mutex, SWITCH_MUTEX_NESTED, ah->memory_pool)) != SWITCH_STATUS_SUCCESS) {
        switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "switch_mutex_init()\n");
        switch_goto_status(SWITCH_STATUS_GENERR, out);
    }

    switch_queue_create(&asr_ctx->q_audio, QUEUE_SIZE, ah->memory_pool);
    switch_queue_create(&asr_ctx->q_text, QUEUE_SIZE, ah->memory_pool);

    asr_ctx->fl_vad_enabled = globals.fl_vad_enabled;
    asr_ctx->frame_len = 0;
    asr_ctx->vad_buffer = NULL;
    asr_ctx->vad_buffer_size = 0;
    asr_ctx->vad_stored_frames = 0;
    asr_ctx->fl_vad_first_cycle = SWITCH_TRUE;

    if((asr_ctx->vad = switch_vad_init(asr_ctx->samplerate, asr_ctx->channels)) == NULL) {
        switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "Couldn't init VAD\n");
        switch_goto_status(SWITCH_STATUS_GENERR, out);
    }

    switch_vad_set_mode(asr_ctx->vad, -1);
    switch_vad_set_param(asr_ctx->vad, "debug", globals.fl_vad_debug);
    if(globals.vad_silence_ms > 0)  { switch_vad_set_param(asr_ctx->vad, "silence_ms", globals.vad_silence_ms); }
    if(globals.vad_voice_ms > 0)    { switch_vad_set_param(asr_ctx->vad, "voice_ms", globals.vad_voice_ms); }
    if(globals.vad_threshold > 0)   { switch_vad_set_param(asr_ctx->vad, "thresh", globals.vad_threshold); }

    if(asr_ctx->samplerate != WHISPER_SAMPLE_RATE) {
        asr_ctx->resampler = speex_resampler_init(asr_ctx->channels, asr_ctx->samplerate, WHISPER_SAMPLE_RATE, SWITCH_RESAMPLE_QUALITY, &err);
        if(err != 0) {
            switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "speex_resampler_init() : %s\n", speex_resampler_strerror(err));
            switch_goto_status(SWITCH_STATUS_GENERR, out);
        }
    }

    cparams = whisper_context_default_params();
    cparams.use_gpu = globals.whisper_use_gpu;
    cparams.gpu_device = globals.whisper_gpu_dev;
    cparams.flash_attn = globals.whisper_flash_attn;

    if((asr_ctx->wctx = whisper_init_from_file_with_params(globals.model_file, cparams)) == NULL) {
        switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "whisper_init_from_file_with_params()\n");
        switch_goto_status(SWITCH_STATUS_GENERR, out);
    }

    asr_ctx->chunk_buffer_size = 0;

    switch_mutex_lock(globals.mutex);
    globals.active_threads++;
    switch_mutex_unlock(globals.mutex);

    switch_threadattr_create(&attr, ah->memory_pool);
    switch_threadattr_detach_set(attr, 1);
    switch_threadattr_stacksize_set(attr, SWITCH_THREAD_STACKSIZE);
    switch_thread_create(&thread, attr, whisper_transcribe_thread, asr_ctx, ah->memory_pool);

out:
    return status;
}

static switch_status_t asr_close(switch_asr_handle_t *ah, switch_asr_flag_t *flags) {
    wasr_ctx_t *asr_ctx = (wasr_ctx_t *) ah->private_info;
    uint8_t fl_wloop = SWITCH_TRUE;

    assert(asr_ctx != NULL);

    asr_ctx->fl_abort = SWITCH_TRUE;
    asr_ctx->fl_destroyed = SWITCH_TRUE;

    switch_mutex_lock(asr_ctx->mutex);
    fl_wloop = (asr_ctx->refs != 0);
    switch_mutex_unlock(asr_ctx->mutex);

    if(fl_wloop) {
        switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_DEBUG, "Waiting for unlock (refs=%u)!\n", asr_ctx->refs);
        while(fl_wloop) {
            switch_mutex_lock(asr_ctx->mutex);
            fl_wloop = (asr_ctx->refs != 0);
            switch_mutex_unlock(asr_ctx->mutex);
            switch_yield(100000);
        }
    }

    if(asr_ctx->q_audio) {
        xdata_buffer_queue_clean(asr_ctx->q_audio);
        switch_queue_term(asr_ctx->q_audio);
    }
    if(asr_ctx->q_text) {
        xdata_buffer_queue_clean(asr_ctx->q_text);
        switch_queue_term(asr_ctx->q_text);
    }
    if(asr_ctx->vad) {
        switch_vad_destroy(&asr_ctx->vad);
    }

    if(asr_ctx->wctx) {
        whisper_free(asr_ctx->wctx);
    }

    if(asr_ctx->resampler) {
        speex_resampler_destroy(asr_ctx->resampler);
    }

    if(asr_ctx->vad_buffer) {
        switch_buffer_destroy(&asr_ctx->vad_buffer);
    }

    switch_set_flag(ah, SWITCH_ASR_FLAG_CLOSED);

    return SWITCH_STATUS_SUCCESS;
}

static switch_status_t asr_feed(switch_asr_handle_t *ah, void *data, unsigned int data_len, switch_asr_flag_t *flags) {
    wasr_ctx_t *asr_ctx = (wasr_ctx_t *) ah->private_info;
    switch_vad_state_t vad_state = 0;
    uint8_t fl_has_audio = SWITCH_FALSE;

    assert(asr_ctx != NULL);

    if(switch_test_flag(ah, SWITCH_ASR_FLAG_CLOSED)) {
        return SWITCH_STATUS_BREAK;
    }
    if(asr_ctx->fl_abort) {
        return SWITCH_STATUS_BREAK;
    }
    if(asr_ctx->fl_pause) {
        return SWITCH_STATUS_SUCCESS;
    }

    if(data_len > 0 && asr_ctx->frame_len == 0) {
        switch_mutex_lock(asr_ctx->mutex);
        asr_ctx->frame_len = data_len;
        asr_ctx->vad_buffer_size = asr_ctx->frame_len * VAD_STORE_FRAMES;
        asr_ctx->chunk_buffer_size = asr_ctx->samplerate * globals.chunk_time_sec;
        switch_mutex_unlock(asr_ctx->mutex);

        if(switch_buffer_create(ah->memory_pool, &asr_ctx->vad_buffer, asr_ctx->vad_buffer_size) != SWITCH_STATUS_SUCCESS) {
            asr_ctx->vad_buffer_size = 0;
            switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "switch_buffer_create()\n");
        }
    }

    if(asr_ctx->fl_vad_enabled && asr_ctx->vad_buffer_size) {
        if(asr_ctx->vad_state == SWITCH_VAD_STATE_STOP_TALKING || (asr_ctx->vad_state == vad_state && vad_state == SWITCH_VAD_STATE_NONE)) {
            if(data_len <= asr_ctx->frame_len) {
                if(asr_ctx->vad_stored_frames >= VAD_STORE_FRAMES) {
                    switch_buffer_zero(asr_ctx->vad_buffer);
                    asr_ctx->vad_stored_frames = 0;
                    asr_ctx->fl_vad_first_cycle = SWITCH_FALSE;
                }
                switch_buffer_write(asr_ctx->vad_buffer, data, MIN(asr_ctx->frame_len, data_len));
                asr_ctx->vad_stored_frames++;
            }
        }

        vad_state = switch_vad_process(asr_ctx->vad, (int16_t *)data, (data_len / sizeof(int16_t)) );
        if(vad_state == SWITCH_VAD_STATE_START_TALKING) {
            asr_ctx->vad_state = vad_state;
            fl_has_audio = SWITCH_TRUE;
        } else if (vad_state == SWITCH_VAD_STATE_STOP_TALKING) {
            switch_vad_reset(asr_ctx->vad);
            asr_ctx->vad_state = vad_state;
            fl_has_audio = SWITCH_FALSE;
        } else if (vad_state == SWITCH_VAD_STATE_TALKING) {
            asr_ctx->vad_state = vad_state;
            fl_has_audio = SWITCH_TRUE;
        }
    } else {
        fl_has_audio = SWITCH_TRUE;
    }

    if(fl_has_audio) {
        if(vad_state == SWITCH_VAD_STATE_START_TALKING && asr_ctx->vad_stored_frames > 0) {
            xdata_buffer_t *tau_buf = NULL;
            const void *ptr = NULL;
            switch_size_t vblen = 0;
            uint32_t rframes = 0, rlen = 0;
            int ofs = 0;

            if((vblen = switch_buffer_peek_zerocopy(asr_ctx->vad_buffer, &ptr)) && ptr && vblen > 0) {
                rframes = (asr_ctx->vad_stored_frames >= VAD_RECOVERY_FRAMES ? VAD_RECOVERY_FRAMES : (asr_ctx->fl_vad_first_cycle ? asr_ctx->vad_stored_frames : VAD_RECOVERY_FRAMES));
                rlen = (rframes * asr_ctx->frame_len);
                ofs = (vblen - rlen);

                if(ofs < 0) {
                    uint32_t hdr_sz = -ofs;
                    uint32_t hdr_ofs = (asr_ctx->vad_buffer_size - hdr_sz);

                    switch_zmalloc(tau_buf, sizeof(xdata_buffer_t));

                    tau_buf->len = (hdr_sz + vblen + data_len);
                    switch_malloc(tau_buf->data, tau_buf->len);

                    memcpy(tau_buf->data, (void *)(ptr + hdr_ofs), hdr_sz);
                    memcpy(tau_buf->data + hdr_sz , (void *)(ptr + 0), vblen);
                    memcpy(tau_buf->data + rlen, data, data_len);

                    if(switch_queue_trypush(asr_ctx->q_audio, tau_buf) != SWITCH_STATUS_SUCCESS) {
                        xdata_buffer_free(&tau_buf);
                    }

                    switch_buffer_zero(asr_ctx->vad_buffer);
                    asr_ctx->vad_stored_frames = 0;
                } else {
                    switch_zmalloc(tau_buf, sizeof(xdata_buffer_t));

                    tau_buf->len = (rlen + data_len);
                    switch_malloc(tau_buf->data, tau_buf->len);

                    memcpy(tau_buf->data, (void *)ptr, rlen);
                    memcpy(tau_buf->data + rlen, data, data_len);

                    if(switch_queue_trypush(asr_ctx->q_audio, tau_buf) != SWITCH_STATUS_SUCCESS) {
                        xdata_buffer_free(&tau_buf);
                    }

                    switch_buffer_zero(asr_ctx->vad_buffer);
                    asr_ctx->vad_stored_frames = 0;
                }
            }
        } else {
            xdata_buffer_push(asr_ctx->q_audio, data, data_len);
        }
    }

    return SWITCH_STATUS_SUCCESS;
}

static switch_status_t asr_check_results(switch_asr_handle_t *ah, switch_asr_flag_t *flags) {
    wasr_ctx_t *asr_ctx = (wasr_ctx_t *)ah->private_info;

    assert(asr_ctx != NULL);

    return (asr_ctx->transcript_results > 0 ? SWITCH_STATUS_SUCCESS : SWITCH_STATUS_FALSE);
}

static switch_status_t asr_get_results(switch_asr_handle_t *ah, char **xmlstr, switch_asr_flag_t *flags) {
    wasr_ctx_t *asr_ctx = (wasr_ctx_t *)ah->private_info;
    char *result = NULL;
    void *pop = NULL;

    assert(asr_ctx != NULL);

    if(switch_queue_trypop(asr_ctx->q_text, &pop) == SWITCH_STATUS_SUCCESS) {
        xdata_buffer_t *tbuff = (xdata_buffer_t *)pop;
        if(tbuff->len > 0) {
            switch_zmalloc(result, tbuff->len + 1);
            memcpy(result, tbuff->data, tbuff->len);
        }
        xdata_buffer_free(&tbuff);

        switch_mutex_lock(asr_ctx->mutex);
        if(asr_ctx->transcript_results > 0) asr_ctx->transcript_results--;
        switch_mutex_unlock(asr_ctx->mutex);
    }

    *xmlstr = result;
    return (result ? SWITCH_STATUS_SUCCESS : SWITCH_STATUS_FALSE);
    return SWITCH_STATUS_FALSE;
}

static switch_status_t asr_start_input_timers(switch_asr_handle_t *ah) {
    wasr_ctx_t *asr_ctx = (wasr_ctx_t *)ah->private_info;

    assert(asr_ctx != NULL);

    //
    // todo
    //

    return SWITCH_STATUS_SUCCESS;
}


static switch_status_t asr_pause(switch_asr_handle_t *ah) {
    wasr_ctx_t *asr_ctx = (wasr_ctx_t *)ah->private_info;

    assert(asr_ctx != NULL);

    if(!asr_ctx->fl_pause) {
        asr_ctx->fl_pause = SWITCH_TRUE;
    }

    return SWITCH_STATUS_SUCCESS;
}

static switch_status_t asr_resume(switch_asr_handle_t *ah) {
    wasr_ctx_t *asr_ctx = (wasr_ctx_t *)ah->private_info;

    assert(asr_ctx != NULL);

    if(asr_ctx->fl_pause) {
        asr_ctx->fl_pause = SWITCH_FALSE;
    }

    return SWITCH_STATUS_SUCCESS;
}

static void asr_text_param(switch_asr_handle_t *ah, char *param, const char *val) {
    wasr_ctx_t *asr_ctx = (wasr_ctx_t *) ah->private_info;

    assert(asr_ctx != NULL);

    switch_mutex_lock(asr_ctx->mutex);

    if(strcasecmp(param, "vad") == 0) {
        if(val) asr_ctx->fl_vad_enabled = switch_true(val);
    } else if(strcasecmp(param, "lang") == 0) {
        if(val) asr_ctx->lang = switch_core_strdup(ah->memory_pool, val);
    } else if(!strcasecmp(param, "tokens")) {
        if(val) asr_ctx->whisper_max_tokens = atoi (val);
    } else if(!strcasecmp(param, "translate")) {
        if(val) asr_ctx->whisper_translate = switch_true(val);
    } else if(!strcasecmp(param, "single-segment")) {
        if(val) asr_ctx->whisper_single_segment = switch_true(val);
    }

    switch_mutex_unlock(asr_ctx->mutex);
}

static void asr_numeric_param(switch_asr_handle_t *ah, char *param, int val) {
}

static void asr_float_param(switch_asr_handle_t *ah, char *param, double val) {
}

static switch_status_t asr_load_grammar(switch_asr_handle_t *ah, const char *grammar, const char *name) {
    return SWITCH_STATUS_SUCCESS;
}

static switch_status_t asr_unload_grammar(switch_asr_handle_t *ah, const char *name) {
    return SWITCH_STATUS_SUCCESS;
}

// ---------------------------------------------------------------------------------------------------------------------------------------------
// main
// ---------------------------------------------------------------------------------------------------------------------------------------------
SWITCH_MODULE_LOAD_FUNCTION(mod_whisper_asr_load) {
    switch_status_t status = SWITCH_STATUS_SUCCESS;
    switch_xml_t cfg, xml, settings, param;
    switch_asr_interface_t *asr_interface;

    memset(&globals, 0, sizeof(globals));
    switch_mutex_init(&globals.mutex, SWITCH_MUTEX_NESTED, pool);

    if((xml = switch_xml_open_cfg(MOD_CONFIG_NAME, &cfg, NULL)) == NULL) {
        switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "Unable to open configuration: %s\n", MOD_CONFIG_NAME);
        switch_goto_status(SWITCH_STATUS_GENERR, out);
    }

    if((settings = switch_xml_child(cfg, "settings"))) {
        for (param = switch_xml_child(settings, "param"); param; param = param->next) {
            char *var = (char *) switch_xml_attr_soft(param, "name");
            char *val = (char *) switch_xml_attr_soft(param, "value");

            if(!strcasecmp(var, "vad-silence-ms")) {
                if(val) globals.vad_silence_ms = atoi (val);
            } else if(!strcasecmp(var, "vad-voice-ms")) {
                if(val) globals.vad_voice_ms = atoi (val);
            } else if(!strcasecmp(var, "vad-threshold")) {
                if(val) globals.vad_threshold = atoi (val);
            } else if(!strcasecmp(var, "vad-enable")) {
                if(val) globals.fl_vad_enabled = switch_true(val);
            } else if(!strcasecmp(var, "vad-debug")) {
                if(val) globals.fl_vad_debug = switch_true(val);
            } else if(!strcasecmp(var, "model")) {
                if(val) globals.model_file = switch_core_strdup(pool, val);
            } else if(!strcasecmp(var, "chunk-time-sec")) {
                if(val) globals.chunk_time_sec = atoi (val);
            } else if(!strcasecmp(var, "whisper-n-threads")) {
                if(val) globals.whisper_n_threads = atoi (val);
            } else if(!strcasecmp(var, "whisper-max-tokens")) {
                if(val) globals.whisper_max_tokens = atoi (val);
            } else if(!strcasecmp(var, "whisper-use-gpu")) {
                if(val) globals.whisper_use_gpu = switch_true(val);
            } else if(!strcasecmp(var, "whisper-flash-attn")) {
                if(val) globals.whisper_flash_attn = switch_true(val);
            } else if(!strcasecmp(var, "whisper-gpu-dev")) {
                if(val) globals.whisper_gpu_dev = atoi (val);
            }
        }
    }

    if(!globals.model_file) {
        switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "Invalid parameter: model\n");
        switch_goto_status(SWITCH_STATUS_GENERR, out);
    }
    if(switch_file_exists(globals.model_file, NULL) != SWITCH_STATUS_SUCCESS) {
        switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "Model not found: %s\n", globals.model_file);
        switch_goto_status(SWITCH_STATUS_GENERR, out);
    }
    if(!globals.chunk_time_sec) {
        globals.chunk_time_sec = DEF_CHUNK_TIME;
    }

    globals.whisper_n_threads = (globals.whisper_n_threads ? globals.whisper_n_threads : 16);

    *module_interface = switch_loadable_module_create_module_interface(pool, modname);
    asr_interface = switch_loadable_module_create_interface(*module_interface, SWITCH_ASR_INTERFACE);
    asr_interface->interface_name = "whisper";
    asr_interface->asr_open = asr_open;
    asr_interface->asr_close = asr_close;
    asr_interface->asr_feed = asr_feed;
    asr_interface->asr_pause = asr_pause;
    asr_interface->asr_resume = asr_resume;
    asr_interface->asr_check_results = asr_check_results;
    asr_interface->asr_get_results = asr_get_results;
    asr_interface->asr_start_input_timers = asr_start_input_timers;
    asr_interface->asr_text_param = asr_text_param;
    asr_interface->asr_numeric_param = asr_numeric_param;
    asr_interface->asr_float_param = asr_float_param;
    asr_interface->asr_load_grammar = asr_load_grammar;
    asr_interface->asr_unload_grammar = asr_unload_grammar;

    switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_NOTICE, "WhisperASR (%s) [%s]\n", MOD_VERSION, whisper_print_system_info());
out:
    if(xml) {
        switch_xml_free(xml);
    }
    return status;
}

SWITCH_MODULE_SHUTDOWN_FUNCTION(mod_whisper_asr_shutdown) {
    uint8_t fl_wloop = SWITCH_TRUE;

    globals.fl_shutdown = SWITCH_TRUE;

    switch_mutex_lock(globals.mutex);
    fl_wloop = (globals.active_threads > 0);
    switch_mutex_unlock(globals.mutex);

    if(fl_wloop) {
        switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_WARNING, "Waiting for termination (%d) threads...\n", globals.active_threads);
        while(fl_wloop) {
            switch_mutex_lock(globals.mutex);
            fl_wloop = (globals.active_threads > 0);
            switch_mutex_unlock(globals.mutex);
            switch_yield(100000);
        }
    }

    return SWITCH_STATUS_SUCCESS;
}
