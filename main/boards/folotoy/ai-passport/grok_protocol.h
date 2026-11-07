#ifndef GROK_PROTOCOL_H
#define GROK_PROTOCOL_H

// The relay's poll answer and the small text helpers around it.
//
// The relay answers in tab-separated lines rather than JSON, so one small buffer
// and no parser state are enough on a board without PSRAM:
//
//   GROK1
//   U <tab> unread count
//   B <tab> id <tab> name <tab> state <tab> seconds since report, -1 for never <tab> note
//   M <tab> id <tab> bot id <tab> age in seconds <tab> text
//
// No ESP-IDF dependency, and constexpr so test/ can prove it at compile time.

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace grok {

inline constexpr std::string_view kProtocolLine = "GROK1";
inline constexpr size_t kMaxResponseBytes = 4096;
inline constexpr size_t kMaxBots = 6;
inline constexpr size_t kMaxMessages = 3;
inline constexpr size_t kMaxOutgoingBytes = 800;

enum class BotState { kUnknown, kIdle, kWorking, kWaiting, kError, kQuiet, kNoReply };

struct Bot {
    std::string id;
    std::string name;
    std::string note;
    BotState state = BotState::kUnknown;
    int report_age_seconds = -1;
};

struct Message {
    uint32_t id = 0;
    std::string bot_id;
    std::string text;
    int age_seconds = 0;
};

struct Poll {
    std::vector<Bot> bots;
    std::vector<Message> messages;
    int unread = 0;
};

constexpr BotState ParseBotState(std::string_view text) {
    if (text == "idle") return BotState::kIdle;
    if (text == "working") return BotState::kWorking;
    if (text == "waiting") return BotState::kWaiting;
    if (text == "error") return BotState::kError;
    if (text == "quiet") return BotState::kQuiet;
    if (text == "noreply") return BotState::kNoReply;
    return BotState::kUnknown;
}

constexpr std::string_view BotStateName(BotState state) {
    switch (state) {
        case BotState::kIdle:
            return "idle";
        case BotState::kWorking:
            return "working";
        case BotState::kWaiting:
            return "waiting_for_approval";
        case BotState::kError:
            return "error";
        case BotState::kQuiet:
            return "stopped_reporting";
        case BotState::kNoReply:
            return "not_answering";
        case BotState::kUnknown:
            break;
    }
    return "unknown";
}

constexpr bool ParseInt(std::string_view text, int& out) {
    const bool negative = !text.empty() && text[0] == '-';
    if (negative) {
        text.remove_prefix(1);
    }
    if (text.empty() || text.size() > 9) {
        return false;
    }
    int value = 0;
    for (const char c : text) {
        if (c < '0' || c > '9') {
            return false;
        }
        value = value * 10 + (c - '0');
    }
    out = negative ? -value : value;
    return true;
}

// Takes the next field off `rest`. The last field of a line runs to its end.
constexpr std::string_view NextField(std::string_view& rest) {
    const size_t tab = rest.find('\t');
    const std::string_view field = rest.substr(0, tab);
    rest = tab == std::string_view::npos ? std::string_view() : rest.substr(tab + 1);
    return field;
}

// Refuses anything that is not a complete answer, so a captive portal's page or a
// cut connection cannot pass for an empty list of Bots. Unknown line kinds are
// skipped, which lets the relay add one without breaking older firmware.
constexpr bool ParsePoll(std::string_view body, Poll& out) {
    if (body.empty() || body.back() != '\n') {
        return false;
    }
    size_t end = body.find('\n');
    if (body.substr(0, end) != kProtocolLine) {
        return false;
    }
    Poll poll;
    size_t start = end + 1;
    while (start < body.size()) {
        end = body.find('\n', start);
        std::string_view rest = body.substr(start, end - start);
        start = end + 1;

        const std::string_view kind = NextField(rest);
        if (kind == "U") {
            if (!ParseInt(NextField(rest), poll.unread) || poll.unread < 0) {
                return false;
            }
        } else if (kind == "B") {
            if (poll.bots.size() == kMaxBots) {
                continue;
            }
            Bot bot;
            bot.id = std::string(NextField(rest));
            bot.name = std::string(NextField(rest));
            bot.state = ParseBotState(NextField(rest));
            if (bot.id.empty() || !ParseInt(NextField(rest), bot.report_age_seconds)) {
                return false;
            }
            bot.note = std::string(rest);
            poll.bots.push_back(std::move(bot));
        } else if (kind == "M") {
            if (poll.messages.size() == kMaxMessages) {
                continue;
            }
            Message message;
            int id = 0;
            if (!ParseInt(NextField(rest), id) || id <= 0) {
                return false;
            }
            message.id = static_cast<uint32_t>(id);
            message.bot_id = std::string(NextField(rest));
            if (!ParseInt(NextField(rest), message.age_seconds)) {
                return false;
            }
            message.text = std::string(rest);
            poll.messages.push_back(std::move(message));
        }
    }
    out = std::move(poll);
    return true;
}

// Lower-cases ASCII and the three Swedish letters, which is what a spoken name
// needs. Å, Ä and Ö are C3 85, C3 84 and C3 96 in UTF-8.
constexpr std::string Fold(std::string_view text) {
    std::string out(text);
    for (size_t i = 0; i < out.size(); i++) {
        const unsigned char c = out[i];
        if (c >= 'A' && c <= 'Z') {
            out[i] = static_cast<char>(c + 32);
        } else if (c == 0xC3 && i + 1 < out.size()) {
            const unsigned char next = out[i + 1];
            if (next == 0x84 || next == 0x85 || next == 0x96) {
                out[i + 1] = static_cast<char>(next + 0x20);
            }
            i++;
        }
    }
    return out;
}

// Finds the Bot a spoken name means. A word that fits two Bots finds neither: a
// message that lands on the wrong Bot would be worse than a question back.
constexpr const Bot* FindBot(const std::vector<Bot>& bots, std::string_view query) {
    const std::string wanted = Fold(query);
    if (wanted.empty()) {
        return nullptr;
    }
    for (const Bot& bot : bots) {
        if (Fold(bot.id) == wanted || Fold(bot.name) == wanted) {
            return &bot;
        }
    }
    const Bot* found = nullptr;
    for (const Bot& bot : bots) {
        const std::string name = Fold(bot.name);
        if (name.find(wanted) != std::string::npos || wanted.find(name) != std::string::npos) {
            if (found != nullptr) {
                return nullptr;
            }
            found = &bot;
        }
    }
    return found;
}

constexpr std::string UrlEncode(std::string_view text) {
    constexpr std::string_view kDigits = "0123456789ABCDEF";
    std::string out;
    out.reserve(text.size() + 16);
    for (const char signed_c : text) {
        const unsigned char c = signed_c;
        const bool plain = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                           (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.' || c == '~';
        if (plain) {
            out.push_back(signed_c);
        } else {
            out.push_back('%');
            out.push_back(kDigits[c >> 4]);
            out.push_back(kDigits[c & 15]);
        }
    }
    return out;
}

// The device token travels to this address, so plain http is refused. Returns
// empty for an address that cannot be used.
constexpr std::string NormalizeRelayUrl(std::string_view url) {
    constexpr std::string_view kScheme = "https://";
    while (!url.empty() && (url.back() == '/' || url.back() == ' ')) {
        url.remove_suffix(1);
    }
    while (!url.empty() && url.front() == ' ') {
        url.remove_prefix(1);
    }
    if (url.size() <= kScheme.size() || url.substr(0, kScheme.size()) != kScheme) {
        return {};
    }
    for (const char c : url) {
        if (static_cast<unsigned char>(c) <= ' ') {
            return {};
        }
    }
    return std::string(url);
}

}  // namespace grok

#endif  // GROK_PROTOCOL_H
