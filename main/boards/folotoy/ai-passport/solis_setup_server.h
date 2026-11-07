#ifndef SOLIS_SETUP_SERVER_H
#define SOLIS_SETUP_SERVER_H

#include <esp_http_server.h>

class SolisMonitor;

// A one-page web server on the local network for entering the SolisCloud key.
//
// The key cannot go through the voice path: everything said to the assistant
// passes the XiaoZhi server and a language model. The page only exists while the
// board has no key, and it can store a key but never shows one.
class SolisSetupServer {
public:
    explicit SolisSetupServer(SolisMonitor* monitor) : monitor_(monitor) {}

    void Start();
    void Stop();
    bool IsRunning() const { return server_ != nullptr; }

private:
    static esp_err_t HandleForm(httpd_req_t* request);
    static esp_err_t HandleSave(httpd_req_t* request);

    SolisMonitor* monitor_;
    httpd_handle_t server_ = nullptr;
};

#endif  // SOLIS_SETUP_SERVER_H
