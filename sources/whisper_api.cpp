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
        WhisperClient(wasr_ctx_t *asr_ctx) {
            asr_ctx_ref = asr_ctx;
            int err = 0;

            if(asr_ctx_ref->samplerate != WHISPER_SAMPLE_RATE) {
                resampler = speex_resampler_init(asr_ctx_ref->channels, asr_ctx_ref->samplerate, WHISPER_SAMPLE_RATE, SWITCH_RESAMPLE_QUALITY, &err);
                if(err != 0) {
                    switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "Resampler init failed: %s\n", speex_resampler_strerror(err));
                    throw std::runtime_error("Couldn't create resampler");
                }
            }

            whisper_ctx = whisper_init_from_file(globals.model_file);
            if(whisper_ctx == NULL) {
                throw std::runtime_error("whisper_ctx == NULL");
            }
        }

        ~WhisperClient() {
            if(whisper_ctx) {
                whisper_free(whisper_ctx);
            }
            if(resampler) {
                speex_resampler_destroy(resampler);
            }

            fl_decoder_active = false;
        }

        int isBusy() {
            return fl_decoder_active;
        }

        switch_status_t transcriptBuffer(switch_byte_t *data, uint32_t data_len, switch_buffer_t *result_buffer) {
            switch_status_t status = SWITCH_STATUS_SUCCESS;
            spx_uint32_t out_len = (rsmp_buffer_size / sizeof(int16_t)); // in samples
            spx_uint32_t in_len = (data_len / sizeof(int16_t));          // in samples

            fl_decoder_active = true;

            switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_DEBUG, "transcriptBuffer: lang=%s, data_len=%u, chunk_time=%u ms, use_parallel=%s, processors=%i, threads=%i\n",
                              asr_ctx_ref->lang, data_len, ((data_len / asr_ctx_ref->frame_len) * asr_ctx_ref->ptime),
                              (asr_ctx_ref->whisper_use_parallel ? "on" : "off"), asr_ctx_ref->whisper_n_processors, asr_ctx_ref->whisper_n_threads
            );

            if(resampler) {
                if(rsmp_buffer_size == 0) {
                    switch_mutex_lock(asr_ctx_ref->mutex);
                    if(asr_ctx_ref->chunk_buffer_size > 0 ) {
                        rsmp_buffer_size = (WHISPER_SAMPLE_RATE * asr_ctx_ref->chunk_buffer_size) / asr_ctx_ref->samplerate;
                        float_buffer_size = (rsmp_buffer_size / sizeof(int16_t)) * sizeof(float);

                        if((rsmp_buffer = (switch_byte_t *) switch_core_alloc(asr_ctx_ref->whisper_thread_memory_pool, rsmp_buffer_size)) == NULL) {
                            switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "mem fail (rsmp_buffer)\n");
                            rsmp_buffer_size = 0;
                        }
                        if((float_buffer = (switch_byte_t *) switch_core_alloc(asr_ctx_ref->whisper_thread_memory_pool, float_buffer_size)) == NULL) {
                            switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "mem fail (float_buffer)\n");
                            float_buffer_size = 0;
                        }
                    }
                    switch_mutex_unlock(asr_ctx_ref->mutex);
                    if(!rsmp_buffer_size || !float_buffer_size) { goto done; }
                }
                //switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_DEBUG, "DEC_RSMP: chunk_buffer_size=%u, rsmp_buffer_size=%u, float_buffer_size=%u\n", asr_ctx_ref->chunk_buffer_size, rsmp_buffer_size, float_buffer_size);

                out_len = (rsmp_buffer_size / sizeof(int16_t));
                speex_resampler_process_interleaved_int(resampler, (const spx_int16_t *) data, (spx_uint32_t *) &in_len, (spx_int16_t *)rsmp_buffer, &out_len);
                conv_i2f((int16_t *)rsmp_buffer, (float *)float_buffer, out_len);
            } else {
                if(float_buffer_size == 0) {
                    switch_mutex_lock(asr_ctx_ref->mutex);
                    if(asr_ctx_ref->chunk_buffer_size > 0 ) {
                        float_buffer_size = (asr_ctx_ref->chunk_buffer_size / sizeof(int16_t)) * sizeof(float);
                        if((float_buffer = (switch_byte_t *) switch_core_alloc(asr_ctx_ref->whisper_thread_memory_pool, float_buffer_size)) == NULL) {
                            switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "mem fail (float_buffer)\n");
                            float_buffer_size = 0;
                        }
                    }
                    switch_mutex_unlock(asr_ctx_ref->mutex);
                    if(!float_buffer_size) { goto done; }
                }

                //switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_DEBUG, "DEC_AS-IS: chunk_buffer_size=%u, rsmp_buffer_size=%u, float_buffer_size=%u\n", asr_ctx_ref->chunk_buffer_size, rsmp_buffer_size, float_buffer_size);

                out_len = in_len;
                conv_i2f((int16_t *)data, (float *)float_buffer, in_len);
            }

            if(out_len > 0) {
                whisper_full_params wparams = whisper_full_default_params(WHISPER_SAMPLING_GREEDY);
                wparams.print_progress   = false;
                wparams.print_special    = false;
                wparams.print_realtime   = false;
                wparams.print_timestamps = false;
                wparams.translate        = asr_ctx_ref->whisper_translate;
                wparams.single_segment   = asr_ctx_ref->whisper_single_segment;
                wparams.max_tokens       = asr_ctx_ref->whisper_max_tokens;
                wparams.n_threads        = asr_ctx_ref->whisper_n_threads;
                wparams.speed_up         = asr_ctx_ref->whisper_speed_up;
                wparams.language         = asr_ctx_ref->lang;
                wparams.audio_ctx        = 0;

                wparams.encoder_begin_callback_user_data = asr_ctx_ref;
                wparams.encoder_begin_callback = (whisper_encoder_begin_callback) WhisperClient::encoder_begin_cb;

                switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_DEBUG, "--> whisper-START\n");

                if(asr_ctx_ref->whisper_use_parallel) {
                    if(whisper_full_parallel(whisper_ctx, wparams, (float *)float_buffer, out_len, asr_ctx_ref->whisper_n_processors) != 0) {
                        switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "whisper_full_parallel() fail\n");
                        goto done;
                    }
                } else {
                    if(whisper_full(whisper_ctx, wparams, (float *)float_buffer, out_len) != 0) {
                        switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "whisper_full() fail\n");
                        goto done;
                    }
                }

                switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_DEBUG, "--> whisper-END\n");

                if(asr_ctx_ref->fl_abort || asr_ctx_ref->fl_destroyed) {
                    goto done;
                }

                const int segments = whisper_full_n_segments(whisper_ctx);
                if(segments > 0) {
                    for(int i = 0; i < segments; ++i) {
                        const char *text = whisper_full_get_segment_text(whisper_ctx, i);
                        switch_buffer_write(result_buffer, text, strlen(text));
                        switch_buffer_write(result_buffer, "\n", 1);
                    }
                }
            }

            done:
            fl_decoder_active = false;
            return status;
        }

    private:
        bool                        fl_decoder_active = false;
        SpeexResamplerState         *resampler = NULL;
        switch_byte_t               *rsmp_buffer = NULL;
        switch_byte_t               *float_buffer = NULL;
        uint32_t                    rsmp_buffer_size = 0;
        uint32_t                    float_buffer_size = 0;
        wasr_ctx_t                  *asr_ctx_ref = NULL;
        struct whisper_context      *whisper_ctx = NULL;

        void conv_i2f(int16_t *in, float *out, uint32_t len) {
            for(uint32_t i = 0; i < len; i++) {
                out[i] = (float) ((in[i] > 0) ? (in[i] / 32767.0) : (in[i] / 32768.0));
            }
        }

        static bool encoder_begin_cb(struct whisper_context *ctx, struct whisper_state *state, void *user_data) {
            wasr_ctx_t *asr_ctx = (wasr_ctx_t *) user_data;
            return (asr_ctx->fl_abort || asr_ctx->fl_destroyed ? false : true);
        }
};

