#ifndef SOLIS_PAGE_H
#define SOLIS_PAGE_H

#include "solis_monitor.h"

#include <lvgl.h>

#include <string>

class Display;

// Full-width LVGL page drawn over the chat view, below the status bar so that
// the Wi-Fi and battery icons and the volume notification stay visible.
class SolisPage {
public:
    explicit SolisPage(Display* display) : display_(display) {}

    // Safe from any task: takes the display lock itself. Creates the widgets the
    // first time the page is shown, so an unused page costs no heap. `setup_url`
    // is where the key can be entered, empty while the local page is not up.
    void Apply(bool visible, const SolisStatus& status, const std::string& setup_url);

private:
    void Create();
    void Render(const SolisStatus& status, const std::string& setup_url);
    lv_obj_t* AddRow(lv_obj_t** label, lv_obj_t** value);

    Display* display_;
    lv_obj_t* root_ = nullptr;
    lv_obj_t* state_ = nullptr;
    lv_obj_t* setup_ = nullptr;
    lv_obj_t* solar_row_ = nullptr;
    lv_obj_t* solar_ = nullptr;
    lv_obj_t* load_row_ = nullptr;
    lv_obj_t* load_ = nullptr;
    lv_obj_t* battery_row_ = nullptr;
    lv_obj_t* battery_label_ = nullptr;
    lv_obj_t* battery_ = nullptr;
    lv_obj_t* grid_row_ = nullptr;
    lv_obj_t* grid_label_ = nullptr;
    lv_obj_t* grid_ = nullptr;
    lv_obj_t* today_heading_ = nullptr;
    lv_obj_t* solar_today_row_ = nullptr;
    lv_obj_t* solar_today_ = nullptr;
    lv_obj_t* bought_row_ = nullptr;
    lv_obj_t* bought_ = nullptr;
    lv_obj_t* sold_row_ = nullptr;
    lv_obj_t* sold_ = nullptr;
    lv_obj_t* footer_ = nullptr;
};

#endif  // SOLIS_PAGE_H
