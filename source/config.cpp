// #include <orbis/UserService.h>
// #include <orbis/Net.h>
#include <string>
#include <cstring>
#include <map>
#include <vector>
#include <list>
#include <regex>
#include <shared_mutex>
#include <stdlib.h>
#include <json-c/json.h>
#include "server/http_server.h"
#include "config.h"
#include "ps5_api/fs.h"
#include "crypt.h"
#include "base64.h"
#include "util.h"

static std::map<std::string, PackageInstallData> pkg_download_history;
std::list<BgDownloadData> bg_download_list;
std::list<BgExtractData> bg_extract_list;
std::list<BgFileOpData> bg_fileop_list;

unsigned char cipher_key[32] = {'s', '5', 'v', '8', 'y', '/', 'B', '?', 'E', '(', 'H', '+', 'M', 'b', 'Q', 'e', 'T', 'h', 'W', 'm', 'Z', 'q', '4', 't', '7', 'w', '9', 'z', '$', 'C', '&', 'F'};
unsigned char cipher_iv[16] = {'Y', 'p', '3', 's', '6', 'v', '9', 'y', '$', 'B', '&', 'E', ')', 'H', '@', 'M'};

std::shared_mutex pkg_mutex_;
std::shared_mutex download_mutex_;
std::shared_mutex extract_mutex_;
std::shared_mutex fileop_mutex_;
uint64_t *g_bytes_transfered;

static bool IsExpired(uint64_t timestamp, uint64_t now)
{
    if (now < timestamp)
        return false;

    return (now - timestamp) >= MAX_PKG_HISTORY_RETENTION;
}

static void PrunePackageHistoryLocked()
{
    uint64_t now = Util::GetTick();
    for (auto it = pkg_download_history.begin(); it != pkg_download_history.end();)
    {
        if (IsExpired(it->second.timestamp, now))
            it = pkg_download_history.erase(it);
        else
            ++it;
    }
}

static void PruneBgDownloadListLocked()
{
    uint64_t now = Util::GetTick();
    for (auto it = bg_download_list.begin(); it != bg_download_list.end();)
    {
        bool terminal_state = (it->state == STATE_FAILED || it->state == STATE_SUCCESS);
        if (terminal_state && IsExpired(it->timestamp, now))
            it = bg_download_list.erase(it);
        else
            ++it;
    }
}

static void PruneBgExtractListLocked()
{
    uint64_t now = Util::GetTick();
    for (auto it = bg_extract_list.begin(); it != bg_extract_list.end();)
    {
        bool terminal_state = (it->state == EXTRACT_STATE_FAILED || it->state == EXTRACT_STATE_SUCCESS);
        if (terminal_state && IsExpired(it->timestamp, now))
            it = bg_extract_list.erase(it);
        else
            ++it;
    }
}

namespace CONFIG
{
    int Encrypt(const std::string &text, std::string &encrypt_text)
    {
        unsigned char tmp_encrypt_text[text.length() * 2];
        int encrypt_text_len;
        memset(tmp_encrypt_text, 0, sizeof(tmp_encrypt_text));
        int ret = openssl_encrypt((unsigned char *)text.c_str(), text.length(), cipher_key, cipher_iv, tmp_encrypt_text, &encrypt_text_len);
        if (ret == 0)
            return 0;
        return Base64::Encode(std::string((const char *)tmp_encrypt_text, encrypt_text_len), encrypt_text);
    }

    int Decrypt(const std::string &text, std::string &decrypt_text)
    {
        std::string tmp_decode_text;
        int ret = Base64::Decode(text, tmp_decode_text);
        if (ret == 0)
            return 0;

        unsigned char tmp_decrypt_text[tmp_decode_text.length() * 2];
        int decrypt_text_len;
        memset(tmp_decrypt_text, 0, sizeof(tmp_decrypt_text));
        ret = openssl_decrypt((unsigned char *)tmp_decode_text.c_str(), tmp_decode_text.length(), cipher_key, cipher_iv, tmp_decrypt_text, &decrypt_text_len);
        if (ret == 0)
            return 0;

        decrypt_text.clear();
        decrypt_text.append(std::string((const char *)tmp_decrypt_text, decrypt_text_len));

        return 1;
    }

