#include "solis_page.h"

#include "display/display.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace {

// Clears the top bar the chat layouts put on the screen.
constexpr int kTopBarHeight = 36;

constexpr uint32_t kBackground = 0x101418;
constexpr uint32_t kAlarmBackground = 0x7A1414;
constexpr uint32_t kText = 0xE8ECEF;
constexpr uint32_t kMuted = 0x8A949C;
constexpr uint32_t kSolar = 0xFFC83D;
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

void FormatPower(char* out, size_t size, int32_t watts) {
    const int32_t magnitude = labs(watts);
    if (magnitude < 10000) {
        snprintf(out, size, "%ld W", static_cast<long>(magnitude));
        return;
    }
    snprintf(out, size, "%ld,%ld kW", static_cast<long>(magnitude / 1000),
             static_cast<long>((magnitude % 1000) / 100));
}

void FormatEnergy(char* out, size_t size, int32_t watt_hours) {
    const int32_t tenths = watt_hours / 100;
    snprintf(out, size, "%ld,%ld kWh", static_cast<long>(tenths / 10),
             static_cast<long>(tenths % 10));
}

struct StateText {
    const char* text;
    uint32_t color;
};

StateText DescribeState(const SolisStatus& status) {
    switch (status.health) {
        case SolisHealth::kNoKey:
            return {"Koppla till SolisCloud", kSolar};
        case SolisHealth::kWaiting:
            return {"H\xC3\xA4mtar data", kMuted};
        case SolisHealth::kRejected:
            return {"Fel API-nyckel", kBad};
        case SolisHealth::kNoPlant:
            return {"Ingen anl\xC3\xA4ggning", kBad};
        case SolisHealth::kUnreachable:
            return {"Ingen kontakt", kWarn};
        case SolisHealth::kOk:
            break;
    }
    switch (status.reading.state) {
        case soliscloud::PlantState::kOnline:
            return {"Online", kGood};
        case soliscloud::PlantState::kOffline:
            // Normal at night for a plant without a battery, so amber and not red.
            return {"Anl\xC3\xA4ggningen offline", kWarn};
        case soliscloud::PlantState::kAlarm:
            return {"LARM i SolisCloud", kBad};
        case soliscloud::PlantState::kUnknown:
            break;
    }
    return {"Status ok\xC3\xA4nd", kText};
}

}  // namespace

lv_obj_t* SolisPage::AddRow(lv_obj_t** label, lv_obj_t** value) {
    lv_obj_t* row = lv_obj_create(root_);
    lv_obj_remove_style_all(row);
    lv_obj_set_size(row, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);

    *label = lv_label_create(row);
    lv_obj_set_style_text_color(*label, Color(kMuted), 0);
    lv_label_set_text(*label, "");

    *value = lv_label_create(row);
    lv_obj_set_style_text_color(*value, Color(kText), 0);
    lv_label_set_text(*value, "-");
    return row;
}

