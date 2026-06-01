#include <fstream>
#include <curl/curl.h>
#include <sys/time.h>
#include <stdint.h>
#include "clients/remote_client.h"
#include "clients/baseclient.h"
#include "config.h"
#include "lang.h"
#include "split_file.h"
#include "util.h"
#include "windows.h"
#include "common.h"

using httplib::DataSink;

struct RangeTransferContext {
    CHTTPClient::HttpResponse *response;
    DataSink *sink;
    char *buffer;
    uint64_t offset;
    uint64_t size;
    uint64_t written;
    bool checked;
    bool valid;
};

static bool ExpectedContentRange(const CHTTPClient::HttpResponse &res, uint64_t offset, uint64_t size)
{
    if (size == 0 || UINT64_MAX - offset < size - 1)
        return false;

    auto it = res.mapHeadersLowercase.find("content-range");
    if (it == res.mapHeadersLowercase.end())
        return false;

    std::string expected = "bytes " + std::to_string(offset) + "-" + std::to_string(offset + size - 1) + "/";
    return it->second.compare(0, expected.length(), expected) == 0;
}

static bool ContentRangeTotal(const CHTTPClient::HttpResponse &res, uint64_t offset, uint64_t size, uint64_t *total)
{
    if (!ExpectedContentRange(res, offset, size))
        return false;

    const std::string &content_range = res.mapHeadersLowercase.at("content-range");
    size_t slash_pos = content_range.find('/');
    if (slash_pos == std::string::npos || slash_pos + 1 >= content_range.length())
        return false;

    std::string total_str = content_range.substr(slash_pos + 1);
    if (total_str == "*")
        return false;

    *total = strtoull(total_str.c_str(), nullptr, 10);
    return true;
}

static bool CheckRangeTransfer(RangeTransferContext *out, size_t bytes)
{
    if (!out->checked)
    {
        out->valid = ExpectedContentRange(*out->response, out->offset, out->size);
        out->checked = true;
    }

    if (!out->valid || out->written + bytes > out->size)
        return false;

    return true;
}

BaseClient::BaseClient(){};

BaseClient::~BaseClient()
{
    if (client != nullptr)
        delete client;
};

int BaseClient::SocketOptCallback(void* ptr, int fd, uint32_t socktype)
{
    int size = 1048576;
    setsockopt(fd, SOL_SOCKET, SO_SNDBUF, &size, sizeof(size));
    setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &size, sizeof(size));
    return 0;
}

int BaseClient::NothingCallback(void* ptr, double dTotalToDownload, double dNowDownloaded, double dTotalToUpload, double dNowUploaded)
{
    return 0;
}

int BaseClient::DownloadProgressCallback(void* ptr, double dTotalToDownload, double dNowDownloaded, double dTotalToUpload, double dNowUploaded)
{
    CHTTPClient::ProgressFnStruct *progress_data = (CHTTPClient::ProgressFnStruct*) ptr;
    int64_t *bytes_transfered = (int64_t *) progress_data->pOwner;
	*bytes_transfered = dNowDownloaded;
    return 0;
}

int BaseClient::UploadProgressCallback(void* ptr, double dTotalToDownload, double dNowDownloaded, double dTotalToUpload, double dNowUploaded)
{
    CHTTPClient::ProgressFnStruct *progress_data = (CHTTPClient::ProgressFnStruct*) ptr;
    int64_t *bytes_transfered = (int64_t *) progress_data->pOwner;
    *bytes_transfered = dNowUploaded;
    return 0;
}

size_t BaseClient::WriteToSplitFileCallback(void *buff, size_t size, size_t nmemb, void *data)
{
    if ((size == 0) || (nmemb == 0) || ((size * nmemb) < 1) || (data == nullptr))
        return 0;

    SplitFile *split_file = reinterpret_cast<SplitFile *>(data);
    if (!split_file->IsClosed())
    {
        if (split_file->Write(reinterpret_cast<char *>(buff), size * nmemb) < 0)
            return 0;
    }
    else
    {
        return 0;
    }

    return size * nmemb;
}

size_t BaseClient::WriteDataSinkCallback(void *pCurlData, size_t usBlockCount, size_t usBlockSize, void *pUserData)
{
    const char* buff = reinterpret_cast<char *>(pCurlData);
    RangeTransferContext *out = reinterpret_cast<RangeTransferContext*>(pUserData);
    size_t bytes = usBlockCount * usBlockSize;

    if (!CheckRangeTransfer(out, bytes))
        return 0;

    if (out->sink->write(buff, bytes))
    {
        out->written += bytes;
        return bytes;
    }
    return 0;
}

