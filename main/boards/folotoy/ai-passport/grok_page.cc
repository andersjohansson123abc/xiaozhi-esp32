#include "grok_page.h"

#include "display/display.h"

#include <cstdio>
#include <cstring>

namespace {

// Clears the top bar the chat layouts put on the screen.
constexpr int kTopBarHeight = 36;

constexpr uint32_t kBackground = 0x101418;
constexpr uint32_t kText = 0xE8ECEF;
constexpr uint32_t kMuted = 0x8A949C;
constexpr uint32_t kBusy = 0x6FB7FF;
constexpr uint32_t kGood = 0x5BD17F;
constexpr uint32_t kWarn = 0xFF9F45;
constexpr uint32_t kBad = 0xFF5C5C;

lv_color_t Color(uint32_t rgb) {
    return lv_color_hex(rgb);
}

// Avoids invalidating, and so redrawing, a label whose text did not change.
void SetText(lv_obj_t* label, const char* text) {
    if (strcmp(lv_label_get_text(label), text) != 0) {
        lv_label_set_text(label, text);
    }
}

void SetVisible(lv_obj_t* object, bool visible) {
    if (visible) {
        lv_obj_remove_flag(object, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(object, LV_OBJ_FLAG_HIDDEN);
    }
}

struct StateText {
    const char* text;
    uint32_t color;
};

StateText DescribeBot(grok::BotState state) {
    switch (state) {
        case grok::BotState::kIdle:
            return {"Ledig", kGood};
        case grok::BotState::kWorking:
            return {"Arbetar", kBusy};
        case grok::BotState::kWaiting:
            return {"Väntar på dig", kWarn};
        case grok::BotState::kError:
            return {"Fel", kBad};
        case grok::BotState::kQuiet:
            return {"Tyst", kWarn};
        case grok::BotState::kNoReply:
            return {"Svarar inte", kBad};
        case grok::BotState::kUnknown:
            break;
    }
    return {"Okänd", kMuted};
}

StateText DescribeHealth(GrokHealth health) {
    switch (health) {
        case GrokHealth::kNoToken:
            return {"Koppla till reläet", kBusy};
        case GrokHealth::kWaiting:
            return {"Hämtar botar", kMuted};
        case GrokHealth::kRejected:
            return {"Fel token", kBad};
        case GrokHealth::kUnreachable:
            return {"Ingen kontakt", kWarn};
        case GrokHealth::kOk:
            break;
    }
    return {"Grok-botar", kText};
}

const std::string& BotName(const grok::Poll& poll, const std::string& id) {
    for (const grok::Bot& bot : poll.bots) {
        if (bot.id == id) {
            return bot.name;
        }
    }
    return id;
}

// The reply waiting to be read, or else what the first busy Bot says it is doing.
std::string DetailText(const grok::Poll& poll) {
    if (!poll.messages.empty()) {
        const grok::Message& message = poll.messages.front();
        return BotName(poll, message.bot_id) + ":\n" + message.text;
    }
    for (const grok::Bot& bot : poll.bots) {
        if (!bot.note.empty() && bot.state != grok::BotState::kIdle) {
            return bot.name + ": " + bot.note;
        }
    }
    return {};
}

}  // namespace

void GrokPage::Create() {
    auto* screen = lv_screen_active();
    root_ = lv_obj_create(screen);
    lv_obj_remove_style_all(root_);
    lv_obj_set_size(root_, lv_obj_get_width(screen), lv_obj_get_height(screen) - kTopBarHeight);
    lv_obj_set_pos(root_, 0, kTopBarHeight);
    lv_obj_set_style_bg_color(root_, Color(kBackground), 0);
    lv_obj_set_style_bg_opa(root_, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_hor(root_, 12, 0);
    lv_obj_set_style_pad_ver(root_, 8, 0);
    lv_obj_set_style_pad_row(root_, 4, 0);
    lv_obj_set_flex_flow(root_, LV_FLEX_FLOW_COLUMN);
    lv_obj_remove_flag(root_, LV_OBJ_FLAG_SCROLLABLE);

    heading_ = lv_label_create(root_);
    lv_obj_set_width(heading_, LV_PCT(100));
    lv_label_set_long_mode(heading_, LV_LABEL_LONG_CLIP);
    lv_label_set_text(heading_, "");

    setup_ = lv_label_create(root_);
    lv_obj_set_width(setup_, LV_PCT(100));
    lv_label_set_long_mode(setup_, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_color(setup_, Color(kText), 0);
    lv_label_set_text(setup_, "");

    for (Row& row : rows_) {
        row.root = lv_obj_create(root_);
        lv_obj_remove_style_all(row.root);
        lv_obj_set_size(row.root, LV_PCT(100), LV_SIZE_CONTENT);
        lv_obj_set_flex_flow(row.root, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(row.root, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER,
                              LV_FLEX_ALIGN_CENTER);
        lv_obj_set_style_pad_column(row.root, 8, 0);

        row.name = lv_label_create(row.root);
        lv_obj_set_flex_grow(row.name, 1);
        lv_label_set_long_mode(row.name, LV_LABEL_LONG_DOT);
        lv_obj_set_style_text_color(row.name, Color(kText), 0);
        lv_label_set_text(row.name, "");

        row.state = lv_label_create(row.root);
        lv_label_set_text(row.state, "");
    }

    detail_ = lv_label_create(root_);
    lv_obj_set_width(detail_, LV_PCT(100));
    lv_obj_set_flex_grow(detail_, 1);
    lv_label_set_long_mode(detail_, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_color(detail_, Color(kText), 0);
    lv_obj_set_style_pad_top(detail_, 6, 0);
    lv_label_set_text(detail_, "");

    footer_ = lv_label_create(root_);
    lv_obj_set_width(footer_, LV_PCT(100));
    lv_label_set_long_mode(footer_, LV_LABEL_LONG_CLIP);
    lv_obj_set_style_text_color(footer_, Color(kMuted), 0);
    lv_label_set_text(footer_, "");
}

void GrokPage::Render(const GrokStatus& status, const std::string& setup_url) {
    const grok::Poll& poll = status.poll;

    StateText heading = DescribeHealth(status.health);
    char text[40];
    if (status.health == GrokHealth::kOk && poll.unread > 0) {
        snprintf(text, sizeof(text), "%d %s", poll.unread,
                 poll.unread == 1 ? "nytt svar" : "nya svar");
        heading = {text, kBusy};
    }
    SetText(heading_, heading.text);
    lv_obj_set_style_text_color(heading_, Color(heading.color), 0);

    const bool needs_token = status.health == GrokHealth::kNoToken;
    SetVisible(setup_, needs_token);
    if (needs_token) {
        const std::string hint = setup_url.empty()
                                     ? std::string("Väntar på WiFi")
                                     : "Skriv in reläet i en webbläsare på samma nätverk:\n" +
                                           setup_url;
        SetText(setup_, hint.c_str());
    }

    for (size_t i = 0; i < rows_.size(); i++) {
        const bool used = status.has_poll && i < poll.bots.size();
        SetVisible(rows_[i].root, used);
        if (!used) {
            continue;
        }
        const StateText state = DescribeBot(poll.bots[i].state);
        SetText(rows_[i].name, poll.bots[i].name.c_str());
        SetText(rows_[i].state, state.text);
        lv_obj_set_style_text_color(rows_[i].state, Color(state.color), 0);
    }

    SetText(detail_, DetailText(poll).c_str());

    if (status.pending_sends > 0) {
        SetText(footer_, "Skickar...");
    } else if (!poll.messages.empty()) {
        SetText(footer_, "Tryck OK och säg läs upp");
    } else {
        SetText(footer_, "");
    }
}

void GrokPage::Apply(bool visible, const GrokStatus& status, const std::string& setup_url) {
    if (root_ == nullptr && !visible) {
        return;
    }
    DisplayLockGuard lock(display_);
    if (!lock) {
        return;
    }
    if (root_ == nullptr) {
        Create();
    }
    SetVisible(root_, visible);
    if (visible) {
        lv_obj_move_foreground(root_);
        Render(status, setup_url);
    }
}
