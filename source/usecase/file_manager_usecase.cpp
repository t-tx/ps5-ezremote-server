#include "file_manager_usecase.h"

namespace Usecase {

    FileManagerUseCase::FileManagerUseCase() {
    }

    FileManagerUseCase::~FileManagerUseCase() {
    }

    bool FileManagerUseCase::ListFiles(RemoteClient* client, const std::string& path, std::vector<DirEntry>& files) {
        if (!client) return false;
        
        files = client->ListDir(path);
        return true;
    }

}