size_t BaseClient::WriteBufferCallback(void *pCurlData, size_t usBlockCount, size_t usBlockSize, void *pUserData)
{
    const char* input = reinterpret_cast<char *>(pCurlData);
    RangeTransferContext *out = reinterpret_cast<RangeTransferContext*>(pUserData);
    size_t bytes = usBlockCount * usBlockSize;

    if (!CheckRangeTransfer(out, bytes))
        return 0;

    memcpy(out->buffer + out->written, input, bytes);
    out->written += bytes;

    return bytes;
}

int BaseClient::Connect(const std::string &url, const std::string &username, const std::string &password, bool send_ping)
{
    this->host_url = url;
    size_t scheme_pos = url.find("://");
    size_t root_pos = url.find("/", scheme_pos + 3);
    if (root_pos != std::string::npos)
    {
        this->host_url = url.substr(0, root_pos);
        this->base_path = url.substr(root_pos);
    }

    client = new CHTTPClient([](const std::string& log){});
    if (!username.empty())
    {
        client->SetBasicAuth(username, password);
    }
    client->InitSession(true, CHTTPClient::SettingsFlag::NO_FLAGS);
    client->SetCertificateFile(CACERT_FILE);
    client->SetSocketOptFnCallback(SocketOptCallback);
    client->SetBufferSize(1048576L);

    if (!send_ping)
        this->connected = true;
    else if (Ping())
        this->connected = true;

    return 1;
}

int BaseClient::Mkdir(const std::string &path)
{
    sprintf(this->response, "%s", lang_strings[STR_UNSUPPORTED_OPERATION_MSG]);
    return 0;
}

int BaseClient::Rmdir(const std::string &path, bool recursive)
{
    sprintf(this->response, "%s", lang_strings[STR_UNSUPPORTED_OPERATION_MSG]);
    return 0;
}

int BaseClient::Size(const std::string &path, uint64_t *size)
{
    CHTTPClient::HeadersMap headers;
    CHTTPClient::HttpResponse res;

    std::string encoded_url = this->host_url + CHTTPClient::EncodeUrl(GetFullPath(path));
    client->SetProgressFnCallback(nullptr, NothingCallback);
    if (client->Head(encoded_url, headers, res))
    {
        if (HTTP_SUCCESS(res.iCode))
        {
            std::string content_length = res.mapHeadersLowercase["content-length"];
            if (content_length.length() > 0)
                *size = atoll(content_length.c_str());
            return 1;
        }
        else // Server doesn't support HEAD request. Try get range with 0 bytes and grab size from the response header 
             // example: Content-Range: bytes 0-10/4372785
        {
            CHTTPClient::HttpResponse range_res;
            CHTTPClient::HeadersMap range_headers;
            range_headers["Range"] = "bytes=0-1";

            if (client->Get(encoded_url, range_headers, range_res))
            {
                if (HTTP_SUCCESS(range_res.iCode))
                {
                    uint64_t total_size = 0;
                    if (range_res.iCode == 206 && ContentRangeTotal(range_res, 0, 2, &total_size))
                    {
                        *size = total_size;
                        return 1;
                    }
                }
            }
        }
    }
    else
    {
        sprintf(this->response, "%s", res.errMessage.c_str());
    }
    return 0;
}

int BaseClient::Get(const std::string &outputfile, const std::string &path, uint64_t offset)
{
    long status = 0;
    bytes_transfered = 0;
    prev_tick = Util::GetTick();
    CHTTPClient::HeadersMap headers;

    if (!Size(path, &bytes_to_download))
    {
        sprintf(this->response, "%s", lang_strings[STR_FAIL_DOWNLOAD_MSG]);
        return 0;
    }

    client->SetProgressFnCallback(&bytes_transfered, DownloadProgressCallback);
    std::string encoded_url = this->host_url + CHTTPClient::EncodeUrl(GetFullPath(path));
    if (client->DownloadFile(outputfile, encoded_url, status))
    {
        if (HTTP_SUCCESS(status))
            return 1;

        sprintf(this->response, "%ld - %s", status, lang_strings[STR_FAIL_DOWNLOAD_MSG]);
        return 0;
    }
    else
    {
        sprintf(this->response, "%ld - %s", status, lang_strings[STR_FAIL_DOWNLOAD_MSG]);
    }
    return 0;
}

