#include <cstdio>
#include <string>
#include <cstring>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "esp_system.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "driver/gpio.h"
#include "mqtt_client.h"
#include "esp_timer.h"

// ========== 配置区域（已保持您的配置，并适配ESP32-C3引脚） ==========
const char* ssid          = "zh5501";
const char* password      = "lj83759355!!";
// EMQX 8883 默认是 SSL 加密端口，ESP-IDF 连 8883 需要证书。
// 建议先使用 mqtt:// 配合非加密端口 1883。这里先为你指向你的服务器。
const char* mqtt_server   = "mqtt://u1617077.ala.asia-southeast1.emqxsl.com:8883"; 
const char* mqtt_user     = "hjzzn";     
const char* mqtt_password = "netzzn";   

// ESP32-C3 只有 0~21 号引脚，已为您更换为 C3 可用的引脚
const gpio_num_t optoPin  = GPIO_NUM_5;     // 连接到光耦输入端的引脚（控制开机）
const gpio_num_t statePin = GPIO_NUM_4;     // 连接到电脑USB 5V的引脚（检测状态）

// MQTT Topics
const std::string topic_cmd  = "cmnd/pc/power";     // 接收控制指令的主题
const std::string topic_stat = "tele/pc/state";    // 发送状态反馈的主题

// 定时汇报状态的相关变量
unsigned long lastStatusCheck = 0;
const unsigned long checkInterval = 2000;   // 每 2 秒检查并汇报一次状态
std::string lastState = "";                 // 记录上一次的状态
// ==================================================================

static const char *TAG = "PC_CONTROL";
static esp_mqtt_client_handle_t client = NULL;
static bool mqtt_connected = false;

// WiFi 事件标志
#define WIFI_CONNECTED_BIT BIT0
static EventGroupHandle_t s_wifi_event_group;

// 获取毫秒级时间戳（替代 Arduino 的 millis()）
unsigned long millis() {
    return (unsigned long)(esp_timer_get_time() / 1000);
}

// 获取电脑当前真实状态
std::string getComputerState() {
    if (gpio_get_level(statePin) == 1) {
        return "ON";
    } else {
        return "OFF";
    }
}

// 强制立即向 MQTT 汇报当前状态
void reportCurrentState() {
    if (!mqtt_connected || client == NULL) return;
    std::string currentState = getComputerState();
    
    // 发送 Retain 消息
    esp_mqtt_client_publish(client, topic_stat.c_str(), currentState.c_str(), 0, 1, 1);
    lastState = currentState;
    
    ESP_LOGI(TAG, "Reported state to MQTT: %s", currentState.c_str());
}

// 模拟按下电源键的异步任务
void trigger_power_task(void *pvParameters) {
    uint32_t duration = (uint32_t)(uintptr_t)pvParameters;
    gpio_set_level(optoPin, 1);
    vTaskDelay(pdMS_TO_TICKS(duration));
    gpio_set_level(optoPin, 0);
    
    vTaskDelay(pdMS_TO_TICKS(1500));
    reportCurrentState();
    vTaskDelete(NULL);
}

// 收到 MQTT 消息时的回调函数
static void mqtt_event_handler(void *handler_args, esp_event_base_t base, int32_t event_id, void *event_data) {
    auto event = static_cast<esp_mqtt_event_handle_t>(event_data);
    switch (static_cast<esp_mqtt_event_id_t>(event_id)) {
        case MQTT_EVENT_CONNECTED:
            ESP_LOGI(TAG, "connected");
            mqtt_connected = true;
            esp_mqtt_client_subscribe(client, topic_cmd.c_str(), 1);
            reportCurrentState();
            break;
        case MQTT_EVENT_DISCONNECTED:
            ESP_LOGI(TAG, "MQTT disconnected");
            mqtt_connected = false;
            break;
        case MQTT_EVENT_DATA: {
            std::string topic(event->topic, event->topic_len);
            std::string messageTemp(event->data, event->data_len);
            
            ESP_LOGI(TAG, "Message arrived [%s] %s", topic.c_str(), messageTemp.c_str());

            if (topic == topic_cmd) {
                if (messageTemp == "ON" || messageTemp == "TOGGLE" || messageTemp == "PRESS") {
                    ESP_LOGI(TAG, "Triggering Power Button...");
                    xTaskCreate(trigger_power_task, "pwr_task", 2048, (void*)(uintptr_t)500, 5, NULL);
                } 
                else if (messageTemp == "FORCE_OFF") {
                    ESP_LOGI(TAG, "Triggering Force Hard Shutdown...");
                    xTaskCreate(trigger_power_task, "pwr_task", 2048, (void*)(uintptr_t)5000, 5, NULL);
                }
            }
            break;
        }
        default:
            break;
    }
}

