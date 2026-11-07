#ifndef SOLIS_MONITOR_H
#define SOLIS_MONITOR_H

#include "soliscloud_protocol.h"

#include <atomic>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>

enum class SolisHealth {
    kNoKey,        // No API key yet: the local setup page is waiting.
    kWaiting,      // Keyed, nothing fetched yet.
    kOk,
    kRejected,     // SolisCloud refused the key.
    kNoPlant,      // The key sees no plant.
    kUnreachable,  // Network or server failure on the latest attempt.
};

// What the page and the MCP tool see.
struct SolisStatus {
    soliscloud::Reading reading;
    bool has_reading = false;  // At least one fetch has succeeded since boot.
    SolisHealth health = SolisHealth::kNoKey;
    int data_age_seconds = -1;  // Since SolisCloud last heard from the plant.
};

// Fetches one plant from SolisCloud with the owner's own API key.
//
// The key can also control the inverter, so it only ever enters through the local
// setup page, is stored on this board alone, and never leaves except as a request
// signature. Nothing here may pass it to the MCP tools or the logs.
//
// TLS needs far more stack and heap than the monitor does when idle, and this
// board has no PSRAM. So the small task below only decides, once per second, and
// a fetch runs in a task that exists for that one fetch, and only while the device
// is idle and the heap can take it.
class SolisMonitor {
public:
    // Both run on the monitor's task: `on_tick` once per second, `on_alarm` when
    // the plant goes into its alarm state.
    using TickCallback = std::function<void(const SolisStatus&)>;
    using AlarmCallback = std::function<void()>;

    SolisMonitor();

    void Start(TickCallback on_tick, AlarmCallback on_alarm);

    bool IsConfigured() const;
    // `plant_id` may be empty, which selects the account's first plant.
    bool SetCredentials(const std::string& key_id, const std::string& secret,
                        const std::string& plant_id);
    void ForgetCredentials();

    SolisStatus GetStatus() const;

private:
    struct Credentials {
        std::string key_id;
        std::string secret;
        std::string plant_id;
    };

    static void TaskEntry(void* arg);
    static void FetchEntry(void* arg);
    void Run();
    void Fetch();
    bool CanFetchNow() const;
    bool ClockIsSet() const;
    void Commit(SolisHealth health, const soliscloud::Reading* reading);

    mutable std::mutex mutex_;
    Credentials credentials_;
    std::string resolved_plant_id_;
    soliscloud::Reading reading_;
    bool has_reading_ = false;
    SolisHealth health_ = SolisHealth::kNoKey;

    std::atomic<bool> fetching_{false};
    std::atomic<bool> fetch_now_{false};
    std::atomic<int64_t> next_fetch_us_{0};

    TickCallback on_tick_;
    AlarmCallback on_alarm_;
};

#endif  // SOLIS_MONITOR_H
