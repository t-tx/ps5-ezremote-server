#ifndef DPI_USECASE_H
#define DPI_USECASE_H

#include <string>

namespace DpiUseCase {
    int Initialize();
    int InstallPackage(const std::string& uri, const std::string& title, const std::string& icon_url, const std::string& content_id);
}

#endif
