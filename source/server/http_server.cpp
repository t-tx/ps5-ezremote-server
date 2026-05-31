#include <string>
#include <cstdio>
#include <atomic>
#include <map>
#include <fstream>
#include <sstream>
#include <json-c/json.h>
#include "http/httplib.h"
#include "server/http_server.h"
#include "clients/remote_client.h"
#include "clients/archiveorg.h"
#include "clients/baseclient.h"
#include "clients/ftpclient.h"
#include "clients/nfsclient.h"
#include "clients/smbclient.h"
#include "clients/sftpclient.h"
#include "clients/webdav.h"
#include "config.h"
#include "fs.h"
#include "util.h"
#include "usecase/dpi_usecase.h"
#include "dbglogger.h"

#define SUCCESS_MSG "{ \"result\": { \"success\": true, \"error\": null } }"
#define FAILURE_MSG "{ \"result\": { \"success\": false, \"error\": \"%s\" } }"
#define SUCCESS_MSG_LEN 48
#define PKG_INITIAL_REQUEST_SIZE 8388608ul



using namespace httplib;

Server *svr;
int http_server_port = 6701;
static pthread_t bg_download_thread;
static std::atomic<bool> bg_download_running{false};
static bool bg_download_thread_started = false;
static uint64_t g_dl_offset;

namespace HttpServer
{
    static int FtpCallback(int64_t xfered, void *arg)
    {
        return 1;
    }

    static int DownloadFtpCallback(int64_t xfered, void *arg)
    {
        *g_bytes_transfered = g_dl_offset + xfered;
        return 1;
    }

    std::string dump_headers(const Headers &headers)
    {
        std::string s;
        char buf[BUFSIZ];

        for (auto it = headers.begin(); it != headers.end(); ++it)
        {
            const auto &x = *it;
            snprintf(buf, sizeof(buf), "%s: %s\n", x.first.c_str(), x.second.c_str());
            s += buf;
        }

        return s;
    }

    std::string log(const Request &req, const Response &res)
    {
        std::string s;
        char buf[BUFSIZ];

        s += "================================\n";

        snprintf(buf, sizeof(buf), "%s %s %s", req.method.c_str(),
                 req.version.c_str(), req.path.c_str());
        s += buf;

        std::string query;
        for (auto it = req.params.begin(); it != req.params.end(); ++it)
        {
            const auto &x = *it;
            snprintf(buf, sizeof(buf), "%c%s=%s",
                     (it == req.params.begin()) ? '?' : '&', x.first.c_str(),
                     x.second.c_str());
            query += buf;
        }
        snprintf(buf, sizeof(buf), "%s\n", query.c_str());
        s += buf;

        s += dump_headers(req.headers);

        s += "--------------------------------\n";

        snprintf(buf, sizeof(buf), "%d %s\n", res.status, res.version.c_str());
        s += buf;
        s += dump_headers(res.headers);
        s += "\n";

        if (!res.body.empty())
        {
            s += res.body;
        }

        s += "\n";

        return s;
    }

    void failed(Response &res, int status, const std::string &msg)
    {
        res.status = status;
        std::string response_msg = "{ \"result\": { \"success\": false, \"error\": \"" + msg + "\" } }";
        res.set_content(response_msg.c_str(), response_msg.length(), "application/json");
        return;
    }

    void bad_request(Response &res, const std::string &msg)
    {
        failed(res, 200, msg);
        return;
    }

    void success(Response &res)
    {
        res.status = 200;
        res.set_content(SUCCESS_MSG, SUCCESS_MSG_LEN, "application/json");
        return;
    }

