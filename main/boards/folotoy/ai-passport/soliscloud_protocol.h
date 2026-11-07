#ifndef SOLISCLOUD_PROTOCOL_H
#define SOLISCLOUD_PROTOCOL_H

// Streaming parsers for SolisCloud's plant endpoints.
//
// A stationDetail answer is many kilobytes of JSON and this board has no PSRAM,
// so nothing here builds a document: the scanner walks the bytes as they arrive
// and hands over one key/value pair at a time, in a fixed amount of memory. No
// ESP-IDF dependency, and constexpr so test/ can prove it at compile time.

#include <cstddef>
#include <cstdint>
#include <string_view>

namespace soliscloud {

inline constexpr std::string_view kStationListResource = "/v1/api/userStationList";
inline constexpr std::string_view kStationDetailResource = "/v1/api/stationDetail";
inline constexpr std::string_view kStationListBody = "{\"pageNo\":1,\"pageSize\":1}";

// Every scalar reaches `on_field` as text. A value that opens an object or array
// reaches it with an empty value, which is how "records" can be located.
class JsonScanner {
public:
    template <typename OnField>
    constexpr void Feed(const char* data, size_t size, OnField&& on_field) {
        for (size_t i = 0; i < size; i++) {
            Step(data[i], on_field);
        }
    }

private:
    enum class State { kIdle, kKey, kAfterKey, kBeforeValue, kStringValue, kLiteral };

    static constexpr bool IsSpace(char c) {
        return c == ' ' || c == '\n' || c == '\r' || c == '\t';
    }

    template <typename OnField>
    constexpr void Step(char c, OnField& on_field) {
        switch (state_) {
            case State::kIdle:
                if (c == '"') {
                    key_size_ = 0;
                    state_ = State::kKey;
                }
                break;
            case State::kKey:
                if (escaped_) {
                    escaped_ = false;
                } else if (c == '\\') {
                    escaped_ = true;
                } else if (c == '"') {
                    state_ = State::kAfterKey;
                } else if (key_size_ < sizeof(key_)) {
                    key_[key_size_++] = c;
                }
                break;
            case State::kAfterKey:
                if (c == ':') {
                    state_ = State::kBeforeValue;
                } else if (!IsSpace(c)) {
                    // That was a string inside an array, not a key.
                    state_ = State::kIdle;
                }
                break;
            case State::kBeforeValue:
                value_size_ = 0;
                if (c == '"') {
                    state_ = State::kStringValue;
                } else if (c == '{' || c == '[') {
                    Emit(on_field);
                } else if (!IsSpace(c)) {
                    value_[value_size_++] = c;
                    state_ = State::kLiteral;
                }
                break;
            case State::kStringValue:
                if (escaped_) {
                    escaped_ = false;
                } else if (c == '\\') {
                    escaped_ = true;
                } else if (c == '"') {
                    Emit(on_field);
                } else if (value_size_ < sizeof(value_)) {
                    value_[value_size_++] = c;
                }
                break;
            case State::kLiteral:
                if (c == ',' || c == '}' || c == ']' || IsSpace(c)) {
                    Emit(on_field);
                } else if (value_size_ < sizeof(value_)) {
                    value_[value_size_++] = c;
                }
                break;
        }
    }

    template <typename OnField>
    constexpr void Emit(OnField& on_field) {
        on_field(std::string_view(key_, key_size_), std::string_view(value_, value_size_));
        state_ = State::kIdle;
    }

