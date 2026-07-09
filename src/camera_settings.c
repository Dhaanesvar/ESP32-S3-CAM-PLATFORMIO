#include "camera_settings.h"
#include "esp_log.h"
#include "esp_system.h"

static const char *TAG = "camera";

static QueueHandle_t xQueueFrameO = NULL;

static const char *sensor_name_from_pid(uint16_t pid)
{
    switch (pid)
    {
    case 0x2640:
        return "OV2640";
    case 0x3660:
        return "OV3660";
    case 0x5640:
        return "OV5640";
    case 0x2145:
        return "GC2145";
    case 0x0308:
        return "GC0308";
    case 0x032A:
        return "GC032A";
    default:
        return "UNKNOWN_SENSOR";
    }
}

static void task_process_handler(void *arg)
{
    while (true)
    {
        camera_fb_t *fb = esp_camera_fb_get();
        if (!fb)
        {
            continue;
        }

        if (!xQueueFrameO)
        {
            esp_camera_fb_return(fb);
            continue;
        }

        camera_fb_t *stale_fb = NULL;
        if (xQueueReceive(xQueueFrameO, &stale_fb, 0) == pdTRUE && stale_fb)
        {
            esp_camera_fb_return(stale_fb);
        }

        if (xQueueSend(xQueueFrameO, &fb, 0) != pdTRUE)
        {
            esp_camera_fb_return(fb);
        }
    }
}

void register_camera(const pixformat_t pixel_fromat,
                     const framesize_t frame_size,
                     const uint8_t fb_count,
                     const QueueHandle_t frame_o)

{
    ESP_LOGI(TAG, "Camera module is %s", CAMERA_MODULE_NAME);

#if CONFIG_CAMERA_MODULE_ESP_EYE || CONFIG_CAMERA_MODULE_ESP32_CAM_BOARD
    /* IO13, IO14 is designed for JTAG by default,
     * to use it as generalized input,
     * firstly declair it as pullup input */
    gpio_config_t conf;
    conf.mode = GPIO_MODE_INPUT;
    conf.pull_up_en = GPIO_PULLUP_ENABLE;
    conf.pull_down_en = GPIO_PULLDOWN_DISABLE;
    conf.intr_type = GPIO_INTR_DISABLE;
    conf.pin_bit_mask = 1LL << 13;
    gpio_config(&conf);
    conf.pin_bit_mask = 1LL << 14;
    gpio_config(&conf);
#endif

    camera_config_t config;
    config.ledc_channel = LEDC_CHANNEL_0;
    config.ledc_timer = LEDC_TIMER_0;
    config.pin_d0 = Y2_GPIO_NUM;
    config.pin_d1 = Y3_GPIO_NUM;
    config.pin_d2 = Y4_GPIO_NUM;
    config.pin_d3 = Y5_GPIO_NUM;
    config.pin_d4 = Y6_GPIO_NUM;
    config.pin_d5 = Y7_GPIO_NUM;
    config.pin_d6 = Y8_GPIO_NUM;
    config.pin_d7 = Y9_GPIO_NUM;
    config.pin_xclk = XCLK_GPIO_NUM;
    config.pin_pclk = PCLK_GPIO_NUM;
    config.pin_vsync = VSYNC_GPIO_NUM;
    config.pin_href = HREF_GPIO_NUM;
    config.pin_sccb_sda = SIOD_GPIO_NUM;
    config.pin_sccb_scl = SIOC_GPIO_NUM;
    config.pin_pwdn = PWDN_GPIO_NUM;
    config.pin_reset = RESET_GPIO_NUM;
    config.xclk_freq_hz = XCLK_FREQ_HZ;
    config.frame_size = frame_size;
    config.pixel_format = pixel_fromat; // for streaming
    config.grab_mode = CAMERA_GRAB_LATEST;
    config.fb_location = CAMERA_FB_IN_PSRAM;
    config.jpeg_quality = 16;
    config.fb_count = fb_count;

    // camera init
    esp_err_t err = esp_camera_init(&config);
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "Camera init failed with error 0x%x", err);
        return;
    }

    sensor_t *s = esp_camera_sensor_get();
    if (!s)
    {
        ESP_LOGE(TAG, "esp_camera_sensor_get() returned NULL");
        return;
    }

    ESP_LOGI(TAG, "Detected camera sensor: %s (PID: 0x%04X)", get_camera_sensor_name(), s->id.PID);
    printf("[CAM] Detected camera sensor: %s (PID: 0x%04X)\r\n", get_camera_sensor_name(), s->id.PID);

    if (s->id.PID == OV3660_PID || s->id.PID == OV2640_PID) {
        s->set_vflip(s, 1); // flip it back
    } else if (s->id.PID == GC0308_PID) {
        s->set_hmirror(s, 0);
    } else if (s->id.PID == GC032A_PID) {
        s->set_vflip(s, 1);
    } else if (s->id.PID == GC2145_PID) {
        s->set_vflip(s, 1);
        s->set_hmirror(s, 0);
    }
    
    //initial sensors are flipped vertically and colors are a bit saturated
    if (s->id.PID == OV3660_PID)
    {
        s->set_brightness(s, 1);  //up the blightness just a bit
        s->set_saturation(s, -2); //lower the saturation
    }

    xQueueFrameO = frame_o;
    xTaskCreatePinnedToCore(task_process_handler, TAG, 3 * 1024, NULL, 5, NULL, 1);
}

const char *get_camera_sensor_name(void)
{
    sensor_t *s = esp_camera_sensor_get();
    if (!s)
    {
        ESP_LOGE(TAG, "get_camera_sensor_name: sensor pointer is NULL");
        return "SENSOR_NOT_AVAILABLE";
    }

    return sensor_name_from_pid(s->id.PID);
}