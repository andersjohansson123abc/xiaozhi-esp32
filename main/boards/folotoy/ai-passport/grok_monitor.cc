#include "grok_monitor.h"

#include "application.h"
#include "board.h"
#include "passport_tls_gate.h"
#include "settings.h"

#include <esp_heap_caps.h>
#include <esp_log.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <network_interface.h>
#include <sdkconfig.h>
#include <wifi_manager.h>

#include <algorithm>

#define TAG "Grok"

namespace {

constexpr char kSettingsNamespace[] = "grok";
constexpr char kUrlKey[] = "url";
constexpr char kTokenKey[] = "token";

constexpr int kTickMs = 1000;
// More than SolisMonitor: the tick renders the page, whose reply text wraps.
constexpr uint32_t kMonitorStackBytes = 5120;
constexpr uint32_t kWorkStackBytes = 8192;
constexpr UBaseType_t kTaskPriority = 2;
constexpr int kHttpTimeoutMs = 20000;
constexpr int64_t kRetryUs = 60LL * 1000000;
constexpr int64_t kSendRetryUs = 30LL * 1000000;
constexpr size_t kMaxOutbox = 3;
constexpr int kMaxSendAttempts = 3;

// The same limits as SolisMonitor: below them a handshake could starve the audio
// path, so the round waits for a later tick.
constexpr size_t kMinFreeHeapBytes = 40 * 1024;
constexpr size_t kMinLargestBlockBytes = 20 * 1024;

auto OpenHttp(const std::string& token) {
    auto http = Board::GetInstance().GetNetwork()->CreateHttp(0);
    http->SetTimeout(kHttpTimeoutMs);
    http->SetKeepAlive(false);
    http->SetHeader("Authorization", "Bearer " + token);
    return http;
}

}  // namespace

GrokMonitor::GrokMonitor() {
    Settings settings(kSettingsNamespace);
    credentials_.relay_url = settings.GetString(kUrlKey);
    credentials_.token = settings.GetString(kTokenKey);
    health_ = credentials_.token.empty() ? GrokHealth::kNoToken : GrokHealth::kWaiting;
}

void GrokMonitor::Start(TickCallback on_tick, ReplyCallback on_reply, SendCallback on_send) {
    on_tick_ = std::move(on_tick);
    on_reply_ = std::move(on_reply);
    on_send_ = std::move(on_send);
    if (xTaskCreate(TaskEntry, "grok", kMonitorStackBytes, this, kTaskPriority, nullptr) !=
        pdPASS) {
        ESP_LOGE(TAG, "Monitor task could not be started");
    }
}

bool GrokMonitor::IsConfigured() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return !credentials_.relay_url.empty() && !credentials_.token.empty();
}

bool GrokMonitor::SetCredentials(const std::string& relay_url, const std::string& token) {
    const std::string url = grok::NormalizeRelayUrl(relay_url);
    if (url.empty() || token.empty()) {
        return false;
    }
    {
        Settings settings(kSettingsNamespace, true);
        settings.SetString(kUrlKey, url);
        settings.SetString(kTokenKey, token);
    }
    {
        std::lock_guard<std::mutex> lock(mutex_);
        credentials_ = {url, token};
        poll_ = {};
        has_poll_ = false;
        health_ = GrokHealth::kWaiting;
    }
    poll_now_ = true;
    return true;
}

void GrokMonitor::ForgetCredentials() {
    {
        Settings settings(kSettingsNamespace, true);
        settings.EraseAll();
    }
    std::lock_guard<std::mutex> lock(mutex_);
    credentials_ = {};
    poll_ = {};
    has_poll_ = false;
    outbox_.clear();
    health_ = GrokHealth::kNoToken;
}

GrokStatus GrokMonitor::GetStatus() const {
    std::lock_guard<std::mutex> lock(mutex_);
    GrokStatus status;
    status.poll = poll_;
    status.has_poll = has_poll_;
    status.health = health_;
    status.pending_sends = static_cast<int>(outbox_.size());
    if (has_poll_) {
        status.poll_age_seconds =
            static_cast<int>((esp_timer_get_time() - poll_time_us_) / 1000000);
    }
    return status;
}

bool GrokMonitor::QueueMessage(const std::string& bot_id, const std::string& bot_name,
                               const std::string& text) {
    if (text.empty() || text.size() > grok::kMaxOutgoingBytes) {
        return false;
    }
    std::lock_guard<std::mutex> lock(mutex_);
    if (outbox_.size() >= kMaxOutbox) {
        return false;
    }
    outbox_.push_back({bot_id, bot_name, text, 0});
    return true;
}

