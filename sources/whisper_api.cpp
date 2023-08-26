/**
 * (C)2023 aks
 * https://akscf.me/
 * https://github.com/akscf/
 **/
#include <stdexcept>
#include "common.h"
#include "whisper.h"
#include "mod_whisper_asr.h"

extern globals_t globals;

class WhisperClient;
class WhisperClient {

    public:
        WhisperClient(wasr_ctx_t *wasr_ctx) {
            wasr_ctx_ref = wasr_ctx;
            int err = 0;

            if(wasr_ctx_ref->samplerate != WHISPER_SAMPLE_RATE) {
                resampler = speex_resampler_init(wasr_ctx_ref->channels, wasr_ctx_ref->samplerate, WHISPER_SAMPLE_RATE, SWITCH_RESAMPLE_QUALITY, &err);
                if(err != 0) {
                    switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "Resampler init failed: %s\n", speex_resampler_strerror(err));
                    throw std::runtime_error("Couldn't create resampler");
                }
                rsmp_buffer_size = 0;
            }

            whisper_ctx = whisper_init_from_file(globals.model_file);
            opt_max_tokens = globals.whisper_max_tokens;
            opt_n_threads = globals.whisper_n_threads;
            opt_n_processors = globals.whisper_n_processors;
            opt_speed_up = globals.whisper_speed_up;
            opt_translate = globals.whisper_translate;
            opt_single_segment = globals.whisper_single_segment;
            opt_use_parallel = globals.whisper_use_parallel;

