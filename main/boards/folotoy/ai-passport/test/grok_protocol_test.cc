// Compile-time tests. The compiler evaluates every check, so this runs with only
// the cross compiler and no hardware:
//   riscv32-esp-elf-g++ -std=gnu++23 -fconstexpr-ops-limit=200000000 \
//       -fconstexpr-loop-limit=1000000 -fsyntax-only -I .. grok_protocol_test.cc
#include "grok_protocol.h"

using namespace grok;

constexpr std::string_view kAnswer =
    "GROK1\n"
    "U\t4\n"
    "B\tsalj\tSäljboten\tworking\t120\tSkriver offert\n"
    "B\tdrift\tDrift\tnoreply\t-1\t\n"
    "M\t42\tsalj\t30\tOfferten är klar.\n"
    "M\t43\tdrift\t5\tAllt ser bra ut.\n";

constexpr bool ParsesTheAnswer() {
    Poll poll;
    if (!ParsePoll(kAnswer, poll)) return false;
    if (poll.unread != 4 || poll.bots.size() != 2 || poll.messages.size() != 2) return false;
    const Bot& first = poll.bots[0];
    if (first.id != "salj" || first.name != "Säljboten" || first.note != "Skriver offert" ||
        first.state != BotState::kWorking || first.report_age_seconds != 120) {
        return false;
    }
    const Bot& second = poll.bots[1];
    if (second.state != BotState::kNoReply || second.report_age_seconds != -1 ||
        !second.note.empty()) {
        return false;
    }
    const Message& message = poll.messages[1];
    return message.id == 43 && message.bot_id == "drift" && message.age_seconds == 5 &&
           message.text == "Allt ser bra ut.";
}
static_assert(ParsesTheAnswer());

constexpr bool Refuses(std::string_view body) {
    Poll poll;
    poll.unread = 7;
    return !ParsePoll(body, poll) && poll.unread == 7;
}
static_assert(Refuses(""));
static_assert(Refuses("<html>captive portal</html>\n"));
static_assert(Refuses("GROK2\nU\t0\n"));
static_assert(Refuses("GROK1\nU\t0"), "a cut connection is not an empty list");
static_assert(Refuses("GROK1\nU\t-1\n"));
static_assert(Refuses("GROK1\nB\t\tnamn\tidle\t1\t\n"));
static_assert(Refuses("GROK1\nB\ta\tnamn\tidle\tsoon\t\n"));
static_assert(Refuses("GROK1\nM\t0\ta\t1\ttext\n"));
static_assert(Refuses("GROK1\nM\t12345678901\ta\t1\ttext\n"));

constexpr bool AcceptsWhatItShouldSkip() {
    Poll poll;
    if (!ParsePoll("GROK1\nZ\tfuture line\nU\t0\n", poll)) return false;
    if (!ParsePoll("GROK1\nB\ta\tA\tbrand-new\t1\t\n", poll)) return false;
    return poll.bots[0].state == BotState::kUnknown;
}
static_assert(AcceptsWhatItShouldSkip());

constexpr bool CapsTheLists() {
    std::string body = "GROK1\n";
    for (int i = 1; i <= 9; i++) {
        const std::string id(1, static_cast<char>('0' + i));
        body += "B\t" + id + "\tn\tidle\t1\t\n";
        body += "M\t" + id + "\ta\t1\tt\n";
    }
    Poll poll;
    return ParsePoll(body, poll) && poll.bots.size() == kMaxBots &&
           poll.messages.size() == kMaxMessages;
}
static_assert(CapsTheLists());

static_assert(ParseBotState("waiting") == BotState::kWaiting);
static_assert(ParseBotState("quiet") == BotState::kQuiet);
static_assert(BotStateName(BotState::kWaiting) == "waiting_for_approval");

static_assert(Fold("ÅÄÖ Sälj") == "åäö sälj");
static_assert(Fold("åäö") == "åäö");

constexpr bool Finds(std::string_view query, std::string_view id) {
    std::vector<Bot> bots(3);
    bots[0].id = "salj";
    bots[0].name = "Säljboten";
    bots[1].id = "drift";
    bots[1].name = "Driftboten";
    bots[2].id = "offert";
    bots[2].name = "Offertbot Öst";
    const Bot* bot = FindBot(bots, query);
    return id.empty() ? bot == nullptr : bot != nullptr && bot->id == id;
}
static_assert(Finds("salj", "salj"));
static_assert(Finds("SÄLJBOTEN", "salj"));
static_assert(Finds("sälj", "salj"));
static_assert(Finds("offertbot öst", "offert"));
static_assert(Finds("OFFERTBOT ÖST", "offert"));
static_assert(Finds("boten", ""), "two Bots fit, so neither is chosen");
static_assert(Finds("ekonomi", ""));
static_assert(Finds("", ""));

static_assert(UrlEncode("hej då & tack=1") == "hej%20d%C3%A5%20%26%20tack%3D1");
static_assert(UrlEncode("a-b_c.d~e") == "a-b_c.d~e");

static_assert(NormalizeRelayUrl(" https://relay.example.workers.dev/ ") ==
              "https://relay.example.workers.dev");
static_assert(NormalizeRelayUrl("http://relay.example").empty());
static_assert(NormalizeRelayUrl("https://").empty());
static_assert(NormalizeRelayUrl("https://a b").empty());
static_assert(NormalizeRelayUrl("").empty());
