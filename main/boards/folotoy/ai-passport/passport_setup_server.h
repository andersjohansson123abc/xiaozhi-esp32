#ifndef PASSPORT_SETUP_SERVER_H
#define PASSPORT_SETUP_SERVER_H

#include <esp_http_server.h>

class GrokMonitor;
class SolisMonitor;

// A small web server on the local network for entering the SolisCloud key and, on
// the Grok build, the relay's device token.
//
// Neither can go through the voice path: everything said to the assistant passes
// the XiaoZhi server and a language model. The server only runs while something is
// missing, each form only stores what is missing, and nothing stored is ever shown.
class PassportSetupServer {
public:
    // `grok` is null on the builds without the Grok monitor.
    PassportSetupServer(SolisMonitor* solis, GrokMonitor* grok) : solis_(solis), grok_(grok) {}

    void Start();
    void Stop();
    bool IsRunning() const { return server_ != nullptr; }

private:
    static esp_err_t HandleForm(httpd_req_t* request);
    static esp_err_t HandleSave(httpd_req_t* request);
    static esp_err_t HandleGrokForm(httpd_req_t* request);
    static esp_err_t HandleGrokSave(httpd_req_t* request);

    SolisMonitor* solis_;
    GrokMonitor* grok_;
    httpd_handle_t server_ = nullptr;
};

#endif  // PASSPORT_SETUP_SERVER_H