            // too waste
            if(switch_buffer_create_dynamic(&result_buffer, 1024, 1024, 16384) != SWITCH_STATUS_SUCCESS) {
                switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "switch_buffer_create_dynamic() fail\n");
                throw std::runtime_error("Couldn't create buffer");
            }

            fl_init_done = true;
        }

        ~WhisperClient() {
            if(whisper_ctx)     { whisper_free(whisper_ctx); }
            if(resampler)       { speex_resampler_destroy(resampler); }
            if(result_buffer)   { switch_buffer_destroy(&result_buffer); }

            fl_decoder_active = false;
        }

        int isReady() {
            return fl_init_done;
        }

        int isBusy() {
            return fl_decoder_active;
        }

        switch_status_t setProperty(char *name, char *val) {
            switch_status_t status = SWITCH_STATUS_SUCCESS;

            if(wasr_ctx_ref->fl_destroyed) {
                return SWITCH_STATUS_FALSE;
            }

            if(strcasecmp(name, "maxTokens") == 0) {
                if(val) { opt_max_tokens = atoi(val); }
            } else if(strcasecmp(name, "threads") == 0) {
                if(val) { opt_n_threads = atoi(val); }
            } else if(strcasecmp(name, "proccesors") == 0) {
                if(val) { opt_n_processors = atoi(val); }
            } else if(strcasecmp(name, "speedUp") == 0) {
                if(val) { opt_speed_up = switch_true(val); }
            } else if(strcasecmp(name, "translate") == 0) {
                if(val) { opt_translate = switch_true(val); }
            } else if(strcasecmp(name, "single-segment") == 0) {
                if(val) { opt_single_segment = switch_true(val); }
            } else if(strcasecmp(name, "use-paralell") == 0) {
                if(val) { opt_use_parallel = switch_true(val); }
            } else {
                status = SWITCH_STATUS_FALSE;
            }

            return status;
        }

        switch_status_t transcriptBuffer(switch_byte_t *data, uint32_t data_len, switch_byte_t **result, uint32_t *result_len) {
            switch_status_t status = SWITCH_STATUS_SUCCESS;
            spx_uint32_t out_len = (rsmp_buffer_size / sizeof(int16_t)); // in samples
            spx_uint32_t in_len = (data_len / sizeof(int16_t));          // in samples

            fl_decoder_active = true;
            switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_DEBUG, "transcriptBuffer: data_len=%u, chunk_time=%u ms, use_parallel=%s, processors=%i, threads=%i\n",
                              data_len, ((data_len / wasr_ctx_ref->frame_len) * wasr_ctx_ref->ptime), opt_use_parallel ? "on" : "off", opt_n_processors, opt_n_threads
            );

            if(resampler) {
                if(rsmp_buffer_size == 0) {
                    if(wasr_ctx_ref->chunk_buf_size == 0) {
                        switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "Couldn't allocate resampling buffer! (chunk_buf_size == 0)\n");
                        switch_goto_status(SWITCH_STATUS_FALSE, done);
                    }
                    rsmp_buffer_size = (WHISPER_SAMPLE_RATE * wasr_ctx_ref->chunk_buf_size) / wasr_ctx_ref->samplerate;
                    if((rsmp_buffer = (switch_byte_t *) switch_core_alloc(wasr_ctx_ref->pool, rsmp_buffer_size)) == NULL) {
                        switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "mem fail (float_buffer)\n");
                        switch_goto_status(SWITCH_STATUS_FALSE, done);
                    }
                    float_buffer_size = (rsmp_buffer_size * sizeof(float));
                    float_buffer = (switch_byte_t *) switch_core_alloc(wasr_ctx_ref->pool, float_buffer_size);
                    if(float_buffer == NULL) {
                        switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "mem fail (float_buffer)\n");
                        switch_goto_status(SWITCH_STATUS_FALSE, done);
                    }
                    out_len = (rsmp_buffer_size / sizeof(int16_t));
                    //switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_DEBUG, "chunk_buf_size: %u, rsmp_buffer_size: %u, float_bufer_size=%u\n", wasr_ctx_ref->chunk_buf_size, rsmp_buffer_size, float_buffer_size);
                }
                speex_resampler_process_interleaved_int(resampler, (const spx_int16_t *) data, (spx_uint32_t *) &in_len, (spx_int16_t *)rsmp_buffer, &out_len);
                conv_i2f((int16_t *)rsmp_buffer, (float *)float_buffer, out_len);
            } else {
                out_len = in_len;
                conv_i2f((int16_t *)data, (float *)float_buffer, in_len);
            }

            // whisper
            if(out_len > 0) {
                whisper_full_params wparams = whisper_full_default_params(WHISPER_SAMPLING_GREEDY);
                wparams.print_progress   = false;
                wparams.print_special    = false;
                wparams.print_realtime   = false;
                wparams.print_timestamps = false;
                wparams.translate        = opt_translate;
                wparams.single_segment   = opt_single_segment;
                wparams.max_tokens       = opt_max_tokens;
                wparams.n_threads        = opt_n_threads;
                wparams.speed_up         = opt_speed_up;
                wparams.language         = wasr_ctx_ref->lang;
                wparams.audio_ctx        = 0;

                wparams.encoder_begin_callback_user_data = wasr_ctx_ref;
                wparams.encoder_begin_callback = (whisper_encoder_begin_callback) WhisperClient::encoder_begin_cb;

                switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_DEBUG, "whisper-transcrip-start\n");

                if(opt_use_parallel) {
                    if(whisper_full_parallel(whisper_ctx, wparams, (float *)float_buffer, out_len, opt_n_processors) != 0) {
                        switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "whisper_full_parallel() failed\n");
                        goto done;
                    }
                } else {
                    if(whisper_full(whisper_ctx, wparams, (float *)float_buffer, out_len) != 0) {
                        switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "whisper_full() failed\n");
                        goto done;
                    }
                }

                if(wasr_ctx_ref->fl_aborted || wasr_ctx_ref->fl_destroyed) {
                    goto done;
                }

                const int segments = whisper_full_n_segments(whisper_ctx);
                switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_DEBUG, "segments=%i\n", segments);

                if(segments > 0) {
                    switch_buffer_zero(result_buffer);
                    for(int i = 0; i < segments; ++i) {
                        const char *text = whisper_full_get_segment_text(whisper_ctx, i);
                        switch_buffer_write(result_buffer, text, strlen(text));
                        switch_buffer_write(result_buffer, "\n", 1);
                    }

                    uint32_t rsz = 0;
                    const void *ptr = NULL;
                    void *lresult = NULL;
                    if((rsz = switch_buffer_peek_zerocopy(result_buffer, &ptr)) > 0) {
                        switch_zmalloc(lresult, rsz);
                        memcpy(lresult, ptr, rsz);
                        *result = (switch_byte_t *)lresult;
                        *result_len = rsz;
                    }
                }

                switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_DEBUG, "whisper-transcrip-end\n");
            }

            done:
            fl_decoder_active = false;
            return status;
        }

    private:
        bool                        fl_decoder_active = false;
        bool                        fl_init_done = false;
        SpeexResamplerState         *resampler = NULL;
        switch_byte_t               *rsmp_buffer = NULL;
        switch_byte_t               *float_buffer = NULL;
        switch_buffer_t             *result_buffer = NULL;
        uint32_t                    rsmp_buffer_size = 0;
        uint32_t                    float_buffer_size = 0;
        wasr_ctx_t                  *wasr_ctx_ref = NULL;
        struct whisper_context      *whisper_ctx = NULL;
        uint32_t                    opt_max_tokens = 0;
        uint32_t                    opt_n_threads = 0;
        uint32_t                    opt_n_processors = 0;
        uint32_t                    opt_speed_up = 0;
        uint32_t                    opt_translate = 0;
        uint32_t                    opt_single_segment = 0;
        uint32_t                    opt_use_parallel = 0;

        // int16 to float
        void conv_i2f(int16_t *in, float *out, uint32_t len) {
            for(uint32_t i = 0; i < len; i++) {
                out[i] = (float) ((in[i] > 0) ? (in[i] / 32767.0) : (in[i] / 32768.0));
            }
        }

        static bool encoder_begin_cb(struct whisper_context *ctx, struct whisper_state *state, void *user_data) {
            wasr_ctx_t *wasr_ctx = (wasr_ctx_t *) user_data;
            return (wasr_ctx->fl_aborted || wasr_ctx->fl_destroyed ? false : true);
        }
};