	PackageInstallData* GetPackageInstallHostData(const std::string &hash)
	{
        if (pkg_download_history.find(hash) != pkg_download_history.end())
		    return &pkg_download_history[hash];
        return nullptr;
	}

	void AddPackageInstallHostData(const std::string &hash, PackageInstallData pkg_data)
	{
		std::unique_lock<std::shared_mutex> lock(pkg_mutex_);
		PrunePackageHistoryLocked();
		std::pair<std::string, PackageInstallData> pair = std::make_pair(hash, pkg_data);
		pkg_download_history.erase(hash);
		pkg_download_history.insert(pair);
	}

	void RemovePackageInstallHostData(const std::string &hash)
	{
		std::unique_lock<std::shared_mutex> lock(pkg_mutex_);
		pkg_download_history.erase(hash);
	}

    void LoadPackageInstallHostData()
    {
        if (FS::FileExists(PKG_INSTALL_HISTORY_PATH))
        {
            json_object *jobj = json_object_from_file(PKG_INSTALL_HISTORY_PATH);
            if (jobj == nullptr)
                return;

            struct array_list *history_list = json_object_get_array(jobj);
            if (history_list == nullptr)
            {
                json_object_put(jobj);
                return;
            }

            for (size_t history_idx = 0; history_idx < history_list->length; ++history_idx)
            {
                PackageInstallData history_item;

                json_object *history_item_obj = (json_object *)array_list_get_idx(history_list, history_idx);
                std::string hash = std::string(json_object_get_string(json_object_object_get(history_item_obj, "hash")));
                history_item.host_info.url = std::string(json_object_get_string(json_object_object_get(history_item_obj, "url")));
                history_item.path = std::string(json_object_get_string(json_object_object_get(history_item_obj, "path")));
                history_item.host_info.username = std::string(json_object_get_string(json_object_object_get(history_item_obj, "username")));
                std::string encrypted_password = std::string(json_object_get_string(json_object_object_get(history_item_obj, "password")));
                history_item.host_info.type = json_object_get_int(json_object_object_get(history_item_obj, "type"));
                json_object *size_obj = json_object_object_get(history_item_obj, "size");
                history_item.file_size = size_obj != nullptr ? json_object_get_uint64(size_obj) : 0;
                history_item.timestamp = json_object_get_uint64(json_object_object_get(history_item_obj, "timestamp"));
                json_object *direct_url_obj = json_object_object_get(history_item_obj, "direct_url");
                history_item.direct_url = direct_url_obj != nullptr ? std::string(json_object_get_string(direct_url_obj)) : "";
                history_item.host_info.client = nullptr;

                if (history_item.host_info.type == CLIENT_TYPE_HTTP_SERVER)
		        {
                    history_item.host_info.http_server_type = std::string(json_object_get_string(json_object_object_get(history_item_obj, "http_server_type")));
                }

                int ret = Decrypt(encrypted_password, history_item.host_info.password);
                if (ret == 0)
                {
                    history_item.host_info.password = encrypted_password;
                }
                AddPackageInstallHostData(hash, history_item);
            }
            json_object_put(jobj);
        }
    }