    State state_ = State::kIdle;
    bool escaped_ = false;
    char key_[32] = {};
    char value_[48] = {};
    size_t key_size_ = 0;
    size_t value_size_ = 0;
};

// Decimal text to thousandths, without floating point. Java's exponent form only
// appears for values below 0.001 or above 10^7, so it reads as "no usable value".
constexpr bool ParseThousandths(std::string_view text, int64_t* out) {
    if (text.empty()) {
        return false;
    }
    size_t i = 0;
    const bool negative = text[0] == '-';
    if (negative) {
        i = 1;
    }
    int64_t whole = 0;
    int64_t fraction = 0;
    int fraction_digits = 0;
    bool in_fraction = false;
    bool any_digit = false;
    for (; i < text.size(); i++) {
        const char c = text[i];
        if (c == '.' && !in_fraction) {
            in_fraction = true;
        } else if (c >= '0' && c <= '9') {
            any_digit = true;
            if (!in_fraction) {
                if (whole > 1000000000000LL) {
                    return false;
                }
                whole = whole * 10 + (c - '0');
            } else if (fraction_digits < 3) {
                fraction = fraction * 10 + (c - '0');
                fraction_digits++;
            }
        } else {
            return false;
        }
    }
    if (!any_digit) {
        return false;
    }
    while (fraction_digits < 3) {
        fraction *= 10;
        fraction_digits++;
    }
    const int64_t value = whole * 1000 + fraction;
    *out = negative ? -value : value;
    return true;
}

// The multiplier of "W", "kW", "MW", "Wh", "kWh" or "MWh".
constexpr int64_t UnitFactor(std::string_view unit) {
    if (unit.empty()) {
        return 1000;  // SolisCloud's plant values default to kilo units.
    }
    switch (unit[0]) {
        case 'k':
        case 'K':
            return 1000;
        case 'M':
            return 1000000;
        case 'G':
            return 1000000000;
        default:
            return 1;
    }
}

enum class PlantState : uint8_t { kUnknown = 0, kOnline = 1, kOffline = 2, kAlarm = 3 };

struct Reading {
    PlantState state = PlantState::kUnknown;
    int32_t pv_watts = 0;
    int32_t load_watts = 0;
    int32_t grid_watts = 0;     // Positive sells, negative buys.
    int32_t battery_watts = 0;  // Positive charges, negative discharges.
    int soc_percent = 0;
    int32_t pv_today_wh = 0;
    int32_t bought_today_wh = 0;
    int32_t sold_today_wh = 0;
    int64_t data_time_ms = 0;
    bool has_load = false;
    bool has_grid = false;
    bool has_battery = false;
};

// Reads the envelope every endpoint shares. An answer that never says success
// (a truncated body, an HTML error page) stays failed.
class Envelope {
public:
    constexpr void OnField(std::string_view key, std::string_view value) {
        if (key == "success" && !seen_success_) {
            seen_success_ = true;
            success_ = value == "true";
        } else if (key == "code" && !seen_code_) {
            seen_code_ = true;
            code_ok_ = value == "0";
        }
    }

    constexpr bool ok() const { return seen_success_ && success_ && (!seen_code_ || code_ok_); }

private:
    bool seen_success_ = false;
    bool success_ = false;
    bool seen_code_ = false;
    bool code_ok_ = false;
};

// Finds the first plant's id in a userStationList answer.
class StationListParser {
public:
    constexpr void Feed(const char* data, size_t size) {
        scanner_.Feed(data, size, [this](std::string_view key, std::string_view value) {
            envelope_.OnField(key, value);
            if (key == "records") {
                in_records_ = true;
            } else if (in_records_ && key == "id" && id_size_ == 0) {
                for (size_t i = 0; i < value.size() && i < sizeof(id_); i++) {
                    id_[id_size_++] = value[i];
                }
            }
        });
    }

    constexpr bool ok() const { return envelope_.ok(); }
    constexpr std::string_view id() const { return std::string_view(id_, id_size_); }

private:
    JsonScanner scanner_;
    Envelope envelope_;
    bool in_records_ = false;
    char id_[24] = {};
    size_t id_size_ = 0;
};

class StationDetailParser {
public:
    constexpr void Feed(const char* data, size_t size) {
        scanner_.Feed(data, size, [this](std::string_view key, std::string_view value) {
            envelope_.OnField(key, value);
            OnField(key, value);
        });
    }

    constexpr bool ok() const { return envelope_.ok() && amounts_[kPower].present; }

    constexpr Reading reading() const {
        Reading r;
        r.state = state_;
        r.data_time_ms = data_time_ms_;
        r.pv_watts = Scaled(kPower);
        r.load_watts = Scaled(kLoad);
        r.grid_watts = Scaled(kGrid);
        r.battery_watts = Scaled(kBattery);
        r.pv_today_wh = Scaled(kDayEnergy);
        r.bought_today_wh = Scaled(kBought);
        r.sold_today_wh = Scaled(kSold);
        r.soc_percent = static_cast<int>(amounts_[kSoc].thousandths / 1000);

        // A plant without a meter or battery reports zeros, not missing fields.
        r.has_load = r.load_watts != 0;
        r.has_grid = r.grid_watts != 0 || r.bought_today_wh != 0 || r.sold_today_wh != 0;
        r.has_battery = r.soc_percent != 0 || r.battery_watts != 0;
        return r;
    }

private:
    enum Field { kPower, kLoad, kGrid, kBattery, kDayEnergy, kBought, kSold, kSoc, kFieldCount };

