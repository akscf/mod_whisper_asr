/**
 * (C)2023 aks
 * https://akscf.me/
 * https://github.com/akscf/
 **/
#include "mod_whisper_asr.h"
#include "whisper_api.h"

globals_t globals;

SWITCH_MODULE_LOAD_FUNCTION(mod_whisper_asr_load);
SWITCH_MODULE_SHUTDOWN_FUNCTION(mod_whisper_asr_shutdown);
SWITCH_MODULE_DEFINITION(mod_whisper_asr, mod_whisper_asr_load, mod_whisper_asr_shutdown, NULL);

// ---------------------------------------------------------------------------------------------------------------------------------------------
static void *SWITCH_THREAD_FUNC whisper_io_thread(switch_thread_t *thread, void *obj) {
    volatile wasr_ctx_t *_ref = (wasr_ctx_t *) obj;
    wasr_ctx_t *wasr_ctx = (wasr_ctx_t *) _ref;
    switch_byte_t *chunk_buffer = NULL;
    uint32_t chunk_buf_offset = 0, chunk_buf_size = 0;
    uint8_t fl_do_transcript = false;
    void *pop = NULL;

    switch_mutex_lock(wasr_ctx->mutex);
    wasr_ctx->deps++;
    switch_mutex_unlock(wasr_ctx->mutex);

    while(true) {
        if(globals.fl_shutdown || wasr_ctx->fl_destroyed ) {
            break;
        }
        if(chunk_buf_size == 0 || chunk_buffer == NULL) {
            switch_mutex_lock(wasr_ctx->mutex);
            chunk_buf_size = wasr_ctx->chunk_buf_size;
            chunk_buffer = wasr_ctx->chunk_buf;
            switch_mutex_unlock(wasr_ctx->mutex);
            goto timer_next;
        }

        fl_do_transcript = false;
        if(whisper_client_is_ready(wasr_ctx)) {
            while(switch_queue_trypop(wasr_ctx->q_audio, &pop) == SWITCH_STATUS_SUCCESS) {
                xdata_buffer_t *audio_buffer = (xdata_buffer_t *)pop;
                if(globals.fl_shutdown || wasr_ctx->fl_destroyed ) {
                    break;
                }
                if(audio_buffer && audio_buffer->len) {
                    memcpy((chunk_buffer + chunk_buf_offset), audio_buffer->data, audio_buffer->len);
                    chunk_buf_offset += audio_buffer->len;
                    if(chunk_buf_offset >= chunk_buf_size) {
                        chunk_buf_offset = chunk_buf_size;
                        fl_do_transcript = true;
                        break;
                    }
                }
                xdata_buffer_free(audio_buffer);
            }
            if(!fl_do_transcript) {
                fl_do_transcript = (chunk_buf_offset > 0 && wasr_ctx->vad_state == SWITCH_VAD_STATE_STOP_TALKING);
            }
        }
        if(fl_do_transcript) {
            xdata_buffer_t *tbuff = NULL;
            switch_byte_t *result_txt = NULL;
            uint32_t result_len = 0;

            if(whisper_client_transcript_buffer(wasr_ctx, chunk_buffer, chunk_buf_offset, &result_txt, &result_len) != SWITCH_STATUS_SUCCESS) {
                switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "transcript faild\n");
            }
            chunk_buf_offset = 0;

            if(result_len > 0) {
                if(xdata_buffer_alloc(&tbuff, result_txt, result_len) == SWITCH_STATUS_SUCCESS) {
                    if(switch_queue_trypush(wasr_ctx->q_text, tbuff) == SWITCH_STATUS_SUCCESS) {
                        switch_mutex_lock(wasr_ctx->mutex);
                        wasr_ctx->transcript_results++;
                        switch_mutex_unlock(wasr_ctx->mutex);
                    } else {
                        xdata_buffer_free(tbuff);
                    }
                }
            }
        }
        timer_next:
        switch_yield(10000);
    }
out:
    switch_mutex_lock(wasr_ctx->mutex);
    if(wasr_ctx->deps > 0) wasr_ctx->deps--;
    switch_mutex_unlock(wasr_ctx->mutex);

    thread_finished();

    return NULL;
}

