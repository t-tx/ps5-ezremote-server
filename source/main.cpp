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
//#include "dbglogger.h"

int main(int argc, char *argv[])
{
	//dbglogger_init();
	//dbglogger_log("If you see this you've set up dbglogger correctly.");

    CONFIG::LoadPackageInstallHostData();
    CONFIG::LoadBgDownloadData();
    DpiUseCase::Initialize();
    LegacyDpiServer::Start();
    HttpServer::StartDownloadThread();
    HttpServer::Start();
    LegacyDpiServer::Stop();
    Util::Notify("ezRemote Server stopped.");

    return 0;
}
