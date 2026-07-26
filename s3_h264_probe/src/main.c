#include <inttypes.h>

#include "esp_heap_caps.h"
#include "esp_h264_dec.h"
#include "esp_h264_dec_sw.h"
#include "esp_log.h"
#include "esp_psram.h"

static const char *TAG = "navdash-h264";

void app_main(void)
{
    const bool psram_ready = esp_psram_is_initialized();
    ESP_LOGI(TAG, "S3_H264_PROBE psram=%s free_internal=%" PRIu32 " free_psram=%" PRIu32,
             psram_ready ? "READY" : "MISSING",
             heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
             heap_caps_get_free_size(MALLOC_CAP_SPIRAM));

    if (!psram_ready) {
        ESP_LOGE(TAG, "STOP: N8R8 PSRAM is required for Royal 526x300 H.264");
        return;
    }

    esp_h264_dec_cfg_sw_t config = {
        .pic_type = ESP_H264_RAW_FMT_I420,
    };
    esp_h264_dec_handle_t decoder = NULL;
    esp_h264_err_t result = esp_h264_dec_sw_new(&config, &decoder);
    ESP_LOGI(TAG, "H264_NEW result=%d handle=%p", result, decoder);
    if (result != ESP_H264_ERR_OK) {
        return;
    }

    result = esp_h264_dec_open(decoder);
    ESP_LOGI(TAG, "H264_OPEN result=%d", result);
    if (result == ESP_H264_ERR_OK) {
        ESP_LOGI(TAG, "READY: decoder accepts Annex-B NAL units; next: RTP FU-A reassembly");
        esp_h264_dec_close(decoder);
    }
    esp_h264_dec_del(decoder);
}