    void SavePackageInstallHostData()
    {
        std::unique_lock<std::shared_mutex> lock(pkg_mutex_);
        PrunePackageHistoryLocked();

        if (!FS::FolderExists(DATA_PATH))
        {
            FS::MkDirs(DATA_PATH);
        }

        json_object *history_list = json_object_new_array();

        for (auto it = pkg_download_history.begin(); it != pkg_download_history.end(); ++it)
        {
            json_object *history_item_obj = json_object_new_object();
            json_object_object_add(history_item_obj, "hash", json_object_new_string(it->first.c_str()));
            json_object_object_add(history_item_obj, "url", json_object_new_string(it->second.host_info.url.c_str()));
            json_object_object_add(history_item_obj, "path", json_object_new_string(it->second.path.c_str()));
            json_object_object_add(history_item_obj, "username", json_object_new_string(it->second.host_info.username.c_str()));
            json_object_object_add(history_item_obj, "type", json_object_new_int(it->second.host_info.type));
            json_object_object_add(history_item_obj, "size", json_object_new_uint64(it->second.file_size));
            json_object_object_add(history_item_obj, "timestamp", json_object_new_uint64(it->second.timestamp));
            if (!it->second.direct_url.empty()) {
                json_object_object_add(history_item_obj, "direct_url", json_object_new_string(it->second.direct_url.c_str()));
            }
            if (it->second.host_info.type == CLIENT_TYPE_HTTP_SERVER)
            {
                json_object_object_add(history_item_obj, "http_server_type", json_object_new_string(it->second.host_info.http_server_type.c_str()));
            }

            std::string encrypted_password;
            if (!it->second.host_info.password.empty())
            {
                Encrypt(it->second.host_info.password, encrypted_password);
            }
            json_object_object_add(history_item_obj, "password", json_object_new_string(encrypted_password.c_str()));

            json_object_array_add(history_list, history_item_obj);
        }
        
        json_object_to_file(PKG_INSTALL_HISTORY_PATH, history_list);
        json_object_put(history_list);
    }

	void AddBgDownloadData(BgDownloadData pkg_data)
	{
		std::unique_lock<std::shared_mutex> lock(download_mutex_);
		PruneBgDownloadListLocked();
		bg_download_list.push_back(pkg_data);
	}

    void LoadBgDownloadData()
    {
        if (FS::FileExists(BG_DOWNLOAD_HISTORY_PATH))
        {
            json_object *jobj = json_object_from_file(BG_DOWNLOAD_HISTORY_PATH);
            if (jobj == nullptr)
                return;

            struct array_list *history_list = json_object_get_array(jobj);
            if (history_list == nullptr)
            {
                json_object_put(jobj);
                return;
            }

            for (size_t history_idx = 0; history_idx < history_list->length; ++history_idx)
            {
                BgDownloadData history_item;

                json_object *history_item_obj = (json_object *)array_list_get_idx(history_list, history_idx);
                history_item.host_info.type = json_object_get_int(json_object_object_get(history_item_obj, "type"));
                history_item.host_info.url = std::string(json_object_get_string(json_object_object_get(history_item_obj, "url")));
                history_item.host_info.username = std::string(json_object_get_string(json_object_object_get(history_item_obj, "username")));
                std::string encrypted_password = std::string(json_object_get_string(json_object_object_get(history_item_obj, "password")));

                if (history_item.host_info.type == CLIENT_TYPE_HTTP_SERVER)
		        {
                    history_item.host_info.http_server_type = std::string(json_object_get_string(json_object_object_get(history_item_obj, "http_server_type")));
                }

                int ret = Decrypt(encrypted_password, history_item.host_info.password);
                if (ret == 0)
                {
                    history_item.host_info.password = encrypted_password;
                }

                history_item.src_path = std::string(json_object_get_string(json_object_object_get(history_item_obj, "src_path")));
                history_item.dest_path = std::string(json_object_get_string(json_object_object_get(history_item_obj, "dest_path")));
                history_item.file_size = json_object_get_uint64(json_object_object_get(history_item_obj, "file_size"));
                history_item.bytes_transfered = json_object_get_uint64(json_object_object_get(history_item_obj, "bytes_transfered"));
                
                json_object *is_dir_obj = json_object_object_get(history_item_obj, "is_dir");
                history_item.is_dir = is_dir_obj != nullptr ? json_object_get_boolean(is_dir_obj) : false;
                
                json_object *completed_bytes_obj = json_object_object_get(history_item_obj, "completed_bytes");
                history_item.completed_bytes = completed_bytes_obj != nullptr ? json_object_get_uint64(completed_bytes_obj) : 0;
                
                history_item.state = static_cast<DownloadState>(json_object_get_int(json_object_object_get(history_item_obj, "state")));
                history_item.retry_count = json_object_get_int(json_object_object_get(history_item_obj, "retry_count"));
                
                if (history_item.state == STATE_DOWNLOADING) {
                    if (history_item.retry_count < 10) {
                        history_item.state = STATE_RESUMED;
                        history_item.retry_count++;
                    } else {
                        history_item.state = STATE_FAILED;
                        history_item.fail_reason = "Max retries exceeded";
                    }
                }
                json_object *fail_reason_obj = json_object_object_get(history_item_obj, "fail_reason");
                if (fail_reason_obj != nullptr)
                {
                    history_item.fail_reason = std::string(json_object_get_string(fail_reason_obj));
                }

                history_item.id = json_object_get_uint64(json_object_object_get(history_item_obj, "id"));
                history_item.timestamp = json_object_get_uint64(json_object_object_get(history_item_obj, "timestamp"));
                json_object *finished_obj = json_object_object_get(history_item_obj, "finished_timestamp");
                history_item.finished_timestamp = finished_obj != nullptr ? json_object_get_uint64(finished_obj) : 0;
                if ((history_item.state == STATE_FAILED || history_item.state == STATE_SUCCESS) && history_item.finished_timestamp == 0)
                {
                    history_item.finished_timestamp = history_item.timestamp;
                }

                AddBgDownloadData(history_item);
            }
            json_object_put(jobj);
        }
    }