    struct Amount {
        int64_t thousandths = 0;
        int64_t factor = 1000;
        bool present = false;
        bool unit_present = false;
    };

    static constexpr int FieldOf(std::string_view key) {
        constexpr std::string_view kNames[kFieldCount] = {
            "power",     "familyLoadPower",        "psum",              "batteryPower",
            "dayEnergy", "gridPurchasedDayEnergy", "gridSellDayEnergy", "batteryPercent",
        };
        for (int i = 0; i < kFieldCount; i++) {
            if (key == kNames[i]) {
                return i;
            }
        }
        return -1;
    }

    constexpr void OnField(std::string_view key, std::string_view value) {
        if (key == "state" && state_ == PlantState::kUnknown) {
            if (value == "1" || value == "2" || value == "3") {
                state_ = static_cast<PlantState>(value[0] - '0');
            }
            return;
        }
        if (key == "dataTimestamp" && data_time_ms_ == 0) {
            int64_t thousandths = 0;
            if (ParseThousandths(value, &thousandths)) {
                data_time_ms_ = thousandths / 1000;
            }
            return;
        }

        // Nested objects can repeat a name, so the first occurrence wins.
        if (key.size() > 3 && key.substr(key.size() - 3) == "Str") {
            const int field = FieldOf(key.substr(0, key.size() - 3));
            if (field >= 0 && !amounts_[field].unit_present) {
                amounts_[field].factor = UnitFactor(value);
                amounts_[field].unit_present = true;
            }
            return;
        }
        const int field = FieldOf(key);
        if (field >= 0 && !amounts_[field].present) {
            amounts_[field].present = ParseThousandths(value, &amounts_[field].thousandths);
        }
    }

    constexpr int32_t Scaled(Field field) const {
        const int64_t base = amounts_[field].thousandths * amounts_[field].factor / 1000;
        if (base > INT32_MAX) {
            return INT32_MAX;
        }
        return base < INT32_MIN ? INT32_MIN : static_cast<int32_t>(base);
    }

    JsonScanner scanner_;
    Envelope envelope_;
    Amount amounts_[kFieldCount] = {};
    PlantState state_ = PlantState::kUnknown;
    int64_t data_time_ms_ = 0;
};

constexpr int HexValue(char c) {
    if (c >= '0' && c <= '9') {
        return c - '0';
    }
    if (c >= 'a' && c <= 'f') {
        return c - 'a' + 10;
    }
    if (c >= 'A' && c <= 'F') {
        return c - 'A' + 10;
    }
    return -1;
}

// Copies one field of a urlencoded form into `out`, returning its length, or -1
// when the field is missing, malformed or does not fit.
constexpr int FormField(std::string_view form, std::string_view name, char* out, size_t capacity) {
    size_t start = 0;
    while (start <= form.size()) {
        size_t end = form.find('&', start);
        if (end == std::string_view::npos) {
            end = form.size();
        }
        const std::string_view pair = form.substr(start, end - start);
        if (pair.size() > name.size() && pair.substr(0, name.size()) == name &&
            pair[name.size()] == '=') {
            const std::string_view encoded = pair.substr(name.size() + 1);
            size_t size = 0;
            for (size_t i = 0; i < encoded.size(); i++) {
                if (size == capacity) {
                    return -1;
                }
                char c = encoded[i];
                if (c == '+') {
                    c = ' ';
                } else if (c == '%') {
                    if (i + 2 >= encoded.size()) {
                        return -1;
                    }
                    const int high = HexValue(encoded[i + 1]);
                    const int low = HexValue(encoded[i + 2]);
                    if (high < 0 || low < 0) {
                        return -1;
                    }
                    c = static_cast<char>(high * 16 + low);
                    i += 2;
                }
                out[size++] = c;
            }
            return static_cast<int>(size);
        }
        start = end + 1;
    }
    return -1;
}

}  // namespace soliscloud

#endif  // SOLISCLOUD_PROTOCOL_H