    static RemoteClient *GetRemoteClient(HostInfo *host_info)
    {
        if (host_info == nullptr)
            return nullptr;

        RemoteClient *tmp_client = nullptr;

        if (host_info->type == CLIENT_TYPE_HTTP_SERVER)
        {
            if (host_info->http_server_type.compare(HTTP_SERVER_ARCHIVEORG) == 0)
            {
                tmp_client = new ArchiveOrgClient();
            }
            else
            {
                tmp_client = new BaseClient();
            }
        }
        else if (host_info->type == CLIENT_TYPE_SMB)
        {
            tmp_client = new SmbClient();
        }
        else if (host_info->type == CLIENT_TYPE_FILEHOST)
        {
            tmp_client = new BaseClient();
        }
        else if (host_info->type == CLIENT_TYPE_WEBDAV)
        {
            tmp_client = new WebDAVClient();
        }
        else if (host_info->type == CLIENT_TYPE_SFTP)
        {
            tmp_client = new SFTPClient();
        }
        else if (host_info->type == CLIENT_TYPE_NFS)
        {
            tmp_client = new NfsClient();
        }
        else if (host_info->type == CLIENT_TYPE_FTP)
        {
            tmp_client = new FtpClient();
            FtpClient *ftp_client = (FtpClient*) tmp_client;
            ftp_client->SetCallbackXferFunction(FtpCallback);
        }

        if (tmp_client != nullptr && !tmp_client->Connect(host_info->url, host_info->username, host_info->password))
        {
            tmp_client->Quit();
            delete tmp_client;
            return nullptr;
        }

        return tmp_client;
    }

    static void DeleteRemoteClient(RemoteClient *tmp_client)
    {
        if (tmp_client == nullptr)
            return;

        tmp_client->Quit();
        delete tmp_client;
    }

    static bool GetRequestedRange(const Request &req, uint64_t file_size, uint64_t &start, uint64_t &end)
    {
        if (req.ranges.size() != 1)
            return false;

        Range range = req.ranges[0];
        if (range.first < 0)
            return false;

        start = (uint64_t)range.first;
        if (range.second >= 0)
        {
            end = (uint64_t)range.second;
        }
        else
        {
            if (file_size == 0 || start >= file_size)
                return false;
            end = file_size - 1;
        }

        if (end < start)
            return false;
        if (file_size > 0 && end >= file_size)
            return false;

        return true;
    }

    static void ClearRequestRanges(const Request &req)
    {
        const_cast<Request &>(req).ranges.clear();
    }

    static void range_not_satisfiable(Response &res, uint64_t file_size, const std::string &msg)
    {
        res.status = 416;
        res.set_header("Accept-Ranges", "bytes");
        if (file_size > 0)
            res.set_header("Content-Range", "bytes */" + std::to_string(file_size));
        res.set_content(msg, "text/plain");
    }

    static bool CompleteDownloadedFile(const char *temp_file, const std::string &dest_path, uint64_t expected_size)
    {
        if (!FS::FileExists(temp_file))
            return false;

        int64_t actual_size = FS::GetSize(temp_file);
        if (actual_size < 0 || (expected_size > 0 && static_cast<uint64_t>(actual_size) != expected_size))
            return false;

        return std::rename(temp_file, dest_path.c_str()) == 0;
    }