extern "C" {
    switch_status_t whisper_client_init(wasr_ctx_t *asr_ctx) {
        switch_status_t status = SWITCH_STATUS_SUCCESS;
        try {
            asr_ctx->whisper_client = new WhisperClient(asr_ctx);
        } catch (std::exception& e) {
            switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "WhisperClient fail: %s\n", e.what());
            status = SWITCH_STATUS_FALSE;
        }
        return status;
    }

    switch_status_t whisper_client_destroy(wasr_ctx_t *asr_ctx) {
        WhisperClient *whisperClient = (WhisperClient *) asr_ctx->whisper_client;
        if(whisperClient) {
            delete whisperClient;
            asr_ctx->whisper_client = NULL;
        }
        return SWITCH_STATUS_SUCCESS;
    }

    int whisper_client_is_busy(wasr_ctx_t *asr_ctx) {
        WhisperClient *whisperClient = (WhisperClient *) asr_ctx->whisper_client;
        if(whisperClient) {
            return whisperClient->isBusy();
        }
        return false;
    }

    switch_status_t whisper_client_transcript_buffer(wasr_ctx_t *asr_ctx, switch_byte_t *data, uint32_t data_len, switch_buffer_t *result_buffer) {
        WhisperClient *whisperClient = (WhisperClient *) asr_ctx->whisper_client;
        if(whisperClient) {
            return whisperClient->transcriptBuffer(data, data_len, result_buffer);
        }
        return SWITCH_STATUS_FALSE;
    }

}
