#include "passport_setup_server.h"

#include "grok_monitor.h"
#include "solis_monitor.h"

#include <esp_log.h>

#include <string>

#define TAG "PassportSetup"

namespace {

constexpr size_t kMaxFormBytes = 400;
constexpr size_t kMaxFieldBytes = 128;

constexpr char kStyle[] =
    "<style>body{font:16px system-ui;max-width:26em;margin:2em auto;padding:0 1em}"
    "label{display:block;margin:1em 0 .3em}"
    "input{width:100%;box-sizing:border-box;padding:.6em;font:inherit}"
    "button{margin-top:1.5em;padding:.7em 1.4em;font:inherit}"
    "small{color:#666}</style>";

constexpr char kFormPage[] =
    "<h1>Solis-koppling</h1>"
    "<p>Nyckeln finns i SolisCloud under Service &rarr; API Management. "
    "Den sparas bara i den h&auml;r enheten.</p>"
    "<form method=post action=/save autocomplete=off>"
    "<label for=k>KeyID</label><input id=k name=key_id required>"
    "<label for=s>KeySecret</label><input id=s name=secret type=password required>"
    "<label for=p>Anl&auml;ggnings-ID</label><input id=p name=plant inputmode=numeric>"
    "<small>Valfritt. Tomt betyder kontots f&ouml;rsta anl&auml;ggning.</small><br>"
    "<button>Spara</button></form>";

constexpr char kGrokLink[] = "<p><a href=/grok>Koppla Grok-botar</a></p>";

constexpr char kGrokFormPage[] =
    "<h1>Grok-koppling</h1>"
    "<p>Adressen och enhetens token f&aring;r du n&auml;r du driftsatt rel&auml;et. "
    "De sparas bara i den h&auml;r enheten.</p>"
    "<form method=post action=/grok/save autocomplete=off>"
    "<label for=u>Rel&auml;ets adress</label>"
    "<input id=u name=url type=url required placeholder=https://>"
    "<label for=t>Enhetens token</label><input id=t name=token type=password required>"
    "<button>Spara</button></form>";

constexpr char kSavedPage[] =
    "<p>Sparat. Enheten h&auml;mtar nu data fr&aring;n SolisCloud "
    "och den h&auml;r sidan st&auml;ngs.</p>";

constexpr char kGrokSavedPage[] = "<p>Sparat. Enheten h&auml;mtar nu dina Grok-botar.</p>";

constexpr char kInvalidPage[] =
    "<p>KeyID och KeySecret m&aring;ste fyllas i. <a href=/>Tillbaka</a></p>";

constexpr char kGrokInvalidPage[] =
    "<p>Adressen m&aring;ste b&ouml;rja med https:// och token m&aring;ste fyllas i. "
    "<a href=/grok>Tillbaka</a></p>";

constexpr char kAlreadySetPage[] = "<p>Det h&auml;r &auml;r redan kopplat.</p>";

esp_err_t SendPage(httpd_req_t* request, const char* title, const char* page,
                   const char* extra = "") {
    std::string html = "<!doctype html><html lang=sv><meta charset=utf-8>"
                       "<meta name=viewport content='width=device-width,initial-scale=1'><title>";
    html += title;
    html += "</title>";
    html += kStyle;
    html += page;
    html += extra;
    html += "</html>";
    httpd_resp_set_type(request, "text/html; charset=utf-8");
    httpd_resp_set_hdr(request, "Cache-Control", "no-store");
    return httpd_resp_sendstr(request, html.c_str());
}

std::string Field(std::string_view form, std::string_view name) {
    char value[kMaxFieldBytes];
    const int size = soliscloud::FormField(form, name, value, sizeof(value));
    return size > 0 ? std::string(value, size) : std::string();
}

// Returns the body, or nothing for one that is empty, too long or cut off.
bool ReadForm(httpd_req_t* request, std::string& out) {
    if (request->content_len == 0 || request->content_len > kMaxFormBytes) {
        return false;
    }
    char body[kMaxFormBytes];
    size_t received = 0;
    while (received < request->content_len) {
        const int count = httpd_req_recv(request, body + received, request->content_len - received);
        if (count <= 0) {
            return false;
        }
        received += count;
    }
    out.assign(body, received);
    return true;
}

}  // namespace

esp_err_t PassportSetupServer::HandleForm(httpd_req_t* request) {
    auto* self = static_cast<PassportSetupServer*>(request->user_ctx);
    if (self->solis_->IsConfigured()) {
        return HandleGrokForm(request);
    }
    const bool offer_grok = self->grok_ != nullptr && !self->grok_->IsConfigured();
    return SendPage(request, "Solis-koppling", kFormPage, offer_grok ? kGrokLink : "");
}

esp_err_t PassportSetupServer::HandleSave(httpd_req_t* request) {
    auto* self = static_cast<PassportSetupServer*>(request->user_ctx);
    if (self->solis_->IsConfigured()) {
        return SendPage(request, "Kopplat", kAlreadySetPage);
    }
    std::string form;
    if (!ReadForm(request, form)) {
        return SendPage(request, "Fel", kInvalidPage);
    }
    if (!self->solis_->SetCredentials(Field(form, "key_id"), Field(form, "secret"),
                                      Field(form, "plant"))) {
        return SendPage(request, "Fel", kInvalidPage);
    }
    ESP_LOGI(TAG, "SolisCloud key stored");
    return SendPage(request, "Sparat", kSavedPage);
}

esp_err_t PassportSetupServer::HandleGrokForm(httpd_req_t* request) {
    auto* self = static_cast<PassportSetupServer*>(request->user_ctx);
    if (self->grok_ == nullptr || self->grok_->IsConfigured()) {
        return SendPage(request, "Kopplat", kAlreadySetPage);
    }
    return SendPage(request, "Grok-koppling", kGrokFormPage);
}

esp_err_t PassportSetupServer::HandleGrokSave(httpd_req_t* request) {
    auto* self = static_cast<PassportSetupServer*>(request->user_ctx);
    if (self->grok_ == nullptr || self->grok_->IsConfigured()) {
        return SendPage(request, "Kopplat", kAlreadySetPage);
    }
    std::string form;
    if (!ReadForm(request, form)) {
        return SendPage(request, "Fel", kGrokInvalidPage);
    }
    if (!self->grok_->SetCredentials(Field(form, "url"), Field(form, "token"))) {
        return SendPage(request, "Fel", kGrokInvalidPage);
    }
    ESP_LOGI(TAG, "Relay stored");
    return SendPage(request, "Sparat", kGrokSavedPage);
}

void PassportSetupServer::Start() {
    if (server_ != nullptr) {
        return;
    }
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.max_uri_handlers = 4;
    config.max_open_sockets = 2;
    config.lru_purge_enable = true;
    if (httpd_start(&server_, &config) != ESP_OK) {
        ESP_LOGW(TAG, "Setup page could not be started");
        server_ = nullptr;
        return;
    }

    const httpd_uri_t routes[] = {
        {.uri = "/", .method = HTTP_GET, .handler = HandleForm, .user_ctx = this},
        {.uri = "/save", .method = HTTP_POST, .handler = HandleSave, .user_ctx = this},
        {.uri = "/grok", .method = HTTP_GET, .handler = HandleGrokForm, .user_ctx = this},
        {.uri = "/grok/save", .method = HTTP_POST, .handler = HandleGrokSave, .user_ctx = this},
    };
    for (const httpd_uri_t& route : routes) {
        httpd_register_uri_handler(server_, &route);
    }
}

void PassportSetupServer::Stop() {
    if (server_ != nullptr) {
        httpd_stop(server_);
        server_ = nullptr;
    }
}