    void SaveBgDownloadData()
    {
        std::unique_lock<std::shared_mutex> lock(download_mutex_);
        PruneBgDownloadListLocked();

        if (!FS::FolderExists(DATA_PATH))
        {
            FS::MkDirs(DATA_PATH);
        }

        json_object *history_list = json_object_new_array();

        for (auto it = bg_download_list.begin(); it != bg_download_list.end(); ++it)
        {
            json_object *history_item_obj = json_object_new_object();
            json_object_object_add(history_item_obj, "type", json_object_new_int(it->host_info.type));
            json_object_object_add(history_item_obj, "url", json_object_new_string(it->host_info.url.c_str()));
            if (it->host_info.type == CLIENT_TYPE_HTTP_SERVER)
            {
                json_object_object_add(history_item_obj, "http_server_type", json_object_new_string(it->host_info.http_server_type.c_str()));
            }

            std::string encrypted_password;
            if (!it->host_info.password.empty())
            {
                Encrypt(it->host_info.password, encrypted_password);
            }

            json_object_object_add(history_item_obj, "username", json_object_new_string(it->host_info.username.c_str()));
            json_object_object_add(history_item_obj, "password", json_object_new_string(encrypted_password.c_str()));
            json_object_object_add(history_item_obj, "src_path", json_object_new_string(it->src_path.c_str()));
            json_object_object_add(history_item_obj, "dest_path", json_object_new_string(it->dest_path.c_str()));
            json_object_object_add(history_item_obj, "file_size", json_object_new_uint64(it->file_size));
            json_object_object_add(history_item_obj, "bytes_transfered", json_object_new_uint64(it->bytes_transfered));
            json_object_object_add(history_item_obj, "is_dir", json_object_new_boolean(it->is_dir));
            json_object_object_add(history_item_obj, "completed_bytes", json_object_new_uint64(it->completed_bytes));
            json_object_object_add(history_item_obj, "state", json_object_new_int(it->state));
            if (it->state == STATE_FAILED && !it->fail_reason.empty())
            {
                json_object_object_add(history_item_obj, "fail_reason", json_object_new_string(it->fail_reason.c_str()));
            }
            json_object_object_add(history_item_obj, "id", json_object_new_uint64(it->id));
            json_object_object_add(history_item_obj, "timestamp", json_object_new_uint64(it->timestamp));
            json_object_object_add(history_item_obj, "finished_timestamp", json_object_new_uint64(it->finished_timestamp));
            json_object_object_add(history_item_obj, "retry_count", json_object_new_int(it->retry_count));

            json_object_array_add(history_list, history_item_obj);
        }
        
        json_object_to_file(BG_DOWNLOAD_HISTORY_PATH, history_list);
        json_object_put(history_list);
    }

    void LockDownloadList()
    {
        download_mutex_.lock();
    }

    void UnlockDownloadList()
    {
        download_mutex_.unlock();
    }

	void AddBgExtractData(BgExtractData pkg_data)
	{
		std::unique_lock<std::shared_mutex> lock(extract_mutex_);
		PruneBgExtractListLocked();
		bg_extract_list.push_back(pkg_data);
	}