// ---------------------------------------------------------------------------------------------------------------------------------------------
static switch_status_t asr_open(switch_asr_handle_t *ah, const char *codec, int samplerate, const char *dest, switch_asr_flag_t *flags) {
    switch_status_t status = SWITCH_STATUS_SUCCESS;
    wasr_ctx_t *wasr_ctx = NULL;

    switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_DEBUG, "asr_open: codec=%s, samplerate=%i, dest=%s, vad=%i\n", codec, samplerate, dest, (globals.fl_vad_enabled));

    if(strcmp(codec, "L16") !=0) {
        switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "Unsupported codec: %s\n", codec);
        switch_goto_status(SWITCH_STATUS_FALSE, out);
    }

    // pre-conf
    wasr_ctx = switch_core_alloc(ah->memory_pool, sizeof(wasr_ctx_t));
    wasr_ctx->pool = ah->memory_pool;
    wasr_ctx->samplerate = samplerate;
    wasr_ctx->ptime = 0;
    wasr_ctx->channels = 1;
    wasr_ctx->lang = switch_core_strdup(ah->memory_pool, globals.default_language);

   if((status = switch_mutex_init(&wasr_ctx->mutex, SWITCH_MUTEX_NESTED, ah->memory_pool)) != SWITCH_STATUS_SUCCESS) {
        switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "mem fail\n");
        switch_goto_status(SWITCH_STATUS_GENERR, out);
    }
    switch_queue_create(&wasr_ctx->q_audio, QUEUE_SIZE, ah->memory_pool);
    switch_queue_create(&wasr_ctx->q_text, QUEUE_SIZE, ah->memory_pool);

    // VAD
    wasr_ctx->fl_vad_enabled = globals.fl_vad_enabled;
    wasr_ctx->frame_len = 0;
    wasr_ctx->vad_fr_buf = NULL;
    wasr_ctx->vad_fr_buf_size = 0; // will be calculated in the feed stage

    if((wasr_ctx->vad = switch_vad_init(wasr_ctx->samplerate, wasr_ctx->channels)) == NULL) {
        switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "Couldn't init VAD\n");
        switch_goto_status(SWITCH_STATUS_GENERR, out);
    }

    switch_vad_set_mode(wasr_ctx->vad, -1);
    if(globals.vad_silence_ms > 0) { switch_vad_set_param(wasr_ctx->vad, "silence_ms", globals.vad_silence_ms); }
    if(globals.vad_voice_ms > 0) { switch_vad_set_param(wasr_ctx->vad, "voice_ms", globals.vad_voice_ms); }
    if(globals.vad_threshold > 0) { switch_vad_set_param(wasr_ctx->vad, "thresh", globals.vad_threshold); }

    // chunk buffer
    wasr_ctx->chunk_buf = NULL;
    wasr_ctx->chunk_buf_size = 0;

    // WhisperClient
    status = whisper_client_init(wasr_ctx);
    if(status != SWITCH_STATUS_SUCCESS) {
        switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "Couldn't init whisper_client\n");
        switch_goto_status(SWITCH_STATUS_GENERR, out);
    }

    ah->private_info = wasr_ctx;

    // start a helper thread
    thread_launch(wasr_ctx->pool, whisper_io_thread, wasr_ctx);
out:
    return status;
}

static switch_status_t asr_close(switch_asr_handle_t *ah, switch_asr_flag_t *flags) {
    wasr_ctx_t *wasr_ctx = (wasr_ctx_t *) ah->private_info;
    int32_t wc = 0;
    assert(wasr_ctx != NULL);

    wasr_ctx->fl_aborted = true;
    wasr_ctx->fl_destroyed = true;
    while(true) {
        if(wasr_ctx->deps <= 0 && whisper_client_is_busy(wasr_ctx) == false) {
            break;
        }
        switch_yield(100000);
        if(++wc % 100 == 0) {
            if(wasr_ctx->deps > 0) {
                switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_WARNING, "wasr_ctx is used (deps=%u)!\n", wasr_ctx->deps);
            }
            if(whisper_client_is_busy(wasr_ctx)) {
                switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_WARNING, "whisper_client is busy!\n");
            }
            wc = 0;
        }
    }

    whisper_client_destroy(wasr_ctx);

    if(wasr_ctx->q_audio) {
        xdata_buffer_queue_clean(wasr_ctx->q_audio);
        switch_queue_term(wasr_ctx->q_audio);
    }
    if(wasr_ctx->q_text) {
        xdata_buffer_queue_clean(wasr_ctx->q_text);
        switch_queue_term(wasr_ctx->q_text);
    }

    switch_set_flag(ah, SWITCH_ASR_FLAG_CLOSED);

    return SWITCH_STATUS_SUCCESS;
}

