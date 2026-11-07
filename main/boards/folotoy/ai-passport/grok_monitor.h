#ifndef GROK_MONITOR_H
#define GROK_MONITOR_H

#include "grok_protocol.h"

#include <atomic>
#include <cstdint>
#include <deque>
#include <functional>
#include <mutex>
#include <string>
#include <vector>

enum class GrokHealth {
    kNoToken,      // No relay yet: the local setup page is waiting.
    kWaiting,      // Connected to nothing yet.
    kOk,
    kRejected,     // The relay refused the device token.
    kUnreachable,  // Network or relay failure on the latest attempt.
};

enum class GrokSendResult {
    kSent,     // The Bot's routine started. It says nothing about an answer.
    kRefused,  // The relay could not start the routine.
    kLimited,  // Over the relay's hourly limit.
    kFailed,   // Gave up after repeated network failures.
};

// What the page and the MCP tools see.
struct GrokStatus {
    grok::Poll poll;
    bool has_poll = false;
    GrokHealth health = GrokHealth::kNoToken;
    int poll_age_seconds = -1;
    int pending_sends = 0;
};

// Follows the owner's Grok Bots through the relay in scripts/grok-relay.
//
// Grok Bot has no public API, so the state of a Bot is what the Bot told the relay,
// and a message is a routine started through a webhook. An answer can take minutes
// and arrives on a later poll.
//
// The device token only opens the relay and only enters through the local setup
// page. As with SolisMonitor, TLS needs far more stack and heap than this class
// does when idle, so the small task below only decides, once per second, and the
// network work runs in a task that exists for that one round.
class GrokMonitor {
public:
    // All three run on one of the monitor's tasks.
    using TickCallback = std::function<void()>;
    using ReplyCallback = std::function<void(const std::string& bot_name)>;
    using SendCallback = std::function<void(const std::string& bot_name, GrokSendResult result)>;

    GrokMonitor();

    void Start(TickCallback on_tick, ReplyCallback on_reply, SendCallback on_send);

    bool IsConfigured() const;
    bool SetCredentials(const std::string& relay_url, const std::string& token);
    void ForgetCredentials();

    GrokStatus GetStatus() const;
    bool QueueMessage(const std::string& bot_id, const std::string& bot_name,
                      const std::string& text);
    // Hands over the replies that have arrived and marks them read on the next poll.
    std::vector<grok::Message> TakeReplies();

private:
    struct Credentials {
        std::string relay_url;
        std::string token;
    };
    struct Outgoing {
        std::string bot_id;
        std::string bot_name;
        std::string text;
        int attempts = 0;
    };

    static void TaskEntry(void* arg);
    static void WorkEntry(void* arg);
    void Run();
    void Work();
    bool HeapAllowsTls() const;
    bool SendNext(const Credentials& credentials);
    void PollRelay(const Credentials& credentials);
    void CommitPoll(grok::Poll&& poll);
    void CommitFailure(GrokHealth health);

    mutable std::mutex mutex_;
    Credentials credentials_;
    grok::Poll poll_;
    bool has_poll_ = false;
    int64_t poll_time_us_ = 0;
    GrokHealth health_ = GrokHealth::kNoToken;
    std::deque<Outgoing> outbox_;
    uint32_t acked_id_ = 0;
    uint32_t announced_id_ = 0;

    std::atomic<bool> working_{false};
    std::atomic<bool> poll_now_{false};
    std::atomic<int64_t> next_poll_us_{0};
    std::atomic<int64_t> next_send_us_{0};

    TickCallback on_tick_;
    ReplyCallback on_reply_;
    SendCallback on_send_;
};

#endif  // GROK_MONITOR_H