    void *DownloadFilesThread(void *argp)
    {
        char temp_file[2049];
        uint64_t tmp_file_size;
        int ret;

        while (bg_download_running.load())
        {
            BgDownloadData* active_dl = nullptr;

            CONFIG::LockDownloadList();
            for (auto it = bg_download_list.begin(); it != bg_download_list.end(); ++it)
            {
                if (it->state == STATE_PENDING || it->state == STATE_DOWNLOADING || it->state == STATE_RESUMED)
                {
                    active_dl = &(*it);
                    break;
                }
            }
            CONFIG::UnlockDownloadList();

            if (active_dl != nullptr)
            {
                if (active_dl->state == STATE_PENDING)
                {
                    RemoteClient *tmp_client = GetRemoteClient(&(active_dl->host_info));
                    if (tmp_client == nullptr)
                    {
                        CONFIG::LockDownloadList();
                        active_dl->state = STATE_FAILED;
                        active_dl->fail_reason = "Failed to connect to host";
                        CONFIG::UnlockDownloadList();
                        CONFIG::SaveBgDownloadData();
                        Util::RichNotify(active_dl->id, "Failed to connect for download %s", active_dl->dest_path.c_str());
                        continue;
                    }

                    g_bytes_transfered = &(active_dl->bytes_transfered);
                    if (active_dl->host_info.type == CLIENT_TYPE_FTP)
                    {
                        FtpClient *ftpclient = (FtpClient*)tmp_client;
                        g_dl_offset = 0;
                        ftpclient->SetCallbackBytes(1);
                        ftpclient->SetCallbackXferFunction(DownloadFtpCallback);
                    }

                    CONFIG::LockDownloadList();
                    active_dl->state = STATE_DOWNLOADING;
                    CONFIG::UnlockDownloadList();
                    CONFIG::SaveBgDownloadData();

                    snprintf(temp_file, sizeof(temp_file), "%s.tmp", active_dl->dest_path.c_str());
                    Util::RichNotify(active_dl->id, "Started download %s", active_dl->dest_path.c_str());

                    ret = tmp_client->Get(temp_file, active_dl->src_path);

                    CONFIG::LockDownloadList();
                    if (ret == 0 || !CompleteDownloadedFile(temp_file, active_dl->dest_path, active_dl->file_size))
                    {
                        active_dl->state = STATE_FAILED;
                        active_dl->fail_reason = "Failed to download or write file";
                        Util::RichNotify(active_dl->id, "Failed to download %s", active_dl->dest_path.c_str());
                    }
                    else
                    {
                        Util::RichNotify(active_dl->id, "Completed download %s", active_dl->dest_path.c_str());
                        active_dl->state = STATE_SUCCESS;
                    }
                    CONFIG::UnlockDownloadList();
                    CONFIG::SaveBgDownloadData();

                    DeleteRemoteClient(tmp_client);
                }
                else if (active_dl->state == STATE_DOWNLOADING || active_dl->state == STATE_RESUMED)
                {
                    // Resume interrupted download
                    RemoteClient *tmp_client = GetRemoteClient(&(active_dl->host_info));
                    if (tmp_client == nullptr)
                    {
                        CONFIG::LockDownloadList();
                        active_dl->state = STATE_FAILED;
                        active_dl->fail_reason = "Failed to reconnect to host";
                        CONFIG::UnlockDownloadList();
                        CONFIG::SaveBgDownloadData();
                        Util::RichNotify(active_dl->id, "Failed to reconnect for download %s", active_dl->dest_path.c_str());
                        continue;
                    }

                    g_bytes_transfered = &(active_dl->bytes_transfered);
                    if (active_dl->host_info.type == CLIENT_TYPE_FTP)
                    {
                        FtpClient *ftpclient = (FtpClient*)tmp_client;
                        ftpclient->SetCallbackBytes(1);
                        ftpclient->SetCallbackXferFunction(DownloadFtpCallback);
                    }

                    CONFIG::LockDownloadList();
                    active_dl->state = STATE_RESUMED;
                    CONFIG::UnlockDownloadList();

                    snprintf(temp_file, sizeof(temp_file), "%s.tmp", active_dl->dest_path.c_str());
                    // Check if temp file still exists, if exists then resume download
                    Util::RichNotify(active_dl->id, "Resuming download %s", active_dl->dest_path.c_str());
                    if (FS::FileExists(temp_file))
                    {
                        tmp_file_size = FS::GetSize(temp_file);
                        g_dl_offset = tmp_file_size;
                        ret = tmp_client->Get(temp_file, active_dl->src_path, tmp_file_size);
                    }
                    else
                    {
                        g_dl_offset = 0;
                        ret = tmp_client->Get(temp_file, active_dl->src_path);
                    }

                    CONFIG::LockDownloadList();
                    if (ret == 0 || !CompleteDownloadedFile(temp_file, active_dl->dest_path, active_dl->file_size))
                    {
                        active_dl->state = STATE_FAILED;
                        active_dl->fail_reason = "Failed to download or write file";
                        Util::RichNotify(active_dl->id, "Failed to download %s", active_dl->dest_path.c_str());
                    }
                    else
                    {
                        Util::RichNotify(active_dl->id, "Completed download %s", active_dl->dest_path.c_str());
                        active_dl->state = STATE_SUCCESS;
                    }
                    CONFIG::UnlockDownloadList();
                    CONFIG::SaveBgDownloadData();

                    DeleteRemoteClient(tmp_client);
                }
            }

            sleep(1);
        }

        return nullptr;
    }