static switch_status_t asr_feed(switch_asr_handle_t *ah, void *data, unsigned int data_len, switch_asr_flag_t *flags) {
    wasr_ctx_t *wasr_ctx = (wasr_ctx_t *) ah->private_info;
    switch_status_t status = SWITCH_STATUS_SUCCESS;
    switch_vad_state_t vad_state = 0;
    uint8_t fl_has_audio = false;

    assert(wasr_ctx != NULL);

    if(switch_test_flag(ah, SWITCH_ASR_FLAG_CLOSED)) {
        return SWITCH_STATUS_BREAK;
    }
    if(wasr_ctx->fl_pause) {
        return SWITCH_STATUS_SUCCESS;
    }

    if(data_len > 0 && wasr_ctx->frame_len == 0) {
        switch_mutex_lock(wasr_ctx->mutex);

        wasr_ctx->frame_len = data_len;
        wasr_ctx->ptime = (data_len / sizeof(int16_t)) / (wasr_ctx->samplerate / 1000);
        wasr_ctx->chunk_buf_size = ((globals.chunk_size_max_sec * 1000) * data_len) / wasr_ctx->ptime;
        wasr_ctx->vad_fr_buf_size = (wasr_ctx->frame_len * VAD_STORE_FRAMES);

        if((wasr_ctx->vad_fr_buf = switch_core_alloc(ah->memory_pool, wasr_ctx->vad_fr_buf_size)) == NULL) {
            wasr_ctx->vad_fr_buf_size = 0; // force disable
            switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "mem fail (vad_fr_buf)\n");
        }
        if((wasr_ctx->chunk_buf = switch_core_alloc(ah->memory_pool, wasr_ctx->chunk_buf_size)) == NULL) {
            wasr_ctx->chunk_buf_size = 0; // force disable
            switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "mem fail (chunk_buf)\n");
        }

        switch_mutex_unlock(wasr_ctx->mutex);
        //switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_DEBUG, "frame_len=%u, ptime=%u, vad_buffer_size=%u, chunk_buf_size=%u\n", data_len, wasr_ctx->ptime, wasr_ctx->vad_fr_buf_size, wasr_ctx->chunk_buf_size);
    }

    if(wasr_ctx->fl_vad_enabled) {
        if(wasr_ctx->vad_state == SWITCH_VAD_STATE_STOP_TALKING || (wasr_ctx->vad_state == vad_state && vad_state == SWITCH_VAD_STATE_NONE)) {
            if(wasr_ctx->vad_fr_buf_size && wasr_ctx->frame_len >= data_len) {
                wasr_ctx->vad_fr_buf_ofs = (wasr_ctx->vad_fr_buf_ofs >= wasr_ctx->vad_fr_buf_size) ? 0 : wasr_ctx->vad_fr_buf_ofs;
                memcpy((void *)(wasr_ctx->vad_fr_buf + wasr_ctx->vad_fr_buf_ofs), data, MIN(wasr_ctx->frame_len, data_len));
                wasr_ctx->vad_fr_buf_ofs += wasr_ctx->frame_len;
            }
        }
        vad_state = switch_vad_process(wasr_ctx->vad, (int16_t *)data, (data_len / sizeof(int16_t)) );
        if(vad_state == SWITCH_VAD_STATE_START_TALKING) {
            wasr_ctx->vad_state = vad_state;
            fl_has_audio = true;
        } else if (vad_state == SWITCH_VAD_STATE_STOP_TALKING) {
            switch_vad_reset(wasr_ctx->vad);
            wasr_ctx->vad_state = vad_state;
            fl_has_audio = false;
        } else if (vad_state == SWITCH_VAD_STATE_TALKING) {
            wasr_ctx->vad_state = vad_state;
            fl_has_audio = true;
        }
    } else {
        fl_has_audio = true;
    }

    if(fl_has_audio) {
        xdata_buffer_t *abuf = NULL;
        // lost frames
        if(vad_state == SWITCH_VAD_STATE_START_TALKING && wasr_ctx->vad_fr_buf_ofs > 0) {
            wasr_ctx->vad_fr_buf_ofs -= (wasr_ctx->frame_len * VAD_RECOVER_FRAMES);
            if(wasr_ctx->vad_fr_buf_ofs < 0 ) { wasr_ctx->vad_fr_buf_ofs = 0; }

            for(int i = 0; i < VAD_RECOVER_FRAMES; i++) {
                if(xdata_buffer_alloc(&abuf, (void *)(wasr_ctx->vad_fr_buf + wasr_ctx->vad_fr_buf_ofs), wasr_ctx->frame_len) == SWITCH_STATUS_SUCCESS) {
                    if(switch_queue_trypush(wasr_ctx->q_audio, abuf) != SWITCH_STATUS_SUCCESS) {
                        xdata_buffer_free(abuf);
                        break;
                    }
                }
                wasr_ctx->vad_fr_buf_ofs += wasr_ctx->frame_len;
                if(wasr_ctx->vad_fr_buf_ofs >= wasr_ctx->vad_fr_buf_size) { break; }
            }
            wasr_ctx->vad_fr_buf_ofs = 0;
        }
        // current frame
        if(xdata_buffer_alloc(&abuf, data, data_len) == SWITCH_STATUS_SUCCESS) {
            if(switch_queue_trypush(wasr_ctx->q_audio, abuf) != SWITCH_STATUS_SUCCESS) {
                xdata_buffer_free(abuf);
            }
        }
    }

    return SWITCH_STATUS_SUCCESS;
}

