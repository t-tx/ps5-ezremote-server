#include "legacy_dpi_server.h"
#include "../usecase/dpi_usecase.h"
#include "../util.h"
#include <string>
#include <vector>
#include <pthread.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <unistd.h>
#include <string.h>

namespace LegacyDpiServer {
    static pthread_t thread_id;
    static int server_fd = -1;
    static bool is_running = false;

    static void* ServerThread(void* arg) {
        int new_socket;
        struct sockaddr_in address;
        int addrlen = sizeof(address);
        char buffer[4096] = {0};

        if ((server_fd = socket(AF_INET, SOCK_STREAM, 0)) == 0) {
            Util::Notify("Legacy DPI socket creation failed");
            return nullptr;
        }

        int opt = 1;
        setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

        address.sin_family = AF_INET;
        address.sin_addr.s_addr = INADDR_ANY;
        address.sin_port = htons(9040);

        if (bind(server_fd, (struct sockaddr *)&address, sizeof(address)) < 0) {
            Util::Notify("Legacy DPI Port 9040 already in use");
            close(server_fd);
            server_fd = -1;
            return nullptr;
        }

        if (listen(server_fd, 3) < 0) {
            Util::Notify("Legacy DPI listen failed");
            close(server_fd);
            server_fd = -1;
            return nullptr;
        }

        while (is_running) {
            new_socket = accept(server_fd, (struct sockaddr *)&address, (socklen_t *)&addrlen);
            if (new_socket < 0) {
                if (!is_running) break;
                continue;
            }

            timeval tv;
            tv.tv_sec = 1;
            tv.tv_usec = 0;
            setsockopt(new_socket, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
            int yes = 1;
            setsockopt(new_socket, IPPROTO_TCP, TCP_NODELAY, &yes, sizeof(int));

            memset(buffer, 0, sizeof(buffer));
            int valread = recv(new_socket, buffer, 4095, 0);            
            if (valread > 0) {
                buffer[valread] = '\0';
                char* pos = strchr(buffer, '\r');
                if (pos != nullptr) *pos = 0;
                pos = strchr(buffer, '\n');
                if (pos != nullptr) *pos = 0;

                if (strncmp(buffer, "stop", 4) == 0) {
                    close(new_socket);
                    continue; // we just ignore stop command so it doesn't kill the main server
                }

                std::string payload(buffer);
                std::vector<std::string> parts = Util::Split(payload, "|");
                
                std::string url = parts.size() > 0 ? parts[0] : payload;
                std::string title = parts.size() > 1 ? parts[1] : "";
                std::string icon = parts.size() > 2 ? parts[2] : "";
                std::string content_id = parts.size() > 3 ? parts[3] : "";

                int ret = DpiUseCase::InstallPackage(url, title, icon, content_id);

                char res_buffer[32];
                if (ret == 0) {
                    sprintf(res_buffer, "0");
                } else {
                    sprintf(res_buffer, "%d", ret);
                }
                send(new_socket, res_buffer, strlen(res_buffer), 0);
            }
            close(new_socket);
        }

        if (server_fd != -1) {
            close(server_fd);
            server_fd = -1;
        }
        return nullptr;
    }

    void Start() {
        if (is_running) return;
        is_running = true;
        pthread_create(&thread_id, NULL, ServerThread, NULL);
    }

    void Stop() {
        is_running = false;
        if (server_fd != -1) {
            shutdown(server_fd, SHUT_RDWR);
            close(server_fd);
            server_fd = -1;
        }
        pthread_join(thread_id, NULL);
    }
}