int BaseClient::Get(SplitFile *split_file, const std::string &path, uint64_t offset)
{
    long status = 0;
    CHTTPClient::HeadersMap headers;

    prev_tick = Util::GetTick();
    std::string encoded_url = this->host_url + CHTTPClient::EncodeUrl(GetFullPath(path));
    client->SetProgressFnCallback(nullptr, NothingCallback);
    if (client->DownloadFile((void*)split_file, encoded_url, (void*)WriteToSplitFileCallback, status))
    {
        return 1;
    }
    else
    {
        sprintf(this->response, "%ld - %s", status, lang_strings[STR_FAIL_DOWNLOAD_MSG]);
    }
    return 0;
}

int BaseClient::GetRange(const std::string &path, DataSink &sink, uint64_t size, uint64_t offset)
{
    CHTTPClient::HttpResponse res;
    CHTTPClient::HeadersMap headers;

    if (size == 0)
        return 0;

    char range_header[128];
    sprintf(range_header, "bytes=%lu-%lu", offset, offset + size - 1);
    headers["Range"] = range_header;

    std::string encoded_url = this->host_url + CHTTPClient::EncodeUrl(GetFullPath(path));
    RangeTransferContext out = {&res, &sink, nullptr, offset, size, 0, false, false};
    if (client->Get(encoded_url, headers, res, (void*) &WriteDataSinkCallback, (void*)&out))
    {
        if (res.iCode == 206 && out.valid && out.written == size)
            return 1;
    }
    else
    {
        if (out.checked && !out.valid)
            sprintf(this->response, "%s", "Remote server did not honor the Range request");
        else
            sprintf(this->response, "%s", res.errMessage.c_str());
    }
    return 0;
}

int BaseClient::GetRange(const std::string &path, void *buffer, uint64_t size, uint64_t offset)
{
    CHTTPClient::HttpResponse res;
    CHTTPClient::HeadersMap headers;

    if (size == 0)
        return 0;

    char range_header[128];
    sprintf(range_header, "bytes=%lu-%lu", offset, offset + size - 1);
    headers["Range"] = range_header;

    std::string encoded_url = this->host_url + CHTTPClient::EncodeUrl(GetFullPath(path));
    client->SetProgressFnCallback(nullptr, NothingCallback);
    RangeTransferContext out = {&res, nullptr, (char*)buffer, offset, size, 0, false, false};
    if (client->Get(encoded_url, headers, res, (void*) &WriteBufferCallback, (void*) &out))
    {
        if (res.iCode == 206 && out.valid && out.written == size)
            return 1;
    }
    else
    {
        if (out.checked && !out.valid)
            sprintf(this->response, "%s", "Remote server did not honor the Range request");
        else
            sprintf(this->response, "%s", res.errMessage.c_str());
    }
    return 0;
}

int BaseClient::Put(const std::string &inputfile, const std::string &path, uint64_t offset)
{
    sprintf(this->response, "%s", lang_strings[STR_UNSUPPORTED_OPERATION_MSG]);
    return 0;
}

int BaseClient::Rename(const std::string &src, const std::string &dst)
{
    sprintf(this->response, "%s", lang_strings[STR_UNSUPPORTED_OPERATION_MSG]);
    return 0;
}

int BaseClient::Delete(const std::string &path)
{
    sprintf(this->response, "%s", lang_strings[STR_UNSUPPORTED_OPERATION_MSG]);
    return 0;
}

int BaseClient::Copy(const std::string &from, const std::string &to)
{
    sprintf(this->response, "%s", lang_strings[STR_UNSUPPORTED_OPERATION_MSG]);
    return 0;
}

int BaseClient::Move(const std::string &from, const std::string &to)
{
    sprintf(this->response, "%s", lang_strings[STR_UNSUPPORTED_OPERATION_MSG]);
    return 0;
}

int BaseClient::Head(const std::string &path, void *buffer, uint64_t size)
{
    CHTTPClient::HttpResponse res;
    CHTTPClient::HeadersMap headers;

    if (size == 0)
        return 0;

    char range_header[128];
    sprintf(range_header, "bytes=%lu-%lu", 0L, size - 1);
    headers["Range"] = range_header;

    std::string encoded_url = this->host_url + CHTTPClient::EncodeUrl(GetFullPath(path));
    client->SetProgressFnCallback(nullptr, NothingCallback);
    RangeTransferContext out = {&res, nullptr, (char*)buffer, 0, size, 0, false, false};
    if (client->Get(encoded_url, headers, res, (void*) &WriteBufferCallback, (void*) &out))
    {
        if (res.iCode == 206 && out.valid && out.written == size)
            return 1;
    }
    else
    {
        if (out.checked && !out.valid)
            sprintf(this->response, "%s", "Remote server did not honor the Range request");
        else
            sprintf(this->response, "%s", res.errMessage.c_str());
    }
    return 0;
}

