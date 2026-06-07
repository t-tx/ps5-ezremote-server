#ifndef EZ_HTTP_SERVER_H
#define EZ_HTTP_SERVER_H

#include "../http/httplib.h"
#include <string>

namespace HttpServer
{
    void HandleOptions(const httplib::Request &req, httplib::Response &res);
    void set_cors_header(httplib::Response &res);
    
    void Start();
    void Stop();
    void StartDownloadThread();
    void StopDownloadThread();
    void StartExtractThread();
    void StopExtractThread();
    void StartFileOpThread();
    void StopFileOpThread();
}

extern int http_server_port;

#endif
