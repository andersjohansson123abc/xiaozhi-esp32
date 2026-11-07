#include "solis_monitor.h"

#include "application.h"
#include "board.h"
#include "settings.h"
#include "soliscloud_crypto.h"

#include <esp_heap_caps.h>
#include <esp_log.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <network_interface.h>
#include <sdkconfig.h>
#include <wifi_manager.h>

#include <ctime>

#define TAG "Solis"

namespace {

constexpr char kSettingsNamespace[] = "solis";
constexpr char kKeyIdKey[] = "key_id";
constexpr char kSecretKey[] = "secret";
constexpr char kPlantKey[] = "plant";

constexpr int kTickMs = 1000;
constexpr uint32_t kMonitorStackBytes = 4096;
constexpr uint32_t kFetchStackBytes = 8192;
constexpr UBaseType_t kTaskPriority = 2;
constexpr int kHttpTimeoutMs = 20000;
constexpr int64_t kRetryUs = 60LL * 1000000;

// Measured on this board the lowest free heap is 47-70 KB, and a TLS session
// costs tens of KB on top of the fetch task's stack. Below these limits a fetch
// could starve the audio path, so it waits for the next tick instead.
constexpr size_t kMinFreeHeapBytes = 40 * 1024;
constexpr size_t kMinLargestBlockBytes = 20 * 1024;

// 2020-01-01. Before the OTA check has set the clock, time() is near zero and
// SolisCloud rejects any Date header more than 15 minutes off.
constexpr time_t kEarliestPlausibleTime = 1577836800;

enum class CallResult { kOk, kRejected, kFailed };

std::string DateHeader() {
    const time_t now = time(nullptr);
    tm utc = {};
    gmtime_r(&now, &utc);
    char text[40];
    strftime(text, sizeof(text), "%a, %d %b %Y %H:%M:%S GMT", &utc);
    return text;
}

template <typename Parser>
CallResult Call(const std::string& key_id, const std::string& secret, std::string_view resource,
                std::string body, Parser& parser) {
    const std::string content_md5 = soliscloud::ContentMd5(body);
    const std::string date = DateHeader();
    const std::string sign = soliscloud::Sign(secret, content_md5, date, resource);

    auto http = Board::GetInstance().GetNetwork()->CreateHttp(0);
    http->SetTimeout(kHttpTimeoutMs);
    http->SetKeepAlive(false);
    http->SetHeader("Content-MD5", content_md5);
    http->SetHeader("Content-Type", std::string(soliscloud::kContentType));
    http->SetHeader("Date", date);
    http->SetHeader("Authorization", "API " + key_id + ":" + sign);
    http->SetContent(std::move(body));

    const std::string url = CONFIG_SOLIS_MONITOR_API_URL + std::string(resource);
    if (auto opened = http->Open("POST", url); !opened) {
        ESP_LOGW(TAG, "Connection failed: %s", opened.error().ToString().c_str());
        return CallResult::kFailed;
    }
    auto status = http->GetStatusCode();
    if (!status) {
        http->Close();
        return CallResult::kFailed;
    }
    if (*status != 200) {
        ESP_LOGW(TAG, "HTTP status %d", *status);
        http->Close();
        return *status == 401 || *status == 403 ? CallResult::kRejected : CallResult::kFailed;
    }

    char buffer[256];
    while (true) {
        auto read = http->Read(buffer, sizeof(buffer));
        if (!read) {
            http->Close();
            return CallResult::kFailed;
        }
        if (*read <= 0) {
            break;
        }
        parser.Feed(buffer, *read);
    }
    http->Close();
    return CallResult::kOk;
}

}  // namespace

SolisMonitor::SolisMonitor() {
    Settings settings(kSettingsNamespace);
    credentials_.key_id = settings.GetString(kKeyIdKey);
    credentials_.secret = settings.GetString(kSecretKey);
    credentials_.plant_id = settings.GetString(kPlantKey);
    health_ = credentials_.secret.empty() ? SolisHealth::kNoKey : SolisHealth::kWaiting;
}

void SolisMonitor::Start(TickCallback on_tick, AlarmCallback on_alarm) {
    on_tick_ = std::move(on_tick);
    on_alarm_ = std::move(on_alarm);
    if (xTaskCreate(TaskEntry, "solis", kMonitorStackBytes, this, kTaskPriority, nullptr) !=
        pdPASS) {
        ESP_LOGE(TAG, "Monitor task could not be started");
    }
}

bool SolisMonitor::IsConfigured() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return !credentials_.key_id.empty() && !credentials_.secret.empty();
}

bool SolisMonitor::SetCredentials(const std::string& key_id, const std::string& secret,
                                  const std::string& plant_id) {
    if (key_id.empty() || secret.empty()) {
        return false;
    }
    {
        Settings settings(kSettingsNamespace, true);
        settings.SetString(kKeyIdKey, key_id);
        settings.SetString(kSecretKey, secret);
        settings.SetString(kPlantKey, plant_id);
    }
    {
        std::lock_guard<std::mutex> lock(mutex_);
        credentials_ = {key_id, secret, plant_id};
        resolved_plant_id_.clear();
        has_reading_ = false;
        health_ = SolisHealth::kWaiting;
    }
    fetch_now_ = true;
    return true;
}

