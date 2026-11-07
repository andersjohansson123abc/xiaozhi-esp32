#include "solis_setup_server.h"

#include "solis_monitor.h"

#include <esp_log.h>

#include <string>

#define TAG "SolisSetup"

namespace {

constexpr size_t kMaxFormBytes = 400;
constexpr size_t kMaxFieldBytes = 128;

constexpr char kFormPage[] =
    "<!doctype html><html lang=sv><meta charset=utf-8>"
    "<meta name=viewport content='width=device-width,initial-scale=1'>"
    "<title>Solis-koppling</title>"
    "<style>body{font:16px system-ui;max-width:26em;margin:2em auto;padding:0 1em}"
    "label{display:block;margin:1em 0 .3em}"
    "input{width:100%;box-sizing:border-box;padding:.6em;font:inherit}"
    "button{margin-top:1.5em;padding:.7em 1.4em;font:inherit}"
    "small{color:#666}</style>"
    "<h1>Solis-koppling</h1>"
    "<p>Nyckeln finns i SolisCloud under Service &rarr; API Management. "
    "Den sparas bara i den h&auml;r enheten.</p>"
    "<form method=post action=/save autocomplete=off>"
    "<label for=k>KeyID</label><input id=k name=key_id required>"
    "<label for=s>KeySecret</label><input id=s name=secret type=password required>"
    "<label for=p>Anl&auml;ggnings-ID</label><input id=p name=plant inputmode=numeric>"
    "<small>Valfritt. Tomt betyder kontots f&ouml;rsta anl&auml;ggning.</small><br>"
    "<button>Spara</button></form></html>";

constexpr char kSavedPage[] =
    "<!doctype html><html lang=sv><meta charset=utf-8><title>Sparat</title>"
    "<p>Sparat. Enheten h&auml;mtar nu data fr&aring;n SolisCloud "
    "och den h&auml;r sidan st&auml;ngs.</p></html>";

constexpr char kInvalidPage[] =
    "<!doctype html><html lang=sv><meta charset=utf-8><title>Fel</title>"
    "<p>KeyID och KeySecret m&aring;ste fyllas i. <a href=/>Tillbaka</a></p></html>";

esp_err_t SendPage(httpd_req_t* request, const char* page) {
    httpd_resp_set_type(request, "text/html; charset=utf-8");
    httpd_resp_set_hdr(request, "Cache-Control", "no-store");
    return httpd_resp_sendstr(request, page);
}

std::string Field(std::string_view form, std::string_view name) {
    char value[kMaxFieldBytes];
    const int size = soliscloud::FormField(form, name, value, sizeof(value));
    return size > 0 ? std::string(value, size) : std::string();
}

}  // namespace

esp_err_t SolisSetupServer::HandleForm(httpd_req_t* request) {
    return SendPage(request, kFormPage);
}

esp_err_t SolisSetupServer::HandleSave(httpd_req_t* request) {
    auto* self = static_cast<SolisSetupServer*>(request->user_ctx);
    if (request->content_len == 0 || request->content_len > kMaxFormBytes) {
        return SendPage(request, kInvalidPage);
    }

    char body[kMaxFormBytes];
    size_t received = 0;
    while (received < request->content_len) {
        const int count = httpd_req_recv(request, body + received, request->content_len - received);
        if (count <= 0) {
            return ESP_FAIL;
        }
        received += count;
    }

    const std::string_view form(body, received);
    if (!self->monitor_->SetCredentials(Field(form, "key_id"), Field(form, "secret"),
                                        Field(form, "plant"))) {
        return SendPage(request, kInvalidPage);
    }
    ESP_LOGI(TAG, "SolisCloud key stored");
    return SendPage(request, kSavedPage);
}

void SolisSetupServer::Start() {
    if (server_ != nullptr) {
        return;
    }
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.max_uri_handlers = 2;
    config.max_open_sockets = 2;
    config.lru_purge_enable = true;
    if (httpd_start(&server_, &config) != ESP_OK) {
        ESP_LOGW(TAG, "Setup page could not be started");
        server_ = nullptr;
        return;
    }

    httpd_uri_t form = {};
    form.uri = "/";
    form.method = HTTP_GET;
    form.handler = HandleForm;
    form.user_ctx = this;
    httpd_register_uri_handler(server_, &form);

    httpd_uri_t save = {};
    save.uri = "/save";
    save.method = HTTP_POST;
    save.handler = HandleSave;
    save.user_ctx = this;
    httpd_register_uri_handler(server_, &save);
}

void SolisSetupServer::Stop() {
    if (server_ != nullptr) {
        httpd_stop(server_);
        server_ = nullptr;
    }
}
