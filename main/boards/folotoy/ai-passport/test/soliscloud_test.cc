// Compile-time tests. The compiler evaluates every check, so this runs with only
// the cross compiler and no hardware:
//   riscv32-esp-elf-g++ -std=gnu++23 -fconstexpr-ops-limit=100000000 -fsyntax-only -I .. soliscloud_test.cc
#include "soliscloud_crypto.h"
#include "soliscloud_protocol.h"

#include <string>
#include <string_view>

using namespace soliscloud;

constexpr std::string Hex(const uint8_t* data, size_t size) {
    constexpr std::string_view kDigits = "0123456789abcdef";
    std::string out;
    for (size_t i = 0; i < size; i++) {
        out.push_back(kDigits[data[i] >> 4]);
        out.push_back(kDigits[data[i] & 15]);
    }
    return out;
}
constexpr bool Md5Is(std::string_view text, std::string_view hex) {
    const auto digest = Md5(text);
    return Hex(digest.data(), digest.size()) == hex;
}
constexpr bool Sha1Is(std::string_view text, std::string_view hex) {
    const auto digest = Sha1(text);
    return Hex(digest.data(), digest.size()) == hex;
}
constexpr bool HmacIs(std::string_view key, std::string_view text, std::string_view hex) {
    const auto digest = HmacSha1(key, text);
    return Hex(digest.data(), digest.size()) == hex;
}
constexpr bool Base64Is(std::string_view text, std::string_view expected) {
    uint8_t bytes[16] = {};
    for (size_t i = 0; i < text.size(); i++) {
        bytes[i] = static_cast<uint8_t>(text[i]);
    }
    return Base64(bytes, text.size()) == expected;
}

// RFC 1321.
static_assert(Md5Is("", "d41d8cd98f00b204e9800998ecf8427e"));
static_assert(Md5Is("abc", "900150983cd24fb0d6963f7d28e17f72"));
static_assert(Md5Is("12345678901234567890123456789012345678901234567890123456789012345678901234567890",
                    "57edf4a22be3c955ac49da2e2107b67a"));
// FIPS 180: empty, one block, and the two-block padding case.
static_assert(Sha1Is("", "da39a3ee5e6b4b0d3255bfef95601890afd80709"));
static_assert(Sha1Is("abc", "a9993e364706816aba3e25717850c26c9cd0d89d"));
static_assert(Sha1Is("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq",
                     "84983e441c3bd26ebaae4aa1f95129e5e54670f1"));
// RFC 2202.
static_assert(HmacIs("Jefe", "what do ya want for nothing?",
                     "effcdf6ae5eb2fa2d27416d5f184df9c259a7c79"));
static_assert(HmacIs(std::string_view(
                         "\xaa\xaa\xaa\xaa\xaa\xaa\xaa\xaa\xaa\xaa\xaa\xaa\xaa\xaa\xaa\xaa"
                         "\xaa\xaa\xaa\xaa\xaa\xaa\xaa\xaa\xaa\xaa\xaa\xaa\xaa\xaa\xaa\xaa"
                         "\xaa\xaa\xaa\xaa\xaa\xaa\xaa\xaa\xaa\xaa\xaa\xaa\xaa\xaa\xaa\xaa"
                         "\xaa\xaa\xaa\xaa\xaa\xaa\xaa\xaa\xaa\xaa\xaa\xaa\xaa\xaa\xaa\xaa"
                         "\xaa\xaa\xaa\xaa\xaa\xaa\xaa\xaa\xaa\xaa\xaa\xaa\xaa\xaa\xaa\xaa",
                         80),
                     "Test Using Larger Than Block-Size Key - Hash Key First",
                     "aa4ae5e15272d00e95705637ce8a3b55ed402112"));
// RFC 4648.
static_assert(Base64Is("f", "Zg=="));
static_assert(Base64Is("fo", "Zm8="));
static_assert(Base64Is("foo", "Zm9v"));
static_assert(Base64Is("foobar", "Zm9vYmFy"));

// SolisCloud's own published example.
static_assert(ContentMd5("{\"pageNo\":1,\"pageSize\":10}") == "kxdxk7rbAsrzSIWgEwhH4w==");
// Expected value computed independently with Python's hashlib and hmac.
static_assert(Sign("test-secret", "kxdxk7rbAsrzSIWgEwhH4w==", "Fri, 26 Jul 2019 06:00:46 GMT",
                   "/v1/api/userStationList") == "ty6sASq/HGK/rDZp74EiLtIrdNw=");

// Decimal parsing.
constexpr int64_t Parsed(std::string_view text) {
    int64_t value = -999999;
    return ParseThousandths(text, &value) ? value : -999999;
}
static_assert(Parsed("3.24") == 3240 && Parsed("0") == 0 && Parsed("-0.5") == -500);
static_assert(Parsed("12") == 12000 && Parsed("1.23456") == 1234);
static_assert(Parsed("null") == -999999 && Parsed("") == -999999 && Parsed("-") == -999999);
static_assert(Parsed("1.0E-4") == -999999 && Parsed("1.2.3") == -999999);

static_assert(UnitFactor("W") == 1 && UnitFactor("kW") == 1000 && UnitFactor("MWh") == 1000000);
static_assert(UnitFactor("") == 1000);