std::vector<grok::Message> GrokMonitor::TakeReplies() {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<grok::Message> replies = std::move(poll_.messages);
    poll_.messages.clear();
    for (const grok::Message& reply : replies) {
        acked_id_ = std::max(acked_id_, reply.id);
    }
    poll_.unread = std::max(0, poll_.unread - static_cast<int>(replies.size()));
    if (!replies.empty()) {
        poll_now_ = true;
    }
    return replies;
}

void GrokMonitor::TaskEntry(void* arg) {
    static_cast<GrokMonitor*>(arg)->Run();
}

void GrokMonitor::WorkEntry(void* arg) {
    auto* monitor = static_cast<GrokMonitor*>(arg);
    monitor->Work();
    ESP_LOGI(TAG, "Round done, %u bytes of stack to spare, lowest free heap %u",
             static_cast<unsigned>(uxTaskGetStackHighWaterMark(nullptr)),
             static_cast<unsigned>(heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL)));
    ReleaseTlsTurn();
    monitor->working_ = false;
    vTaskDelete(nullptr);
}

bool GrokMonitor::HeapAllowsTls() const {
    const size_t free_heap = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    const size_t largest = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL);
    if (free_heap < kMinFreeHeapBytes || largest < kMinLargestBlockBytes) {
        ESP_LOGW(TAG, "Round postponed: %u free, %u largest block",
                 static_cast<unsigned>(free_heap), static_cast<unsigned>(largest));
        return false;
    }
    return true;
}

void GrokMonitor::Run() {
    while (true) {
        const int64_t now = esp_timer_get_time();
        const DeviceState state = Application::GetInstance().GetDeviceState();
        const bool idle = state == kDeviceStateIdle;

        bool has_send = false;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            has_send = !outbox_.empty();
        }
        // A conversation can stay open for minutes, so a message may leave while the
        // assistant waits for speech. It may not leave while it talks, and polling,
        // which nobody is waiting for, waits for idle.
        const bool send_due = has_send && now >= next_send_us_ &&
                              (idle || state == kDeviceStateListening);
        const bool poll_due = idle && (poll_now_ || now >= next_poll_us_);

        if ((send_due || poll_due) && !working_ && IsConfigured() &&
            WifiManager::GetInstance().IsConnected() && HeapAllowsTls() && TryTakeTlsTurn()) {
            working_ = true;
            if (xTaskCreate(WorkEntry, "grok_net", kWorkStackBytes, this, kTaskPriority,
                            nullptr) != pdPASS) {
                working_ = false;
                ReleaseTlsTurn();
                ESP_LOGW(TAG, "No memory for the network task");
                next_poll_us_ = now + kRetryUs;
                next_send_us_ = now + kSendRetryUs;
            }
        }
        if (on_tick_) {
            on_tick_();
        }
        vTaskDelay(pdMS_TO_TICKS(kTickMs));
    }
}

void GrokMonitor::Work() {
    Credentials credentials;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        credentials = credentials_;
    }
    if (credentials.token.empty()) {
        return;
    }

    if (SendNext(credentials)) {
        // The Bot's state has changed, and this may be the only round for minutes.
        poll_now_ = true;
    }
    const bool idle = Application::GetInstance().GetDeviceState() == kDeviceStateIdle;
    if (idle && (poll_now_ || esp_timer_get_time() >= next_poll_us_)) {
        poll_now_ = false;
        PollRelay(credentials);
    }
}