static switch_status_t asr_check_results(switch_asr_handle_t *ah, switch_asr_flag_t *flags) {
    wasr_ctx_t *wasr_ctx = (wasr_ctx_t *) ah->private_info;
    switch_status_t status = SWITCH_STATUS_FALSE;

    assert(wasr_ctx != NULL);

    return (wasr_ctx->transcript_results > 0 ? SWITCH_STATUS_SUCCESS : SWITCH_STATUS_FALSE);
}

static switch_status_t asr_get_results(switch_asr_handle_t *ah, char **xmlstr, switch_asr_flag_t *flags) {
    wasr_ctx_t *wasr_ctx = (wasr_ctx_t *) ah->private_info;
    char *result = NULL;
    void *pop = NULL;

    assert(wasr_ctx != NULL);

    if(switch_queue_trypop(wasr_ctx->q_text, &pop) == SWITCH_STATUS_SUCCESS) {
        xdata_buffer_t *tbuff = (xdata_buffer_t *)pop;
        if(tbuff->len > 0) {
            switch_zmalloc(result, tbuff->len + 1);
            memcpy(result, tbuff->data, tbuff->len);
        }
        xdata_buffer_free(tbuff);

        switch_mutex_lock(wasr_ctx->mutex);
        if(wasr_ctx->transcript_results > 0) wasr_ctx->transcript_results--;
        switch_mutex_unlock(wasr_ctx->mutex);
    }

    *xmlstr = result;
    return (result ? SWITCH_STATUS_SUCCESS : SWITCH_STATUS_FALSE);
}

static switch_status_t asr_start_input_timers(switch_asr_handle_t *ah) {
    wasr_ctx_t *wasr_ctx = (wasr_ctx_t *) ah->private_info;
    assert(wasr_ctx != NULL);

    switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_DEBUG, "ASR_START_INPUT_TIMER (todo)\n");

    return SWITCH_STATUS_SUCCESS;
}


static switch_status_t asr_pause(switch_asr_handle_t *ah) {
    wasr_ctx_t *wasr_ctx = (wasr_ctx_t *) ah->private_info;
    assert(wasr_ctx != NULL);

    if(!wasr_ctx->fl_pause) {
        wasr_ctx->fl_pause = true;
    }

    return SWITCH_STATUS_SUCCESS;
}

static switch_status_t asr_resume(switch_asr_handle_t *ah) {
    wasr_ctx_t *wasr_ctx = (wasr_ctx_t *) ah->private_info;
    assert(wasr_ctx != NULL);

    if(wasr_ctx->fl_pause) {
        wasr_ctx->fl_pause = false;
    }

    return SWITCH_STATUS_SUCCESS;
}