// A plant at noon, in the shape the API documents, with unrelated fields around.
constexpr std::string_view kNoon = R"({
  "success": true, "code": "0", "msg": "success",
  "data": {
    "id": "1298491919448631809", "state": 1, "stationName": "Villa \"Sol\", {test}",
    "power": 3.24, "powerStr": "kW",
    "dayEnergy": 12.3, "dayEnergyStr": "kWh",
    "psum": -1.2, "psumStr": "kW",
    "batteryPower": 1500, "batteryPowerStr": "W",
    "batteryPercent": 82.0,
    "familyLoadPower": 0.64, "familyLoadPowerStr": "kW",
    "gridPurchasedDayEnergy": 1.1, "gridPurchasedDayEnergyStr": "kWh",
    "gridSellDayEnergy": 4.5, "gridSellDayEnergyStr": "kWh",
    "dataTimestamp": "1794024000000",
    "tags": ["power", "state"],
    "nested": {"power": 99.0, "state": 3, "powerStr": "MW"}
  }
})";

// Chunks at every size, because a network read can split anywhere.
constexpr Reading ParseChunked(std::string_view json, size_t chunk) {
    StationDetailParser parser;
    for (size_t i = 0; i < json.size(); i += chunk) {
        parser.Feed(json.data() + i, chunk < json.size() - i ? chunk : json.size() - i);
    }
    return parser.ok() ? parser.reading() : Reading{};
}
constexpr bool IsNoon(const Reading& r) {
    return r.state == PlantState::kOnline && r.pv_watts == 3240 && r.load_watts == 640 &&
           r.grid_watts == -1200 && r.battery_watts == 1500 && r.soc_percent == 82 &&
           r.pv_today_wh == 12300 && r.bought_today_wh == 1100 && r.sold_today_wh == 4500 &&
           r.data_time_ms == 1794024000000LL && r.has_load && r.has_grid && r.has_battery;
}
static_assert(IsNoon(ParseChunked(kNoon, kNoon.size())));
static_assert(IsNoon(ParseChunked(kNoon, 1)));
static_assert(IsNoon(ParseChunked(kNoon, 7)));
static_assert(IsNoon(ParseChunked(kNoon, 256)));

// No meter, no battery: all zeros hide the rows. Offline is still a good answer.
constexpr std::string_view kBare =
    R"({"success":true,"code":"0","data":{"state":2,"power":0,"powerStr":"kW","psum":0,)"
    R"("batteryPower":0,"batteryPercent":0,"familyLoadPower":0,"dayEnergy":0}})";
static_assert(ParseChunked(kBare, 16).state == PlantState::kOffline);
static_assert(!ParseChunked(kBare, 16).has_battery && !ParseChunked(kBare, 16).has_grid);
static_assert(!ParseChunked(kBare, 16).has_load);

// Failures must not look like readings.
constexpr bool DetailOk(std::string_view json) {
    StationDetailParser parser;
    parser.Feed(json.data(), json.size());
    return parser.ok();
}
static_assert(DetailOk(kNoon));
static_assert(!DetailOk(R"({"success":false,"code":"B0001","msg":"no"})"));
static_assert(!DetailOk(R"({"success":true,"code":"1","data":{"power":1}})"));
static_assert(!DetailOk(R"({"success":true,"code":"0","data":{}})"));
static_assert(!DetailOk("<html>Bad gateway</html>"));

// The plant list, with the summary object before the records.
constexpr std::string_view kList =
    R"({"success":true,"code":"0","data":{"page":{"records":[)"
    R"({"id":"1298491919448631809","power":1.0},{"id":"222"}],"total":2},)"
    R"("stationStatusVo":{"all":2}}})";
constexpr bool ListIs(std::string_view json, bool ok, std::string_view id) {
    StationListParser parser;
    parser.Feed(json.data(), json.size());
    return parser.ok() == ok && parser.id() == id;
}
static_assert(ListIs(kList, true, "1298491919448631809"));
static_assert(ListIs(R"({"success":true,"code":"0","data":{"id":"9","page":{"records":[]}}})", true, ""));
static_assert(ListIs(R"({"success":false,"code":"Z1","msg":"x"})", false, ""));

// The setup form.
constexpr bool FormIs(std::string_view form, std::string_view name, int size, std::string_view text) {
    char out[16] = {};
    const int result = FormField(form, name, out, sizeof(out));
    return result == size && (size < 0 || std::string_view(out, size) == text);
}
static_assert(FormIs("key_id=abc&secret=x%2By%3D&plant=", "key_id", 3, "abc"));
static_assert(FormIs("key_id=abc&secret=x%2By%3D&plant=", "secret", 4, "x+y="));
static_assert(FormIs("key_id=abc&secret=x%2By%3D&plant=", "plant", 0, ""));
static_assert(FormIs("a=1", "b", -1, ""));
static_assert(FormIs("akey_id=1", "key_id", -1, ""));
static_assert(FormIs("a=%2", "a", -1, ""));
static_assert(FormIs("a=%zz", "a", -1, ""));
static_assert(FormIs("a=0123456789abcdefg", "a", -1, ""));
static_assert(FormIs("a=b+c", "a", 3, "b c"));
