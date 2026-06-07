#ifndef EZ_CONFIG_H
#define EZ_CONFIG_H

#include <string>
#include <vector>
#include <list>
#include <string>
#include <algorithm>
#include <map>
#include <set>

#define FILEHOST_DB_PATH "/data/homebrew/ezremote-client/filehost.db"
#define BACKGROUND_QUEUE_PATH "/data/homebrew/ezremote-client/background.queue"
#define CONFIG_PATH "/data/homebrew/ezremote-client/config.json"
#define DATA_PATH "/data/homebrew/ezremote-client"
#define TMP_SFO_PATH DATA_PATH "/tmp_pkg.sfo"
#define PKG_INSTALL_HISTORY_PATH DATA_PATH "/pkg_install_history.json"
#define BG_DOWNLOAD_HISTORY_PATH DATA_PATH "/bg_download_history.json"
#define BG_EXTRACT_HISTORY_PATH DATA_PATH "/bg_extract_history.json"
#define BG_FILEOP_HISTORY_PATH DATA_PATH "/bg_fileop_history.json"
#define DEBUG_SERVER_LOG_PATH DATA_PATH "/ezremote_server.log"
#define NOTIFY_ICON_FILE "/user" DATA_PATH "/sce_sys/icon0.png"
#define CLIENT_ELF_PATH DATA_PATH "/ezremote_client.elf"
#define SERVER_ELF_PATH DATA_PATH "/ezremote-server.elf"

#include "clients/remote_client.h"

#define APP_ID "ezremote-client"
#define HTTP_SERVER_APACHE "Apache"
#define HTTP_SERVER_MS_IIS "Microsoft IIS"
#define HTTP_SERVER_NGINX "Nginx"
#define HTTP_SERVER_NPX_SERVE "Serve"
#define HTTP_SERVER_RCLONE "RClone"
#define HTTP_SERVER_ARCHIVEORG "Archive.org"
#define HTTP_SERVER_MYRIENT "Myrient"
#define HTTP_SERVER_GITHUB "Github"

#define MAX_PKG_HISTORY_RETENTION 1209600000000L



struct HostInfo
{
    int type;
    std::string url;
    std::string http_server_type;
    std::string username;
    std::string password;
    RemoteClient *client;
};

struct PackageInstallData
{
    HostInfo host_info;
    std::string path;
    uint64_t file_size;
    uint64_t timestamp;
    std::string direct_url;
};

struct BgDownloadData {
    HostInfo host_info;
    std::string src_path;
    std::string dest_path;
    uint64_t bytes_transfered;
    uint64_t completed_bytes;
    uint64_t file_size;
    DownloadState state;
    std::string fail_reason;
    uint64_t id;
    uint64_t timestamp;
    uint64_t finished_timestamp = 0;
    int retry_count = 0;
    bool cancel_requested = false;
    bool is_dir = false;
    std::vector<std::string> active_files;
};

struct BgExtractData {
    HostInfo host_info;
    std::string src_path;
    std::string dest_path;
    std::string folder_name;
    uint64_t bytes_transfered;
    uint64_t file_size;
    ExtractState state;
    std::string fail_reason;
    uint64_t id;
    uint64_t timestamp;
    uint64_t finished_timestamp = 0;
    int retry_count = 0;
    bool cancel_requested = false;
};

extern uint64_t *g_bytes_transfered;
extern char working_dir[256];
extern char internal_server_ip[64];
extern char internal_server_port[64];
extern char server_ip[64];
extern char server_port[64];
#include "clients/remote_client.h"

struct RemoteSettings
{
    char site_name[32];
    char server[256];
    char username[33];
    char password[128];
    ClientType type;
    bool enable_rpi;
    uint32_t supported_actions;
    char http_server_type[24];
    char default_directory[256];
};

class RemoteClient;

extern RemoteSettings *remote_settings;
extern RemoteClient *remoteclient;
extern bool enable_direct_download_redirect;
extern int http_int_server_port;

extern char alldebrid_api_key[64];
extern char realdebrid_api_key[64];
extern char CACERT_FILE[256];
extern bool show_hidden_files;
extern std::list<BgDownloadData> bg_download_list;
extern std::list<BgExtractData> bg_extract_list;
extern std::list<BgFileOpData> bg_fileop_list;

namespace CONFIG
{
    PackageInstallData* GetPackageInstallHostData(const std::string &hash);
    void AddPackageInstallHostData(const std::string &hash, PackageInstallData pkg_data);
    void RemovePackageInstallHostData(const std::string &hash);
    void LoadPackageInstallHostData();
    void SavePackageInstallHostData();
    void AddBgDownloadData(BgDownloadData bg_download_data);
    void LoadBgDownloadData();
    void SaveBgDownloadData();
    void LockDownloadList();
    void UnlockDownloadList();

    void AddBgExtractData(BgExtractData bg_extract_data);
    void LoadBgExtractData();
    void SaveBgExtractData();
    void LockExtractList();
    void UnlockExtractList();

    void AddBgFileOpData(BgFileOpData bg_fileop_data);
    void LoadBgFileOpData();
    void SaveBgFileOpData();
    void LoadConfiguredSites();
    void SaveConfiguredSites();
    void LockFileOpList();
    void UnlockFileOpList();
}
#endif
extern std::vector<RemoteSettings> configured_sites;