static void asr_text_param(switch_asr_handle_t *ah, char *param, const char *val) {
    wasr_ctx_t *wasr_ctx = (wasr_ctx_t *) ah->private_info;
    assert(wasr_ctx != NULL);

    if(strcasecmp(param, "vad") == 0) {
        wasr_ctx->fl_vad_enabled = switch_true(val);
        return;
    }
    if(strcasecmp(param, "lang") == 0) {
        if(val) { wasr_ctx->lang = switch_core_strdup(ah->memory_pool, val); }
    }

    whisper_client_set_property(wasr_ctx, param, val);
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
#define CONFIG_NAME "whisper_asr.conf"
SWITCH_MODULE_LOAD_FUNCTION(mod_whisper_asr_load) {
    switch_status_t status = SWITCH_STATUS_SUCCESS;
    switch_xml_t cfg, xml, settings, param;
    switch_asr_interface_t *asr_interface;

    memset(&globals, 0, sizeof(globals));

    globals.pool = pool;
    switch_mutex_init(&globals.mutex, SWITCH_MUTEX_NESTED, pool);

    if((xml = switch_xml_open_cfg(CONFIG_NAME, &cfg, NULL)) == NULL) {
        switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "Couldn't open configuration file: %s\n", CONFIG_NAME);
        switch_goto_status(SWITCH_STATUS_GENERR, out);
    }

    if((settings = switch_xml_child(cfg, "settings"))) {
        for (param = switch_xml_child(settings, "param"); param; param = param->next) {
            char *var = (char *) switch_xml_attr_soft(param, "name");
            char *val = (char *) switch_xml_attr_soft(param, "value");

            if(!strcasecmp(var, "vad-silence-ms")) {
                globals.vad_silence_ms = atoi (val);
            } else if(!strcasecmp(var, "vad-voice-ms")) {
                globals.vad_voice_ms = atoi (val);
            } else if(!strcasecmp(var, "vad-threshold")) {
                globals.vad_threshold = atoi (val);
            } else if(!strcasecmp(var, "vad-enable")) {
                globals.fl_vad_enabled = switch_true(val);
            } else if(!strcasecmp(var, "default-language")) {
                globals.default_language = switch_core_strdup(pool, val);
            } else if(!strcasecmp(var, "model")) {
                globals.model_file = switch_core_strdup(pool, val);
            } else if(!strcasecmp(var, "chunk-size-max-sec")) {
                globals.chunk_size_max_sec = atoi (val);
            } else if(!strcasecmp(var, "whisper-n-threads")) {
                globals.whisper_n_threads = atoi (val);
            } else if(!strcasecmp(var, "whisper-max-tokens")) {
                globals.whisper_tokens = atoi (val);
            } else if(!strcasecmp(var, "whisper-speed-up")) {
                globals.whisper_speed_up = switch_true(val);
            } else if(!strcasecmp(var, "whisper-translate")) {
                globals.whisper_translate = switch_true(val);
            } else if(!strcasecmp(var, "whisper-single-segment")) {
                globals.whisper_single_segment = switch_true(val);
            } else if(!strcasecmp(var, "whisper-n-processors")) {
                globals.whisper_n_processors = atoi (val);
            } else if(!strcasecmp(var, "whisper-use-parallel")) {
                globals.whisper_use_parallel = switch_true(val);
            }
        }
    }

    if(!globals.model_file) {
        switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "Invalid parameter: model\n");
        switch_goto_status(SWITCH_STATUS_GENERR, out);
    }
    if(switch_file_exists(globals.model_file, NULL) != SWITCH_STATUS_SUCCESS) {
        switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "Model not found (%s)\n", globals.model_file);
        switch_goto_status(SWITCH_STATUS_GENERR, out);
    }
    if(!globals.chunk_size_max_sec) {
        globals.chunk_size_max_sec = CHUNK_MAX_LEN_SEC;
    }

    globals.default_language = globals.default_language != NULL ? globals.default_language : "en";
    globals.whisper_tokens = globals.whisper_tokens > 0 ? globals.whisper_tokens : WHISPER_DEFAULT_TOKENS;
    globals.whisper_n_threads = globals.whisper_n_threads >= WHISPER_DEFAULT_THREADS ? globals.whisper_n_threads : WHISPER_DEFAULT_THREADS;
    globals.whisper_n_processors = globals.whisper_n_processors > 1 ? globals.whisper_n_processors : 1;

    // -------------------------
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

    switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_NOTICE, "WhisperASR-%s\n", VERSION);
out:
    if(xml) {
        switch_xml_free(xml);
    }
    return status;
}

SWITCH_MODULE_SHUTDOWN_FUNCTION(mod_whisper_asr_shutdown) {

    globals.fl_shutdown = true;

    while(globals.active_threads > 0) {
        switch_yield(100000);
        if(globals.fl_shutdown) { break; }
    }

    return SWITCH_STATUS_SUCCESS;
}