void SolisMonitor::ForgetCredentials() {
    {
        Settings settings(kSettingsNamespace, true);
        settings.EraseAll();
    }
    std::lock_guard<std::mutex> lock(mutex_);
    credentials_ = {};
    resolved_plant_id_.clear();
    reading_ = {};
    has_reading_ = false;
    health_ = SolisHealth::kNoKey;
}

SolisStatus SolisMonitor::GetStatus() const {
    std::lock_guard<std::mutex> lock(mutex_);
    SolisStatus status;
    status.reading = reading_;
    status.has_reading = has_reading_;
    status.health = health_;
    if (has_reading_ && reading_.data_time_ms > 0 && ClockIsSet()) {
        const int64_t age = static_cast<int64_t>(time(nullptr)) - reading_.data_time_ms / 1000;
        status.data_age_seconds = age < 0 ? 0 : static_cast<int>(age);
    }
    return status;
}

void SolisMonitor::TaskEntry(void* arg) {
    static_cast<SolisMonitor*>(arg)->Run();
}

void SolisMonitor::FetchEntry(void* arg) {
    auto* monitor = static_cast<SolisMonitor*>(arg);
    monitor->Fetch();
    ESP_LOGI(TAG, "Fetch done, %u bytes of stack to spare, lowest free heap %u",
             static_cast<unsigned>(uxTaskGetStackHighWaterMark(nullptr)),
             static_cast<unsigned>(heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL)));
    monitor->fetching_ = false;
    vTaskDelete(nullptr);
}

bool SolisMonitor::ClockIsSet() const {
    return time(nullptr) >= kEarliestPlausibleTime;
}

bool SolisMonitor::CanFetchNow() const {
    if (!WifiManager::GetInstance().IsConnected() || !ClockIsSet()) {
        return false;
    }
    // A handshake mid-sentence would compete with the audio path.
    if (Application::GetInstance().GetDeviceState() != kDeviceStateIdle) {
        return false;
    }
    const size_t free_heap = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    const size_t largest = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL);
    if (free_heap < kMinFreeHeapBytes || largest < kMinLargestBlockBytes) {
        ESP_LOGW(TAG, "Fetch postponed: %u free, %u largest block",
                 static_cast<unsigned>(free_heap), static_cast<unsigned>(largest));
        return false;
    }
    return true;
}

void SolisMonitor::Run() {
    while (true) {
        const bool due = fetch_now_ || esp_timer_get_time() >= next_fetch_us_;
        if (due && !fetching_ && IsConfigured() && CanFetchNow()) {
            fetch_now_ = false;
            fetching_ = true;
            if (xTaskCreate(FetchEntry, "solis_fetch", kFetchStackBytes, this, kTaskPriority,
                            nullptr) != pdPASS) {
                fetching_ = false;
                ESP_LOGW(TAG, "No memory for the fetch task");
                next_fetch_us_ = esp_timer_get_time() + kRetryUs;
            }
        }
        if (on_tick_) {
            on_tick_(GetStatus());
        }
        vTaskDelay(pdMS_TO_TICKS(kTickMs));
    }
}

void SolisMonitor::Fetch() {
    Credentials credentials;
    std::string plant_id;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        credentials = credentials_;
        plant_id = credentials.plant_id.empty() ? resolved_plant_id_ : credentials.plant_id;
    }

    if (plant_id.empty()) {
        soliscloud::StationListParser list;
        const CallResult result =
            Call(credentials.key_id, credentials.secret, soliscloud::kStationListResource,
                 std::string(soliscloud::kStationListBody), list);
        if (result != CallResult::kOk || !list.ok()) {
            Commit(result == CallResult::kFailed ? SolisHealth::kUnreachable
                                                 : SolisHealth::kRejected,
                   nullptr);
            return;
        }
        if (list.id().empty()) {
            Commit(SolisHealth::kNoPlant, nullptr);
            return;
        }
        plant_id = std::string(list.id());
        std::lock_guard<std::mutex> lock(mutex_);
        resolved_plant_id_ = plant_id;
    }

    soliscloud::StationDetailParser detail;
    const CallResult result =
        Call(credentials.key_id, credentials.secret, soliscloud::kStationDetailResource,
             "{\"id\":\"" + plant_id + "\"}", detail);
    if (result != CallResult::kOk || !detail.ok()) {
        Commit(result == CallResult::kFailed ? SolisHealth::kUnreachable : SolisHealth::kRejected,
               nullptr);
        return;
    }
    const soliscloud::Reading reading = detail.reading();
    Commit(SolisHealth::kOk, &reading);
}

void SolisMonitor::Commit(SolisHealth health, const soliscloud::Reading* reading) {
    bool alarm_began = false;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        // ForgetCredentials() may have run while this fetch was on the wire.
        if (credentials_.secret.empty()) {
            return;
        }
        health_ = health;
        if (reading != nullptr) {
            alarm_began = has_reading_ && reading_.state != soliscloud::PlantState::kAlarm &&
                          reading->state == soliscloud::PlantState::kAlarm;
            reading_ = *reading;
            has_reading_ = true;
        }
    }

    const int64_t delay_us = health == SolisHealth::kUnreachable
                                 ? kRetryUs
                                 : static_cast<int64_t>(CONFIG_SOLIS_MONITOR_POLL_SECONDS) * 1000000;
    next_fetch_us_ = esp_timer_get_time() + delay_us;

    if (alarm_began && on_alarm_) {
        on_alarm_();
    }
}
