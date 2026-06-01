#pragma once
#include <string>

namespace PS5_API {
    namespace Installer {
        int Initialize();
        void Terminate();
        int GetInstallStatus(const std::string& content_id, void* progress_info);
        int InstallRemotePkg(const std::string& url); // Example raw api call
    }
}
