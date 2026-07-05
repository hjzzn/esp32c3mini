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

// ========== 配置区域 ==========
const char* ssid          = "LightSpeed";
const char* password      = "83653316zzn";

const char* mqtt_server   = "u1617077.ala.asia-southeast1.emqxsl.com";
const uint32_t mqtt_port  = 8883;
const char* mqtt_user     = "hjzzn";     
const char* mqtt_password = "netzzn";   

// 从外部嵌入文件引入证书指针
extern const uint8_t emqxsl_ca_crt_start[] asm("_binary_emqxsl_ca_crt_start");
extern const uint8_t emqxsl_ca_crt_end[]   asm("_binary_emqxsl_ca_crt_end");

// ESP32-C3 引脚配置
const gpio_num_t optoPin  = GPIO_NUM_5;     // 连接到光耦输入端的引脚（控制开机）
const gpio_num_t statePin = GPIO_NUM_4;     // 连接到电脑USB 5V的引脚（检测状态）

// MQTT Topics
const std::string topic_switch_status = "SWITCH/e1784b9a-1f8e-48e1-8fbf-aa37bfd87ba2/STATUS"; // ✨ 已修改变量名
const std::string topic_switch_cmd    = "SWITCH/e1784b9a-1f8e-48e1-8fbf-aa37bfd87ba2/POWER";  // 开关控制主题 

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
    
    // 发送 Retain 消息（使用修改后的变量名 topic_switch_status）
    esp_mqtt_client_publish(client, topic_switch_status.c_str(), currentState.c_str(), 0, 1, 1);
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
            
            // 订阅简化后的唯一控制主题
            esp_mqtt_client_subscribe(client, topic_switch_cmd.c_str(), 1);
            ESP_LOGI(TAG, "Subscribed to topic: %s", topic_switch_cmd.c_str());
            
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

            // 只处理简化后的单一控制主题消息
            if (topic == topic_switch_cmd) {
                if (messageTemp == "ON" || messageTemp == "TOGGLE" || messageTemp == "PRESS") {
                    ESP_LOGI(TAG, "Triggering Power Button via %s...", topic.c_str());
                    xTaskCreate(trigger_power_task, "pwr_task", 2048, (void*)(uintptr_t)500, 5, NULL);
                } 
                else if (messageTemp == "FORCE_OFF") {
                    ESP_LOGI(TAG, "Triggering Force Hard Shutdown via %s...", topic.c_str());
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
    gpio_set_level(optoPin, 0);  

    io_conf.mode = GPIO_MODE_INPUT;
    io_conf.pin_bit_mask = (1ULL << statePin);
    io_conf.pull_down_en = GPIO_PULLDOWN_ENABLE; 
    gpio_config(&io_conf);

    setup_wifi();

    // 初始化 MQTT 结构体
    esp_mqtt_client_config_t mqtt_cfg = {};
    mqtt_cfg.broker.address.hostname = mqtt_server;     
    mqtt_cfg.broker.address.port = mqtt_port;           
    mqtt_cfg.broker.address.transport = MQTT_TRANSPORT_OVER_SSL; 

    // 将外部嵌入的二进制证书内容和长度精准配置进去
    mqtt_cfg.broker.verification.certificate = reinterpret_cast<const char*>(emqxsl_ca_crt_start);
    mqtt_cfg.broker.verification.certificate_len = emqxsl_ca_crt_end - emqxsl_ca_crt_start; 

    mqtt_cfg.credentials.client_id = "my_unique_esp32c3_device_01";
    mqtt_cfg.credentials.username = mqtt_user;
    mqtt_cfg.credentials.authentication.password = mqtt_password;
    
    client = esp_mqtt_client_init(&mqtt_cfg);
    esp_mqtt_client_register_event(client, static_cast<esp_mqtt_event_id_t>(ESP_EVENT_ANY_ID), mqtt_event_handler, NULL);
    esp_mqtt_client_start(client);
}

// 模拟 Arduino 的 loop() 的后台 FreeRTOS 任务
void loop_task(void *pvParameters) {
    while (true) {
        unsigned long now = millis();
        if (now - lastStatusCheck > checkInterval) {
            lastStatusCheck = now;
            
            std::string currentState = getComputerState();
            if (currentState != lastState) {
                reportCurrentState();
            }
        }
        vTaskDelay(pdMS_TO_TICKS(100)); 
    }
}

// ESP-IDF 引导总入口
extern "C" void app_main(void) {
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    setup();
    xTaskCreate(loop_task, "loop_task", 3072, NULL, 4, NULL);
}