bool BaseClient::FileExists(const std::string &path)
{
    uint64_t file_size;
    return Size(path, &file_size);
}

std::vector<DirEntry> BaseClient::ListDir(const std::string &path)
{
    std::vector<DirEntry> out;
    DirEntry entry;
    Util::SetupPreviousFolder(path, &entry);
    out.push_back(entry);

    return out;
}

std::string BaseClient::GetPath(std::string ppath1, std::string ppath2)
{
    std::string path1 = ppath1;
    std::string path2 = ppath2;
    path1 = Util::Trim(Util::Trim(path1, " "), "/");
    path2 = Util::Trim(Util::Trim(path2, " "), "/");
    path1 = this->base_path + ((this->base_path.length() > 0) ? "/" : "") + path1 + "/" + path2;
    if (path1[0] != '/')
        path1 = "/" + path1;
    return path1;
}

std::string BaseClient::GetFullPath(std::string ppath1)
{
    std::string path1 = ppath1;
    path1 = Util::Trim(Util::Trim(path1, " "), "/");
    path1 = this->base_path + "/" + path1;
    Util::ReplaceAll(path1, "//", "/");
    return path1;
}

bool BaseClient::IsConnected()
{
    return this->connected;
}

bool BaseClient::Ping()
{
    CHTTPClient::HttpResponse res;
    CHTTPClient::HeadersMap headers;

    std::string encoded_url = this->host_url + CHTTPClient::EncodeUrl(GetFullPath("/"));
    if (client->Head(encoded_url, headers, res))
    {
        return true;
    }
    else
    {
        sprintf(this->response, "%s", res.errMessage.c_str());
    }
    return false;
}

const char *BaseClient::LastResponse()
{
    return this->response;
}

int BaseClient::Quit()
{
    if (client != nullptr)
    {
        client->CleanupSession();
        delete client;
        client = nullptr;
    }
    return 1;
}

ClientType BaseClient::clientType()
{
    return CLIENT_TYPE_HTTP_SERVER;
}

uint32_t BaseClient::SupportedActions()
{
    return REMOTE_ACTION_DOWNLOAD | REMOTE_ACTION_INSTALL | REMOTE_ACTION_EXTRACT;
}

std::string BaseClient::GetDirectUrl(const std::string &path)
{
    if (this->host_url.empty())
        return "";

    return this->host_url + CHTTPClient::EncodeUrl(GetFullPath(path));
}

std::string BaseClient::Escape(const std::string &url)
{
    CURL *curl = curl_easy_init();
    if (curl)
    {
        char *output = curl_easy_escape(curl, url.c_str(), url.length());
        if (output)
        {
            std::string encoded_url = std::string(output);
            curl_free(output);
            curl_easy_cleanup(curl);
            return encoded_url;
        }
        curl_easy_cleanup(curl);
    }
    return "";
}

std::string BaseClient::UnEscape(const std::string &url)
{
    CURL *curl = curl_easy_init();
    if (curl)
    {
        int decode_len;
        char *output = curl_easy_unescape(curl, url.c_str(), url.length(), &decode_len);
        if (output)
        {
            std::string decoded_url = std::string(output, decode_len);
            curl_free(output);
            curl_easy_cleanup(curl);
            return decoded_url;
        }
        curl_easy_cleanup(curl);
    }
    return "";
}

void *BaseClient::Open(const std::string &path, int flags)
{
    sprintf(this->response, "%s", lang_strings[STR_UNSUPPORTED_OPERATION_MSG]);
    return nullptr;
}

void BaseClient::Close(void *fp)
{
    sprintf(this->response, "%s", lang_strings[STR_UNSUPPORTED_OPERATION_MSG]);
}

int BaseClient::GetRange(void *fp, DataSink &sink, uint64_t size, uint64_t offset)
{
    sprintf(this->response, "%s", lang_strings[STR_UNSUPPORTED_OPERATION_MSG]);
    return -1;
}

int BaseClient::GetRange(void *fp, void *buffer, uint64_t size, uint64_t offset)
{
    sprintf(this->response, "%s", lang_strings[STR_UNSUPPORTED_OPERATION_MSG]);
    return -1;
}
