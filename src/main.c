#include <stdio.h>
#include <stdbool.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "esp_log.h"

static const char *TAG = "SMART_FAN";

// 1. 数据结构定义
typedef struct {
    float temperature;
    float humidity;
    bool  human_present;
} sensor_data_t;

typedef struct {
    uint8_t speed_percent;
    bool    direction_cw;
} motor_cmd_t;

typedef struct {
    float   temp;
    float   humi;
    bool    human;
    uint8_t speed;
    bool    auto_mode;
} display_info_t;

// 2. 句柄定义
static QueueHandle_t xSensorQueue      = NULL;
static QueueHandle_t xMotorQueue       = NULL;
static SemaphoreHandle_t xDisplayMutex = NULL;
static display_info_t g_display_data   = {0};

// 3. 任务 1：传感器数据采集（周期生产数据）
void Task_Sensor(void *pvParameters)
{
    sensor_data_t sensor_val;
    float mock_temp = 25.0f;

    for (;;) {
        mock_temp += 1.5f;
        if (mock_temp > 35.0f) mock_temp = 25.0f;

        sensor_val.temperature = mock_temp;
        sensor_val.humidity    = 60.0f;
        sensor_val.human_present = true;

        ESP_LOGI("SENSOR", "[Task_Sensor] 生成数据: 温度=%.1f C, 有人=%d", 
                 sensor_val.temperature, sensor_val.human_present);

        xQueueSend(xSensorQueue, &sensor_val, pdMS_TO_TICKS(100));
        vTaskDelay(pdMS_TO_TICKS(2000)); // 挂起 2 秒，释放 CPU
    }
}

// 4. 任务 2：决策控制中枢（事件驱动处理）
void Task_Control(void *pvParameters)
{
    sensor_data_t rx_sensor;
    motor_cmd_t motor_cmd = {0, true};
    bool is_auto_mode = true;

    for (;;) {
        // 阻塞等待传感器数据包到达
        if (xQueueReceive(xSensorQueue, &rx_sensor, portMAX_DELAY) == pdPASS) {
            if (is_auto_mode) {
                if (rx_sensor.human_present && rx_sensor.temperature >= 26.0f) {
                    if (rx_sensor.temperature >= 32.0f) motor_cmd.speed_percent = 100;
                    else if (rx_sensor.temperature >= 28.0f) motor_cmd.speed_percent = 60;
                    else motor_cmd.speed_percent = 30;
                } else {
                    motor_cmd.speed_percent = 0;
                }
            }

            // 调速指令推入电机队列
            xQueueSend(xMotorQueue, &motor_cmd, pdMS_TO_TICKS(50));

            // 互斥锁保护，更新显示结构体
            if (xSemaphoreTake(xDisplayMutex, pdMS_TO_TICKS(50)) == pdTRUE) {
                g_display_data.temp = rx_sensor.temperature;
                g_display_data.humi = rx_sensor.humidity;
                g_display_data.human = rx_sensor.human_present;
                g_display_data.speed = motor_cmd.speed_percent;
                g_display_data.auto_mode = is_auto_mode;
                xSemaphoreGive(xDisplayMutex);
            }
        }
    }
}

// 5. 任务 3：电机执行器（消费控制指令）
void Task_Motor(void *pvParameters)
{
    motor_cmd_t cmd;

    for (;;) {
        if (xQueueReceive(xMotorQueue, &cmd, portMAX_DELAY) == pdPASS) {
            ESP_LOGI("MOTOR", "[Task_Motor] 收到指令: 目标转速 = %d%%", cmd.speed_percent);
        }
    }
}

// 6. 任务 4：OLED 显示刷新（低优先级周期运行）
void Task_Display(void *pvParameters)
{
    display_info_t local_display;

    for (;;) {
        if (xSemaphoreTake(xDisplayMutex, pdMS_TO_TICKS(20)) == pdTRUE) {
            local_display = g_display_data;
            xSemaphoreGive(xDisplayMutex);

            ESP_LOGI("OLED", "[Task_Display] 界面刷新: 温度=%.1f C, 转速=%d%%, 模式=%s",
                     local_display.temp, local_display.speed, 
                     local_display.auto_mode ? "自动" : "手动");
        }

        vTaskDelay(pdMS_TO_TICKS(1000)); // 1 秒刷新一次
    }
}

// 主入口
void app_main(void)
{
    ESP_LOGI(TAG, "==== FreeRTOS 教学工程启动 ====");

    // 创建 IPC 组件
    xSensorQueue  = xQueueCreate(5, sizeof(sensor_data_t));
    xMotorQueue   = xQueueCreate(5, sizeof(motor_cmd_t));
    xDisplayMutex = xSemaphoreCreateMutex();

    // 创建 4 个并发任务
    xTaskCreate(Task_Display, "Task_Display", 2048, NULL, 1, NULL);
    xTaskCreate(Task_Sensor,  "Task_Sensor",  2048, NULL, 2, NULL);
    xTaskCreate(Task_Control, "Task_Control", 3072, NULL, 3, NULL);
    xTaskCreate(Task_Motor,   "Task_Motor",   2048, NULL, 3, NULL);
}