void SolisPage::Create() {
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

    state_ = lv_label_create(root_);
    lv_obj_set_width(state_, LV_PCT(100));
    lv_label_set_long_mode(state_, LV_LABEL_LONG_CLIP);
    lv_label_set_text(state_, "");

    setup_ = lv_label_create(root_);
    lv_obj_set_width(setup_, LV_PCT(100));
    lv_label_set_long_mode(setup_, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_color(setup_, Color(kText), 0);
    lv_label_set_text(setup_, "");

    lv_obj_t* label = nullptr;
    solar_row_ = AddRow(&label, &solar_);
    lv_label_set_text(label, "Sol");
    lv_obj_set_style_text_color(solar_, Color(kSolar), 0);

    load_row_ = AddRow(&label, &load_);
    lv_label_set_text(label, "Huset");

    battery_row_ = AddRow(&battery_label_, &battery_);
    grid_row_ = AddRow(&grid_label_, &grid_);

    today_heading_ = lv_label_create(root_);
    lv_obj_set_style_text_color(today_heading_, Color(kMuted), 0);
    lv_obj_set_style_pad_top(today_heading_, 6, 0);
    lv_label_set_text(today_heading_, "Idag");

    solar_today_row_ = AddRow(&label, &solar_today_);
    lv_label_set_text(label, "Sol");

    bought_row_ = AddRow(&label, &bought_);
    lv_label_set_text(label, "K\xC3\xB6pt");

    sold_row_ = AddRow(&label, &sold_);
    lv_label_set_text(label, "S\xC3\xA5ld");

    footer_ = lv_label_create(root_);
    lv_obj_set_style_text_color(footer_, Color(kMuted), 0);
    lv_obj_set_style_pad_top(footer_, 6, 0);
    lv_label_set_text(footer_, "");
}

void SolisPage::Render(const SolisStatus& status, const std::string& setup_url) {
    char text[32];
    const auto& r = status.reading;

    const StateText state = DescribeState(status);
    SetText(state_, state.text);
    lv_obj_set_style_text_color(state_, Color(state.color), 0);
    const bool alarm =
        status.health == SolisHealth::kOk && r.state == soliscloud::PlantState::kAlarm;
    lv_obj_set_style_bg_color(root_, Color(alarm ? kAlarmBackground : kBackground), 0);

    const bool needs_key = status.health == SolisHealth::kNoKey;
    SetVisible(setup_, needs_key);
    if (needs_key) {
        const std::string hint = setup_url.empty()
                                     ? std::string("V\xC3\xA4ntar p\xC3\xA5 WiFi")
                                     : "Skriv in nyckeln i en webbl\xC3\xA4sare p\xC3\xA5 "
                                       "samma n\xC3\xA4tverk:\n" + setup_url;
        SetText(setup_, hint.c_str());
    }

    const bool has_data = status.has_reading;
    SetVisible(solar_row_, has_data);
    SetVisible(today_heading_, has_data);
    SetVisible(solar_today_row_, has_data);
    SetVisible(load_row_, has_data && r.has_load);
    SetVisible(battery_row_, has_data && r.has_battery);
    SetVisible(grid_row_, has_data && r.has_grid);
    SetVisible(bought_row_, has_data && r.has_grid);
    SetVisible(sold_row_, has_data && r.has_grid);
    SetVisible(footer_, has_data);
    if (!has_data) {
        return;
    }

    FormatPower(text, sizeof(text), r.pv_watts);
    SetText(solar_, text);
    FormatPower(text, sizeof(text), r.load_watts);
    SetText(load_, text);

    if (r.has_battery) {
        const char* direction = r.battery_watts > 0   ? "Laddar"
                                : r.battery_watts < 0 ? "Urladdar"
                                                      : "Batteri";
        snprintf(text, sizeof(text), "%s %d %%", direction, r.soc_percent);
        SetText(battery_label_, text);
        FormatPower(text, sizeof(text), r.battery_watts);
        SetText(battery_, text);
    }

    if (r.has_grid) {
        SetText(grid_label_, r.grid_watts < 0 ? "K\xC3\xB6per" : "S\xC3\xA4ljer");
        FormatPower(text, sizeof(text), r.grid_watts);
        SetText(grid_, text);
        FormatEnergy(text, sizeof(text), r.bought_today_wh);
        SetText(bought_, text);
        FormatEnergy(text, sizeof(text), r.sold_today_wh);
        SetText(sold_, text);
    }

    FormatEnergy(text, sizeof(text), r.pv_today_wh);
    SetText(solar_today_, text);

    if (status.data_age_seconds >= 0) {
        snprintf(text, sizeof(text), "Data: %d min gammal", status.data_age_seconds / 60);
        SetText(footer_, text);
    } else {
        SetText(footer_, "");
    }
}

void SolisPage::Apply(bool visible, const SolisStatus& status, const std::string& setup_url) {
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