// Returns whether a message left, in which case a poll shows the Bot's new state.
bool GrokMonitor::SendNext(const Credentials& credentials) {
    Outgoing item;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (outbox_.empty() || esp_timer_get_time() < next_send_us_) {
            return false;
        }
        item = outbox_.front();
    }

    auto http = OpenHttp(credentials.token);
    http->SetHeader("Content-Type", "application/x-www-form-urlencoded");
    http->SetContent("bot=" + grok::UrlEncode(item.bot_id) +
                     "&text=" + grok::UrlEncode(item.text));

    int status = 0;
    if (auto opened = http->Open("POST", credentials.relay_url + "/device/send"); opened) {
        if (auto code = http->GetStatusCode(); code) {
            status = *code;
        }
        http->Close();
    } else {
        ESP_LOGW(TAG, "Send failed: %s", opened.error().ToString().c_str());
    }
    ESP_LOGI(TAG, "Message to %s: HTTP %d", item.bot_id.c_str(), status);

    bool finished = true;
    GrokSendResult result = GrokSendResult::kRefused;
    if (status == 200) {
        result = GrokSendResult::kSent;
    } else if (status == 429) {
        result = GrokSendResult::kLimited;
    } else if (status == 0 || status >= 500) {
        // A 502 is the Bot's webhook refusing, which a retry cannot change.
        if (status != 502 && item.attempts + 1 < kMaxSendAttempts) {
            finished = false;
        } else if (status != 502) {
            result = GrokSendResult::kFailed;
        }
    }

    if (!finished) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!outbox_.empty()) {
            outbox_.front().attempts++;
        }
        next_send_us_ = esp_timer_get_time() + kSendRetryUs;
        return false;
    }
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!outbox_.empty()) {
            outbox_.pop_front();
        }
    }
    if (status == 401 || status == 403) {
        CommitFailure(GrokHealth::kRejected);
    }
    if (on_send_) {
        on_send_(item.bot_name, result);
    }
    return result == GrokSendResult::kSent;
}

void GrokMonitor::PollRelay(const Credentials& credentials) {
    uint32_t ack = 0;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        ack = acked_id_;
    }
    std::string url = credentials.relay_url + "/device/poll";
    if (ack > 0) {
        url += "?ack=" + std::to_string(ack);
    }

    auto http = OpenHttp(credentials.token);
    if (auto opened = http->Open("GET", url); !opened) {
        ESP_LOGW(TAG, "Connection failed: %s", opened.error().ToString().c_str());
        CommitFailure(GrokHealth::kUnreachable);
        return;
    }
    auto status = http->GetStatusCode();
    if (!status || *status != 200) {
        const int code = status ? *status : 0;
        ESP_LOGW(TAG, "Poll status %d", code);
        http->Close();
        CommitFailure(code == 401 || code == 403 ? GrokHealth::kRejected
                                                 : GrokHealth::kUnreachable);
        return;
    }

    std::string body;
    body.reserve(1024);
    char buffer[256];
    while (true) {
        auto read = http->Read(buffer, sizeof(buffer));
        if (!read) {
            http->Close();
            CommitFailure(GrokHealth::kUnreachable);
            return;
        }
        const int count = *read;
        if (count <= 0) {
            break;
        }
        if (body.size() + count > grok::kMaxResponseBytes) {
            http->Close();
            CommitFailure(GrokHealth::kUnreachable);
            return;
        }
        body.append(buffer, count);
    }
    http->Close();

    grok::Poll poll;
    if (!grok::ParsePoll(body, poll)) {
        ESP_LOGW(TAG, "Answer not understood (%u bytes)", static_cast<unsigned>(body.size()));
        CommitFailure(GrokHealth::kUnreachable);
        return;
    }
    CommitPoll(std::move(poll));
}

void GrokMonitor::CommitPoll(grok::Poll&& poll) {
    std::string new_reply_from;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        // ForgetCredentials() may have run while this poll was on the wire.
        if (credentials_.token.empty()) {
            return;
        }
        // Replies taken after this poll left must not come back.
        std::erase_if(poll.messages,
                      [this](const grok::Message& message) { return message.id <= acked_id_; });
        for (const grok::Message& message : poll.messages) {
            if (message.id > announced_id_) {
                announced_id_ = message.id;
                new_reply_from = message.bot_id;
                for (const grok::Bot& bot : poll.bots) {
                    if (bot.id == message.bot_id) {
                        new_reply_from = bot.name;
                    }
                }
            }
        }
        poll_ = std::move(poll);
        has_poll_ = true;
        poll_time_us_ = esp_timer_get_time();
        health_ = GrokHealth::kOk;
    }
    next_poll_us_ =
        esp_timer_get_time() + static_cast<int64_t>(CONFIG_GROK_MONITOR_POLL_SECONDS) * 1000000;
    if (!new_reply_from.empty() && on_reply_) {
        on_reply_(new_reply_from);
    }
}

void GrokMonitor::CommitFailure(GrokHealth health) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (credentials_.token.empty()) {
            return;
        }
        health_ = health;
    }
    next_poll_us_ = esp_timer_get_time() + kRetryUs;
}