    void LoadBgExtractData()
    {
        if (FS::FileExists(BG_EXTRACT_HISTORY_PATH))
        {
            json_object *jobj = json_object_from_file(BG_EXTRACT_HISTORY_PATH);
            if (jobj == nullptr)
                return;

            struct array_list *history_list = json_object_get_array(jobj);
            if (history_list == nullptr)
            {
                json_object_put(jobj);
                return;
            }

            for (size_t history_idx = 0; history_idx < history_list->length; ++history_idx)
            {
                BgExtractData history_item;

                json_object *history_item_obj = (json_object *)array_list_get_idx(history_list, history_idx);
                history_item.host_info.type = json_object_get_int(json_object_object_get(history_item_obj, "type"));
                history_item.host_info.url = std::string(json_object_get_string(json_object_object_get(history_item_obj, "url")));
                history_item.host_info.username = std::string(json_object_get_string(json_object_object_get(history_item_obj, "username")));
                std::string encrypted_password = std::string(json_object_get_string(json_object_object_get(history_item_obj, "password")));

                if (history_item.host_info.type == CLIENT_TYPE_HTTP_SERVER)
		        {
                    history_item.host_info.http_server_type = std::string(json_object_get_string(json_object_object_get(history_item_obj, "http_server_type")));
                }

                int ret = Decrypt(encrypted_password, history_item.host_info.password);
                if (ret == 0)
                {
                    history_item.host_info.password = encrypted_password;
                }

                history_item.src_path = std::string(json_object_get_string(json_object_object_get(history_item_obj, "src_path")));
                history_item.dest_path = std::string(json_object_get_string(json_object_object_get(history_item_obj, "dest_path")));
                history_item.folder_name = std::string(json_object_get_string(json_object_object_get(history_item_obj, "folder_name")));
                history_item.file_size = json_object_get_uint64(json_object_object_get(history_item_obj, "file_size"));
                history_item.bytes_transfered = json_object_get_uint64(json_object_object_get(history_item_obj, "bytes_transfered"));
                history_item.state = static_cast<ExtractState>(json_object_get_int(json_object_object_get(history_item_obj, "state")));
                
                history_item.retry_count = json_object_get_int(json_object_object_get(history_item_obj, "retry_count"));
                
                if (history_item.state == EXTRACT_STATE_EXTRACTING) {
                    if (history_item.retry_count < 10) {
                        history_item.state = EXTRACT_STATE_PENDING;
                        history_item.bytes_transfered = 0;
                        history_item.retry_count++;
                        std::string tmp_folder = history_item.dest_path + "/.extract_" + std::to_string(history_item.id);
                        if (FS::FolderExists(tmp_folder)) {
                            FS::RmRecursive(tmp_folder);
                        }
                    } else {
                        history_item.state = EXTRACT_STATE_FAILED;
                        history_item.fail_reason = "Max retries exceeded";
                    }
                }
                json_object *fail_reason_obj = json_object_object_get(history_item_obj, "fail_reason");
                if (fail_reason_obj != nullptr)
                {
                    history_item.fail_reason = std::string(json_object_get_string(fail_reason_obj));
                }

                history_item.id = json_object_get_uint64(json_object_object_get(history_item_obj, "id"));
                history_item.timestamp = json_object_get_uint64(json_object_object_get(history_item_obj, "timestamp"));
                json_object *finished_obj = json_object_object_get(history_item_obj, "finished_timestamp");
                history_item.finished_timestamp = finished_obj != nullptr ? json_object_get_uint64(finished_obj) : 0;
                if ((history_item.state == EXTRACT_STATE_FAILED || history_item.state == EXTRACT_STATE_SUCCESS) && history_item.finished_timestamp == 0)
                {
                    history_item.finished_timestamp = history_item.timestamp;
                }

                AddBgExtractData(history_item);
            }
            json_object_put(jobj);
        }
    }

