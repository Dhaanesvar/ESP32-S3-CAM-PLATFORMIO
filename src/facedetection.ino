#include "camera_settings.h"
#include "face_detection.hpp"
#include "iic_data_send.hpp"
#include "WiFi.h"
#include "img_converters.h"

static QueueHandle_t xQueueAIFrame = NULL;
static QueueHandle_t xQueueIICData = NULL;
static QueueHandle_t xQueueStreamFrame = NULL;

static SemaphoreHandle_t xJpegMutex = NULL;
static uint8_t *sLatestJpeg = NULL;
static size_t sLatestJpegLen = 0;

static WiFiServer streamServer(80);

static const char *kApSsid = "ESP32S3-FACE-CAM";
static const char *kApPassword = "12345678";

static const char *kIndexHtml =
"<!doctype html><html><head><meta charset='utf-8'>"
"<meta name='viewport' content='width=device-width,initial-scale=1'>"
"<title>ESP32S3 Face Cam</title>"
"<style>body{font-family:Arial,sans-serif;background:#101820;color:#f2f2f2;text-align:center;margin:0;padding:20px;}"
"h1{font-size:22px;margin:8px 0 6px;}p{margin:0 0 14px;color:#b9c6d2;}img{width:100%;max-width:720px;border:2px solid #2dd4bf;border-radius:12px;box-shadow:0 8px 24px rgba(0,0,0,.35);}</style>"
"</head><body><h1>ESP32S3 Face Cam Live Stream</h1><p>Open from AP: ESP32S3-FACE-CAM</p><img src='/stream' alt='Live Stream'></body></html>";

static void update_latest_jpeg(uint8_t *jpeg_buf, size_t jpeg_len)
{
  if (xSemaphoreTake(xJpegMutex, portMAX_DELAY) == pdTRUE)
  {
    if (sLatestJpeg)
    {
      free(sLatestJpeg);
      sLatestJpeg = NULL;
      sLatestJpegLen = 0;
    }

    sLatestJpeg = jpeg_buf;
    sLatestJpegLen = jpeg_len;
    xSemaphoreGive(xJpegMutex);
  }
  else
  {
    free(jpeg_buf);
  }
}

static void task_stream_collector(void *arg)
{
  while (true)
  {
    camera_fb_t *frame = NULL;
    if (xQueueReceive(xQueueStreamFrame, &frame, portMAX_DELAY))
    {
      if (!frame)
      {
        continue;
      }

      uint8_t *jpeg_buf = NULL;
      size_t jpeg_len = 0;
      bool converted = false;

      if (frame->format == PIXFORMAT_JPEG)
      {
        jpeg_len = frame->len;
        jpeg_buf = (uint8_t *)malloc(jpeg_len);
        if (jpeg_buf)
        {
          memcpy(jpeg_buf, frame->buf, jpeg_len);
          converted = true;
        }
      }
      else
      {
        converted = fmt2jpg(frame->buf,
                            frame->len,
                            frame->width,
                            frame->height,
                            frame->format,
                            80,
                            &jpeg_buf,
                            &jpeg_len);
      }

      if (converted && jpeg_buf && jpeg_len > 0)
      {
        update_latest_jpeg(jpeg_buf, jpeg_len);
      }

      esp_camera_fb_return(frame);
    }
  }
}

static void send_index_page(WiFiClient &client)
{
  client.print("HTTP/1.1 200 OK\r\n");
  client.print("Content-Type: text/html\r\n");
  client.print("Connection: close\r\n\r\n");
  client.print(kIndexHtml);
}

static void stream_mjpeg(WiFiClient &client)
{
  client.print("HTTP/1.1 200 OK\r\n");
  client.print("Content-Type: multipart/x-mixed-replace; boundary=frame\r\n");
  client.print("Cache-Control: no-cache\r\n");
  client.print("Connection: close\r\n\r\n");

  while (client.connected())
  {
    uint8_t *frame_copy = NULL;
    size_t frame_len = 0;

    if (xSemaphoreTake(xJpegMutex, pdMS_TO_TICKS(50)) == pdTRUE)
    {
      if (sLatestJpeg && sLatestJpegLen > 0)
      {
        frame_len = sLatestJpegLen;
        frame_copy = (uint8_t *)malloc(frame_len);
        if (frame_copy)
        {
          memcpy(frame_copy, sLatestJpeg, frame_len);
        }
      }
      xSemaphoreGive(xJpegMutex);
    }

    if (frame_copy && frame_len > 0)
    {
      client.print("--frame\r\n");
      client.print("Content-Type: image/jpeg\r\n");
      client.printf("Content-Length: %u\r\n\r\n", (unsigned int)frame_len);
      client.write(frame_copy, frame_len);
      client.print("\r\n");
      free(frame_copy);
    }

    delay(66);
  }
}

static void task_web_server(void *arg)
{
  streamServer.begin();

  while (true)
  {
    WiFiClient client = streamServer.available();
    if (!client)
    {
      vTaskDelay(pdMS_TO_TICKS(10));
      continue;
    }

    client.setTimeout(200);
    String requestLine = client.readStringUntil('\r');
    client.readStringUntil('\n');

    if (requestLine.startsWith("GET /stream"))
    {
      stream_mjpeg(client);
    }
    else if (requestLine.startsWith("GET / "))
    {
      send_index_page(client);
    }
    else
    {
      client.print("HTTP/1.1 404 Not Found\r\nConnection: close\r\n\r\n");
    }

    client.stop();
  }
}

static void start_ap_mode_and_web()
{
  WiFi.mode(WIFI_AP);
  bool ap_started = WiFi.softAP(kApSsid, kApPassword);
  IPAddress ip = WiFi.softAPIP();

  if (ap_started)
  {
    Serial.printf("[AP] SSID:%s PASS:%s IP:%s\r\n", kApSsid, kApPassword, ip.toString().c_str());
  }
  else
  {
    Serial.println("[AP] Failed to start");
  }
}

void setup() 
{
  Serial.begin(115200);
  delay(200);
  Serial.println("[BOOT] ESP32S3 face detection started");

  xJpegMutex = xSemaphoreCreateMutex();

  /* 创建图像传输队列 */
  xQueueAIFrame = xQueueCreate(2, sizeof(camera_fb_t *)); 
  /* 创建IIC数据传输队列 */
  xQueueIICData = xQueueCreate(2, sizeof(iic_send_data_t *));
  /* 创建Web流媒体图像队列 */
  xQueueStreamFrame = xQueueCreate(2, sizeof(camera_fb_t *));

  /* 注册摄像头处理任务 */
  register_camera(PIXFORMAT_RGB565, FRAMESIZE_240X240, 4, xQueueAIFrame);
  /* 注册人脸检测任务 */
  register_human_face_detection(xQueueAIFrame, NULL, xQueueIICData, xQueueStreamFrame, true);
  /* 注册IIC数据传输任务 */
  register_iic_data_send(xQueueIICData, NULL);

  xTaskCreatePinnedToCore(task_stream_collector, "stream_collector", 8 * 1024, NULL, 4, NULL, 1);

  start_ap_mode_and_web();
  xTaskCreatePinnedToCore(task_web_server, "web_server", 8 * 1024, NULL, 3, NULL, 0);
}


void loop() 
{
  
}