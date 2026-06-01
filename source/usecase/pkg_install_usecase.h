#pragma once
#include <string>
#include <vector>
#include "config.h"
#include "clients/remote_client.h"

namespace Usecase {

    struct InstallProgress {
        std::string activity_message;
        uint64_t bytes_downloaded;
        uint64_t bytes_total;
        int status; // 0=InProgress, 1=Done, -1=Error
    };

    class PkgInstallUseCase {
    public:
        PkgInstallUseCase();
        ~PkgInstallUseCase();

        // Starts an installation in the background
        bool StartRemoteInstall(RemoteClient* client, const std::string& url, const std::string& path, RemoteSettings* settings);

        // Get the progress of the current installation
        InstallProgress GetProgress();

    private:
        InstallProgress current_progress;
        
        static void* BackgroundInstallThread(void* arg);
    };

}