    void SaveBgExtractData()
    {
        std::unique_lock<std::shared_mutex> lock(extract_mutex_);
        PruneBgExtractListLocked();

        if (!FS::FolderExists(DATA_PATH))
        {
            FS::MkDirs(DATA_PATH);
        }

        json_object *history_list = json_object_new_array();

        for (auto it = bg_extract_list.begin(); it != bg_extract_list.end(); ++it)
        {
            json_object *history_item_obj = json_object_new_object();
            json_object_object_add(history_item_obj, "type", json_object_new_int(it->host_info.type));
            json_object_object_add(history_item_obj, "url", json_object_new_string(it->host_info.url.c_str()));
            if (it->host_info.type == CLIENT_TYPE_HTTP_SERVER)
            {
                json_object_object_add(history_item_obj, "http_server_type", json_object_new_string(it->host_info.http_server_type.c_str()));
            }

            std::string encrypted_password;
            if (!it->host_info.password.empty())
            {
                Encrypt(it->host_info.password, encrypted_password);
            }

            json_object_object_add(history_item_obj, "username", json_object_new_string(it->host_info.username.c_str()));
            json_object_object_add(history_item_obj, "password", json_object_new_string(encrypted_password.c_str()));
            json_object_object_add(history_item_obj, "src_path", json_object_new_string(it->src_path.c_str()));
            json_object_object_add(history_item_obj, "dest_path", json_object_new_string(it->dest_path.c_str()));
            json_object_object_add(history_item_obj, "folder_name", json_object_new_string(it->folder_name.c_str()));
            json_object_object_add(history_item_obj, "file_size", json_object_new_uint64(it->file_size));
            json_object_object_add(history_item_obj, "bytes_transfered", json_object_new_uint64(it->bytes_transfered));
            json_object_object_add(history_item_obj, "state", json_object_new_int(it->state));
            if (it->state == EXTRACT_STATE_FAILED && !it->fail_reason.empty())
            {
                json_object_object_add(history_item_obj, "fail_reason", json_object_new_string(it->fail_reason.c_str()));
            }
            json_object_object_add(history_item_obj, "id", json_object_new_uint64(it->id));
            json_object_object_add(history_item_obj, "timestamp", json_object_new_uint64(it->timestamp));
            json_object_object_add(history_item_obj, "finished_timestamp", json_object_new_uint64(it->finished_timestamp));
            json_object_object_add(history_item_obj, "retry_count", json_object_new_int(it->retry_count));

            json_object_array_add(history_list, history_item_obj);
        }
        
        json_object_to_file(BG_EXTRACT_HISTORY_PATH, history_list);
        json_object_put(history_list);
    }

    void LockExtractList()
    {
        extract_mutex_.lock();
    }

    void UnlockExtractList()
    {
        extract_mutex_.unlock();
    }

    static void PruneBgFileOpListLocked()
    {
        uint64_t current_time = time(NULL);
        bg_fileop_list.remove_if([current_time](const BgFileOpData& item) {
            return (item.state == FILEOP_STATE_SUCCESS || item.state == FILEOP_STATE_FAILED) && 
                   (current_time - item.timestamp > 86400); 
        });
    }

    void AddBgFileOpData(BgFileOpData bg_fileop_data)
    {
        std::unique_lock<std::shared_mutex> lock(fileop_mutex_);
        PruneBgFileOpListLocked();
        bg_fileop_list.push_back(bg_fileop_data);
    }