// WiFi 事件处理
static void wifi_event_handler(void* arg, esp_event_base_t event_base, int32_t event_id, void* event_data) {
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        esp_wifi_connect();
        ESP_LOGI(TAG, "Attempting MQTT connection... try again in 5 seconds");
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        auto event = static_cast<ip_event_got_ip_t*>(event_data);
        ESP_LOGI(TAG, "WiFi connected. IP address: " IPSTR, IP2STR(&event->ip_info.ip));
        xEventGroupSetBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
    }
}

// 初始化 WiFi 
void setup_wifi() {
    ESP_LOGI(TAG, "Connecting to WiFi...");
    s_wifi_event_group = xEventGroupCreate();
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, NULL, NULL));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &wifi_event_handler, NULL, NULL));

    wifi_config_t wifi_config = {};
    std::strcpy(reinterpret_cast<char*>(wifi_config.sta.ssid), ssid);
    std::strcpy(reinterpret_cast<char*>(wifi_config.sta.password), password);

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
    ESP_ERROR_CHECK(esp_wifi_start());

    xEventGroupWaitBits(s_wifi_event_group, WIFI_CONNECTED_BIT, pdFALSE, pdFALSE, portMAX_DELAY);
}

// 模拟 Arduino 的 setup()
void setup() {
    // 初始化引脚模式
    gpio_config_t io_conf = {};
    io_conf.intr_type = GPIO_INTR_DISABLE;
    io_conf.mode = GPIO_MODE_OUTPUT;
    io_conf.pin_bit_mask = (1ULL << optoPin);
    io_conf.pull_down_en = GPIO_PULLDOWN_DISABLE;
    io_conf.pull_up_en = GPIO_PULLUP_DISABLE;
    gpio_config(&io_conf);
    gpio_set_level(optoPin, 0);  // 默认断开

    io_conf.mode = GPIO_MODE_INPUT;
    io_conf.pin_bit_mask = (1ULL << statePin);
    io_conf.pull_down_en = GPIO_PULLDOWN_ENABLE; // 启用下拉，防止悬空
    gpio_config(&io_conf);

    setup_wifi();

    // 初始化 MQTT
    esp_mqtt_client_config_t mqtt_cfg = {};
    mqtt_cfg.broker.address.uri = mqtt_server;
    mqtt_cfg.credentials.username = mqtt_user;
    mqtt_cfg.credentials.authentication.password = mqtt_password;
    
    client = esp_mqtt_client_init(&mqtt_cfg);
    esp_mqtt_client_register_event(client, ESP_EVENT_ANY_ID, mqtt_event_handler, NULL);
    esp_mqtt_client_start(client);
}

// 模拟 Arduino 的 loop() 的后台 FreeRTOS 任务
void loop_task(void *pvParameters) {
    while (true) {
        // 定时检查状态（非阻塞式定时器）
        unsigned long now = millis();
        if (now - lastStatusCheck > checkInterval) {
            lastStatusCheck = now;
            
            std::string currentState = getComputerState();
            // 只有当状态发生改变时，才向 MQTT 发送数据
            if (currentState != lastState) {
                reportCurrentState();
            }
        }
        vTaskDelay(pdMS_TO_TICKS(100)); // 释放 CPU，每 100ms 检查一次时间轴
    }
}

// ESP-IDF 引导总入口
extern "C" void app_main(void) {
    // 初始化 NVS
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    // 执行初始化
    setup();

    // 创建后台任务来跑原本 loop() 里的内容
    xTaskCreate(loop_task, "loop_task", 3072, NULL, 4, NULL);
}