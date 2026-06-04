#undef main

#include <string>
#include <vector>
#include <stdio.h>
#include <stdlib.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <unistd.h>

#include "server/http_server.h"
#include "config.h"
#include "util.h"
#include "usecase/dpi_usecase.h"
#include "server/legacy_dpi_server.h"
#include "dbglogger.h"

int main(int argc, char *argv[])
{
	dbglogger_init_str("file:/data/homebrew/ezremote-client/server.log");
	dbglogger_log("ezremote-server dbglogger started.");

    CONFIG::LoadPackageInstallHostData();
    CONFIG::LoadBgDownloadData();
    CONFIG::LoadBgExtractData();
    CONFIG::LoadBgFileOpData();
    DpiUseCase::Initialize();
    LegacyDpiServer::Start();

    HttpServer::StartDownloadThread();
    HttpServer::StartExtractThread();
    HttpServer::StartFileOpThread();
    HttpServer::Start();
    LegacyDpiServer::Stop();
    Util::Notify("ezRemote Server stopped.");

    return 0;
}
