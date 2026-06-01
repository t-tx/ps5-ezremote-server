#include "pkg_install_usecase.h"
#include "installer.h"
#include <pthread.h>

namespace Usecase {

    struct InstallJobArgs {
        RemoteClient* client;
        std::string url;
        std::string path;
        RemoteSettings* settings;
        PkgInstallUseCase* instance;
    };

    PkgInstallUseCase::PkgInstallUseCase() {
        current_progress.status = 1;
        current_progress.bytes_downloaded = 0;
        current_progress.bytes_total = 0;
    }

    PkgInstallUseCase::~PkgInstallUseCase() {
    }

    InstallProgress PkgInstallUseCase::GetProgress() {
        return current_progress;
    }

    void* PkgInstallUseCase::BackgroundInstallThread(void* arg) {
        InstallJobArgs* job = (InstallJobArgs*)arg;
        
        job->instance->current_progress.status = 0;
        job->instance->current_progress.activity_message = "Preparing installation...";

        // Real code would poll headers and install
        pkg_header header;
        memset(&header, 0, sizeof(header));
        
        if (job->client->Head(job->path, &header, sizeof(header)) == 0) {
            job->instance->current_progress.status = -1;
            job->instance->current_progress.activity_message = "Failed to fetch header.";
            delete job;
            return NULL;
        }

        job->instance->current_progress.activity_message = "Installing PKG...";
        
        std::string title = INSTALLER::GetRemotePkgTitle(job->client, job->path, &header);
        int ret = INSTALLER::InstallRemotePkg(job->client, job->url, &header, title, job->path);
        
        if (ret == 0) {
            job->instance->current_progress.status = 1;
            job->instance->current_progress.activity_message = "Installation triggered successfully.";
        } else {
            job->instance->current_progress.status = -1;
            job->instance->current_progress.activity_message = "Installation failed.";
        }

        delete job;
        return NULL;
    }

    bool PkgInstallUseCase::StartRemoteInstall(RemoteClient* client, const std::string& url, const std::string& path, RemoteSettings* settings) {
        if (current_progress.status == 0) {
            return false; // Already installing
        }

        InstallJobArgs* job = new InstallJobArgs();
        job->client = client;
        job->url = url;
        job->path = path;
        job->settings = settings;
        job->instance = this;

        pthread_t thread_id;
        pthread_create(&thread_id, NULL, BackgroundInstallThread, job);
        pthread_detach(thread_id);

        return true;
    }
}