// ===========================================================================================================================================================================
extern "C" {
    switch_status_t whisper_client_init(wasr_ctx_t *wasr_ctx) {
        switch_status_t status = SWITCH_STATUS_SUCCESS;
        try {
            wasr_ctx->whisper_client = new WhisperClient(wasr_ctx);
        } catch (std::exception& e) {
            switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "WhisperClient fail: %s\n", e.what());
            status = SWITCH_STATUS_FALSE;
        }
        return status;
    }

    switch_status_t whisper_client_destroy(wasr_ctx_t *wasr_ctx) {
        WhisperClient *whisperClient = (WhisperClient *) wasr_ctx->whisper_client;
        if(whisperClient) {
            delete whisperClient;
            wasr_ctx->whisper_client = NULL;
        }
        return SWITCH_STATUS_SUCCESS;
    }

    int whisper_client_is_ready(wasr_ctx_t *wasr_ctx) {
        WhisperClient *whisperClient = (WhisperClient *) wasr_ctx->whisper_client;
        if(whisperClient) {
            return whisperClient->isReady();
        }
        return false;
    }

    int whisper_client_is_busy(wasr_ctx_t *wasr_ctx) {
        WhisperClient *whisperClient = (WhisperClient *) wasr_ctx->whisper_client;
        if(whisperClient) {
            return whisperClient->isBusy();
        }
        return false;
    }

    switch_status_t whisper_client_set_property(wasr_ctx_t *wasr_ctx, char *name, const char *val) {
        WhisperClient *whisperClient = (WhisperClient *) wasr_ctx->whisper_client;
        if(whisperClient) {
            return whisperClient->setProperty(name, (char *) val);
        }
        return SWITCH_STATUS_FALSE;
    }

    switch_status_t whisper_client_transcript_buffer(wasr_ctx_t *wasr_ctx, switch_byte_t *data, uint32_t data_len, switch_byte_t **result, uint32_t *result_len) {
        WhisperClient *whisperClient = (WhisperClient *) wasr_ctx->whisper_client;
        if(whisperClient) {
            return whisperClient->transcriptBuffer(data, data_len, result, result_len);
        }
        return SWITCH_STATUS_FALSE;
    }

}