    void *ServerThread(void *argp)
    {
        svr->Get("/", [&](const Request &req, Response &res)
                 { res.set_redirect("/index.html"); });

        svr->Post("/store_bg_install_data", [&](const Request &req, Response &res)
        {
            const char *hash_param;
            const char *path_param;
            const char *url_param;
            const char *username_param;
            const char *password_param;
            const char *http_server_type_param;
            int type_param;
            uint64_t size_param;

            json_object *jobj = json_tokener_parse(req.body.c_str());
            if (jobj != nullptr)
            {
                hash_param = json_object_get_string(json_object_object_get(jobj, "hash"));
                url_param = json_object_get_string(json_object_object_get(jobj, "url"));
                path_param = json_object_get_string(json_object_object_get(jobj, "path"));
                username_param  = json_object_get_string(json_object_object_get(jobj, "username"));
                password_param = json_object_get_string(json_object_object_get(jobj, "password"));
                http_server_type_param = json_object_get_string(json_object_object_get(jobj, "http_server_type"));
                type_param = json_object_get_int(json_object_object_get(jobj, "type"));
                json_object *size_obj = json_object_object_get(jobj, "size");
                size_param = size_obj != nullptr ? json_object_get_uint64(size_obj) : 0;

                const char *direct_url_param = nullptr;
                json_object *direct_url_obj = json_object_object_get(jobj, "direct_url");
                if (direct_url_obj != nullptr) {
                    direct_url_param = json_object_get_string(direct_url_obj);
                }

                if (url_param == nullptr || hash_param == nullptr)
                {
                    bad_request(res, "Required url_param or hash parameter missing");
                    json_object_put(jobj);
                    return;
                }

                PackageInstallData pkg_data;
                pkg_data.host_info.url = url_param;
                if (username_param != nullptr)
                    pkg_data.host_info.username = username_param;
                if (password_param != nullptr)
                    pkg_data.host_info.password = password_param;
                if (path_param != nullptr)
                    pkg_data.path = path_param;
                if (http_server_type_param != nullptr)
                    pkg_data.host_info.http_server_type = http_server_type_param;
                if (direct_url_param != nullptr)
                    pkg_data.direct_url = direct_url_param;
                pkg_data.file_size = size_param;
                pkg_data.timestamp = Util::GetTick();
                pkg_data.host_info.type = type_param;
                pkg_data.host_info.client = nullptr;

                CONFIG::AddPackageInstallHostData(hash_param, pkg_data);
                CONFIG::SavePackageInstallHostData();
                success(res);
                json_object_put(jobj);
            }
            else
            {
                bad_request(res, "Invalid payload");
            }
        });

        svr->Get("/bg_redirect/(.*)", [&](const Request &req, Response &res)
        {
            std::string hash = req.matches[1];
            PackageInstallData* pkg_host_data = CONFIG::GetPackageInstallHostData(hash);
            
            if (pkg_host_data != nullptr && !pkg_host_data->direct_url.empty()) {
                res.set_redirect(pkg_host_data->direct_url.c_str());
            } else {
                failed(res, 404, "Redirect not found");
            }
        });

        svr->Get("/bg_install/(.*)", [&](const Request &req, Response &res)
        {
            std::string hash = req.matches[1];
            PackageInstallData* pkg_host_data = CONFIG::GetPackageInstallHostData(hash);

            if (pkg_host_data == nullptr)
            {
                ClearRequestRanges(req);
                failed(res, 500, "Cannot resume background install of " + hash + ". Host data not found.");
                return;
            }

            RemoteClient *tmp_client = GetRemoteClient(&(pkg_host_data->host_info));
            if (tmp_client == nullptr)
            {
                ClearRequestRanges(req);
                failed(res, 500, "Cannot connect to background install host");
                return;
            }

            std::string path = pkg_host_data->path;
            uint64_t file_size = pkg_host_data->file_size;
            uint64_t range_start = 0;
            uint64_t range_end = 0;

            if (req.ranges.empty())
            {
                if (file_size == 0)
                {
                    DeleteRemoteClient(tmp_client);
                    range_not_satisfiable(res, file_size, "Range header required for background install");
                    return;
                }

                res.set_header("Accept-Ranges", "bytes");
                res.set_content_provider(
                    (size_t)file_size, "application/octet-stream",
                    [tmp_client, path](size_t offset, size_t length, DataSink &sink) {
                        int ret = tmp_client->GetRange(path, sink, length, offset);
                        return (ret == 1);
                    },
                    [tmp_client](bool success) {
                        DeleteRemoteClient(tmp_client);
                    });
                return;
            }

            if (!GetRequestedRange(req, file_size, range_start, range_end))
            {
                DeleteRemoteClient(tmp_client);
                ClearRequestRanges(req);
                range_not_satisfiable(res, file_size, "Invalid Range header for background install");
                return;
            }

            uint64_t range_len = (range_end - range_start) + 1;
            ClearRequestRanges(req);
            res.status = 206;
            res.set_header("Accept-Ranges", "bytes");
            std::string content_range = "bytes " + std::to_string(range_start) + "-" + std::to_string(range_end) + "/";
            content_range += file_size > 0 ? std::to_string(file_size) : "*";
            res.set_header("Content-Range", content_range);

            res.set_content_provider(
                (size_t)range_len, "application/octet-stream",
                [tmp_client, path, range_start](size_t offset, size_t length, DataSink &sink) {
                    int ret = tmp_client->GetRange(path, sink, length, range_start + offset);
                    return (ret == 1);
                },
                [tmp_client](bool success) {
                    DeleteRemoteClient(tmp_client);
                });

        });

        svr->Post("/install", [&](const Request &req, Response &res)
        {
            json_object *jobj = json_tokener_parse(req.body.c_str());
            if (jobj != nullptr)
            {
                const char* url_param = json_object_get_string(json_object_object_get(jobj, "url"));
                const char* title_param = json_object_get_string(json_object_object_get(jobj, "title"));
                const char* icon_url_param = json_object_get_string(json_object_object_get(jobj, "icon_url"));
                const char* content_id_param = json_object_get_string(json_object_object_get(jobj, "content_id"));

                if (url_param == nullptr)
                {
                    bad_request(res, "Missing url parameter");
                    json_object_put(jobj);
                    return;
                }

                int ret = DpiUseCase::InstallPackage(
                    url_param,
                    title_param ? title_param : "",
                    icon_url_param ? icon_url_param : "",
                    content_id_param ? content_id_param : ""
                );

                if (ret == 0)
                {
                    success(res);
                }
                else
                {
                    char error_msg[128];
                    snprintf(error_msg, sizeof(error_msg), "Install failed with Error Code: 0x%08X", ret);
                    failed(res, 500, error_msg);
                }
                json_object_put(jobj);
            }
            else
            {
                bad_request(res, "Invalid payload");
            }
        });

        svr->Post("/download_url", [&](const Request &req, Response &res)
        {
            int type_param;
            const char *url_param;
            const char *username_param;
            const char *password_param;
            const char *http_server_type_param;
            const char *src_path_param;
            const char *dest_path_param;
            uint64_t file_size_param;
            uint64_t id_param;

            json_object *jobj = json_tokener_parse(req.body.c_str());
            if (jobj != nullptr)
            {
                type_param = json_object_get_int(json_object_object_get(jobj, "type"));
                url_param = json_object_get_string(json_object_object_get(jobj, "url"));
                username_param  = json_object_get_string(json_object_object_get(jobj, "username"));
                password_param = json_object_get_string(json_object_object_get(jobj, "password"));
                http_server_type_param = json_object_get_string(json_object_object_get(jobj, "http_server_type"));
                src_path_param = json_object_get_string(json_object_object_get(jobj, "src_path"));
                dest_path_param = json_object_get_string(json_object_object_get(jobj, "dest_path"));
                json_object *file_size_obj = json_object_object_get(jobj, "size");
                json_object *id_obj = json_object_object_get(jobj, "id");
                file_size_param = file_size_obj != nullptr ? json_object_get_uint64(file_size_obj) : 0;
                id_param = id_obj != nullptr ? json_object_get_uint64(id_obj) : Util::GetTick();

                if (url_param == nullptr || src_path_param == nullptr || dest_path_param == nullptr)
                {
                    bad_request(res, "Required parameters are missing");
                    json_object_put(jobj);
                    return;
                }

                BgDownloadData download_data;
                download_data.host_info.type = type_param;
                download_data.host_info.url = url_param;
                download_data.host_info.client = nullptr;
                download_data.src_path = src_path_param;
                download_data.dest_path = dest_path_param;
                download_data.file_size = file_size_param;
                download_data.state = STATE_PENDING;
                download_data.id = id_param;
                download_data.bytes_transfered = 0;
                download_data.timestamp = Util::GetTick();

                if (username_param != nullptr)
                    download_data.host_info.username = username_param;
                if (password_param != nullptr)
                    download_data.host_info.password = password_param;
                if (http_server_type_param != nullptr)
                    download_data.host_info.http_server_type = http_server_type_param;

                CONFIG::AddBgDownloadData(download_data);
                CONFIG::SaveBgDownloadData();
                success(res);
                json_object_put(jobj);
                return;
            }

            bad_request(res, "Invalid payload");
        });

        svr->Get("/get_download_state", [&](const Request &req, Response &res)
        {
            json_object *download_list = json_object_new_array();

            CONFIG::LockDownloadList();
            for (auto it = bg_download_list.begin(); it != bg_download_list.end(); ++it)
            {
                json_object *download_item_obj = json_object_new_object();
                json_object_object_add(download_item_obj, "path", json_object_new_string(it->dest_path.c_str()));
                json_object_object_add(download_item_obj, "bytes_transfered", json_object_new_uint64(it->bytes_transfered));
                json_object_object_add(download_item_obj, "file_size", json_object_new_uint64(it->file_size));
                json_object_object_add(download_item_obj, "state", json_object_new_int(it->state));
                if (it->state == STATE_FAILED)
                    json_object_object_add(download_item_obj, "fail_reason", json_object_new_string(it->fail_reason.c_str()));
                json_object_object_add(download_item_obj, "timestamp", json_object_new_uint64(it->timestamp/1000000));
                json_object_array_add(download_list, download_item_obj);
            }
            CONFIG::UnlockDownloadList();
            
            const char *payload_str = json_object_to_json_string(download_list);

            res.status = 200;
            res.set_content(payload_str, "application/json");
            json_object_put(download_list);
        });

        svr->Get("/stop", [&](const Request & /*req*/, Response & /*res*/)
        {
            svr->stop();
        });

        svr->Get("/version", [&](const Request & req, Response &res)
        {
            res.status = 200;
            char version[20];
            sprintf(version, "%.2f", EZREMOTE_VERSION);
            res.set_content(version, "text/html");
        });

        svr->set_error_handler([](const Request & /*req*/, Response &res)
        {
            const char *fmt = "<p>Error Status: <span style='color:red;'>%d</span></p>";
            char buf[BUFSIZ];
            snprintf(buf, sizeof(buf), fmt, res.status);
            res.set_content(buf, "text/html");
        });

        svr->set_logger([](const Request &req, const Response &res)
        {
            dbglogger_log("[ezremote-server] [%s] %s -> %d", req.method.c_str(), req.path.c_str(), res.status);
            if (res.status >= 400 && !res.body.empty()) {
                dbglogger_log("[ezremote-server] Error body: %s", res.body.c_str());
            }
        });
       
        svr->set_payload_max_length(1024 * 1024 * 12);
        svr->set_tcp_nodelay(true);
        FS::MkDirs("/data/homebrew/ezremote-client/game-icons");
        svr->set_mount_point("/game-icons", "/data/homebrew/ezremote-client/game-icons");
        svr->set_mount_point("/", "/data/homebrew/ezremote-client/assets/");

        svr->listen("0.0.0.0", http_server_port);

        return NULL;
    }

    void Start()
    {
        if (svr == nullptr)
            svr = new Server();
        if (!svr->is_valid())
        {
            return;
        }

        Util::Notify("Starting ezRemote Server %.2f on port %d", EZREMOTE_VERSION, http_server_port);
        ServerThread(nullptr);
        StopDownloadThread();
    }

    void Stop()
    {
        if (svr != nullptr)
            svr->stop();
        StopDownloadThread();
    }

    void StartDownloadThread()
    {
        if (bg_download_thread_started)
            return;

        bg_download_running.store(true);
        if (pthread_create(&bg_download_thread, NULL, DownloadFilesThread, NULL) == 0)
        {
            bg_download_thread_started = true;
        }
        else
        {
            bg_download_running.store(false);
        }
    }

    void StopDownloadThread()
    {
        if (!bg_download_thread_started)
            return;

        bg_download_running.store(false);
        pthread_join(bg_download_thread, NULL);
        bg_download_thread_started = false;
    }
}
