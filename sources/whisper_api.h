/**
 * (C)2023 aks
 * https://akscf.me/
 * https://github.com/akscf/
 **/
#ifndef MOD_WHISPER_API_H
#define MOD_WHISPER_API_H

#include <switch.h>

switch_status_t whisper_client_init(wasr_ctx_t *asr_ctx);
switch_status_t whisper_client_destroy(wasr_ctx_t *asr_ctx);
int whisper_client_is_busy(wasr_ctx_t *asr_ctx);
switch_status_t whisper_client_transcript_buffer(wasr_ctx_t *asr_ctx, switch_byte_t *data, uint32_t data_len, switch_buffer_t *result_buffer);


#endif
