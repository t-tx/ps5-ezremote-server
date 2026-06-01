#pragma once
#include <string>
#include <vector>
#include "clients/remote_client.h"

namespace Usecase {

    class FileManagerUseCase {
    public:
        FileManagerUseCase();
        ~FileManagerUseCase();

        // List files in a directory
        bool ListFiles(RemoteClient* client, const std::string& path, std::vector<DirEntry>& files);
        
        // Mkdir, Delete, Copy etc can be added here
    };

}