    void LoadBgFileOpData()
    {
        if (FS::FileExists(BG_FILEOP_HISTORY_PATH))
        {
            json_object *jobj = json_object_from_file(BG_FILEOP_HISTORY_PATH);
            if (jobj == nullptr)
                return;

            struct array_list *history_list = json_object_get_array(jobj);
            if (history_list == nullptr)
            {
                json_object_put(jobj);
                return;
            }

            for (size_t history_idx = 0; history_idx < history_list->length; ++history_idx)
            {
                BgFileOpData history_item;

                json_object *history_item_obj = (json_object *)array_list_get_idx(history_list, history_idx);
                history_item.id = json_object_get_uint64(json_object_object_get(history_item_obj, "id"));
                history_item.type = static_cast<FileOpType>(json_object_get_int(json_object_object_get(history_item_obj, "type")));
                history_item.dest_path = std::string(json_object_get_string(json_object_object_get(history_item_obj, "dest_path")));
                history_item.items_processed = json_object_get_uint64(json_object_object_get(history_item_obj, "items_processed"));
                history_item.total_items = json_object_get_uint64(json_object_object_get(history_item_obj, "total_items"));
                history_item.state = static_cast<FileOpState>(json_object_get_int(json_object_object_get(history_item_obj, "state")));
                history_item.timestamp = json_object_get_uint64(json_object_object_get(history_item_obj, "timestamp"));
                json_object *finished_obj = json_object_object_get(history_item_obj, "finished_timestamp");
                history_item.finished_timestamp = finished_obj != nullptr ? json_object_get_uint64(finished_obj) : 0;
                
                json_object *fail_reason_obj = json_object_object_get(history_item_obj, "fail_reason");
                if (fail_reason_obj != nullptr)
                {
                    history_item.fail_reason = std::string(json_object_get_string(fail_reason_obj));
                }

                json_object *items_arr = json_object_object_get(history_item_obj, "items");
                if (items_arr && json_object_get_type(items_arr) == json_type_array) {
                    struct array_list *arr = json_object_get_array(items_arr);
                    for (size_t i = 0; i < arr->length; i++) {
                        history_item.items.push_back(json_object_get_string((json_object*)array_list_get_idx(arr, i)));
                    }
                }

                // If interrupted, mark as failed because we don't easily resume middle of file copy yet
                if (history_item.state == FILEOP_STATE_PROCESSING) {
                    history_item.state = FILEOP_STATE_FAILED;
                    history_item.fail_reason = "Interrupted by server restart";
                    history_item.finished_timestamp = Util::GetTick();
                }
                if ((history_item.state == FILEOP_STATE_FAILED || history_item.state == FILEOP_STATE_SUCCESS) && history_item.finished_timestamp == 0)
                {
                    history_item.finished_timestamp = history_item.timestamp;
                }

                AddBgFileOpData(history_item);
            }
            json_object_put(jobj);
        }
    }

    void SaveBgFileOpData()
    {
        std::unique_lock<std::shared_mutex> lock(fileop_mutex_);
        PruneBgFileOpListLocked();

        if (!FS::FolderExists(DATA_PATH))
        {
            FS::MkDirs(DATA_PATH);
        }

        json_object *history_list = json_object_new_array();

        for (auto it = bg_fileop_list.begin(); it != bg_fileop_list.end(); ++it)
        {
            json_object *history_item_obj = json_object_new_object();
            json_object_object_add(history_item_obj, "id", json_object_new_uint64(it->id));
            json_object_object_add(history_item_obj, "type", json_object_new_int(it->type));
            json_object_object_add(history_item_obj, "dest_path", json_object_new_string(it->dest_path.c_str()));
            json_object_object_add(history_item_obj, "items_processed", json_object_new_uint64(it->items_processed));
            json_object_object_add(history_item_obj, "total_items", json_object_new_uint64(it->total_items));
            json_object_object_add(history_item_obj, "state", json_object_new_int(it->state));
            json_object_object_add(history_item_obj, "timestamp", json_object_new_uint64(it->timestamp));
            json_object_object_add(history_item_obj, "finished_timestamp", json_object_new_uint64(it->finished_timestamp));
            json_object_object_add(history_item_obj, "retry_count", json_object_new_int(it->retry_count));

            if (it->state == FILEOP_STATE_FAILED && !it->fail_reason.empty())
            {
                json_object_object_add(history_item_obj, "fail_reason", json_object_new_string(it->fail_reason.c_str()));
            }

            json_object *items_arr = json_object_new_array();
            for (const auto& item : it->items) {
                json_object_array_add(items_arr, json_object_new_string(item.c_str()));
            }
            json_object_object_add(history_item_obj, "items", items_arr);

            json_object_array_add(history_list, history_item_obj);
        }
        
        json_object_to_file(BG_FILEOP_HISTORY_PATH, history_list);
        json_object_put(history_list);
    }

    void LockFileOpList()
    {
        fileop_mutex_.lock();
    }

    void UnlockFileOpList()
    {
        fileop_mutex_.unlock();
    }
}
int http_int_server_port = 9090;
