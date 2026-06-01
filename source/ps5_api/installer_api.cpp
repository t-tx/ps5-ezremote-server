#include "installer_api.h"
#include "sceAppInstUtil.h"

namespace PS5_API {
    namespace Installer {
        int Initialize() {
            return sceAppInstUtilInitialize();
        }

        void Terminate() {
            // Not defined in header
        }

        int GetInstallStatus(const std::string& content_id, void* progress_info) {
            return sceAppInstUtilGetInstallStatus(content_id.c_str(), (SceAppInstallStatusInstalled*)progress_info);
        }

        int InstallRemotePkg(const std::string& url) {
            // Stubbed raw API
            return 0;
        }
    }
}
