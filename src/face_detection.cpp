#include "face_detection.hpp"
#include "esp_log.h"
#include "esp_camera.h"
#include "dl_image.hpp"
#include "human_face_detect_msr01.hpp"
#include "human_face_detect_mnp01.hpp"
#include "who_ai_utils.hpp"

#define TWO_STAGE_ON 1

static const char *TAG = "human_face_detection";

static QueueHandle_t xQueueFrameI = NULL;
static QueueHandle_t xQueueEvent = NULL;
static QueueHandle_t xQueueFrameO = NULL;
static QueueHandle_t xQueueResult = NULL;

static bool gReturnFB = true;

static target_face_information_t detect_result;

static void save_detection_result(std::list<dl::detect::result_t> &results)
{
  int i = 0;
  for (std::list<dl::detect::result_t>::iterator prediction = results.begin(); prediction != results.end(); prediction++, i++)
  {
    if(prediction->keypoint.size() == 10)
    {
      detect_result.center_x = (uint8_t)prediction->box[0];
      detect_result.center_y = (uint8_t)prediction->box[1];
      detect_result.width = (uint8_t)(prediction->box[2] - prediction->box[0]);
      detect_result.length = (uint8_t)(prediction->box[3] - prediction->box[1]);
    }
  }
}

static void task_process_handler(void *arg)
{
  camera_fb_t *frame = NULL;
  uint16_t no_face_frame_count = 0;
  HumanFaceDetectMSR01 detector(0.3F, 0.3F, 10, 0.3F);
#if TWO_STAGE_ON
  HumanFaceDetectMNP01 detector2(0.4F, 0.3F, 10);
#endif

  while (true)
  {
    if (xQueueReceive(xQueueFrameI, &frame, portMAX_DELAY))
    {
#if TWO_STAGE_ON
      std::list<dl::detect::result_t> &detect_candidates = detector.infer((uint16_t *)frame->buf, {(int)frame->height, (int)frame->width, 3});
      std::list<dl::detect::result_t> &detect_results = detector2.infer((uint16_t *)frame->buf, {(int)frame->height, (int)frame->width, 3}, detect_candidates);
#else
      std::list<dl::detect::result_t> &detect_results = detector.infer((uint16_t *)frame->buf, {(int)frame->height, (int)frame->width, 3});
#endif
      if (detect_results.size() > 0)
      {
        no_face_frame_count = 0;
        draw_detection_result((uint16_t *)frame->buf, frame->height, frame->width, detect_results);
        save_detection_result(detect_results);
         printf("center_x:%u, center_y:%u, width:%u, length:%u,\r\n",
           detect_result.center_x,
           detect_result.center_y,
           detect_result.width,
           detect_result.length);
      }
      else
      {
        no_face_frame_count++;
        detect_result.center_x = 0;
        detect_result.center_y = 0;
        detect_result.width = 0;
        detect_result.length = 0;

        if (no_face_frame_count >= 30)
        {
          no_face_frame_count = 0;
          printf("[FACE] none\r\n");
        }
      }
    }

    if (xQueueFrameO)
    {
      camera_fb_t *stale_frame = NULL;
      if (xQueueReceive(xQueueFrameO, &stale_frame, 0) == pdTRUE && stale_frame)
      {
        esp_camera_fb_return(stale_frame);
      }

      if (xQueueSend(xQueueFrameO, &frame, 0) != pdTRUE)
      {
        esp_camera_fb_return(frame);
      }
    }
    else if (gReturnFB)
    {
      esp_camera_fb_return(frame);
    }
    else
    {
      free(frame);
    }
    if (xQueueResult)
    {
      xQueueSend(xQueueResult, &detect_result, portMAX_DELAY);
    }
  }
}

static void task_event_handler(void *arg)
{
  while (true)
  {
  }
}

void register_human_face_detection(const QueueHandle_t frame_i,
                                   const QueueHandle_t event,
                                   const QueueHandle_t result,
                                   const QueueHandle_t frame_o,
                                   const bool camera_fb_return)
{
  xQueueFrameI = frame_i;
  xQueueFrameO = frame_o;
  xQueueEvent = event;
  xQueueResult = result;
  gReturnFB = camera_fb_return;

  xTaskCreatePinnedToCore(task_process_handler, TAG, 5 * 1024, NULL, 5, NULL, 1);
  // xTaskCreatePinnedToCore(task_event_handler, TAG, 4 * 1024, NULL, 5, NULL, 0);
}