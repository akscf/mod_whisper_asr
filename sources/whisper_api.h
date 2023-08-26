/**
 * (C)2023 aks
 * https://akscf.me/
 * https://github.com/akscf/
 **/
#ifndef __WISPER_API_H
#define __WISPER_API_H

#include <switch.h>

switch_status_t whisper_client_init(wasr_ctx_t *wasr_ctx);
switch_status_t whisper_client_destroy(wasr_ctx_t *wasr_ctx);
switch_status_t whisper_client_set_property(wasr_ctx_t *wasr_ctx, char *name, const char *val);
switch_status_t whisper_client_transcript_buffer(wasr_ctx_t *wasr_ctx, switch_byte_t *data, uint32_t data_len, switch_byte_t **result, uint32_t *result_len);
int whisper_client_is_ready(wasr_ctx_t *wasr_ctx);
int whisper_client_is_busy(wasr_ctx_t *wasr_ctx);

#endif
