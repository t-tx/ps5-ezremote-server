#include <fstream>
#include <curl/curl.h>
#include <sys/time.h>
#include <stdint.h>
#include "sceSystemService.h"
#include "clients/remote_client.h"
#include "clients/baseclient.h"
#include "config.h"
#include "util.h"

using httplib::Client;
using httplib::DataSink;
using httplib::Headers;
using httplib::Result;

struct RangeTransferContext {
    CHTTPClient::HttpResponse *response;
    DataSink *sink;
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

struct download_status
{
    uint64_t offset;
    uint64_t *bytes_transfered;
};

BaseClient::BaseClient(){};

BaseClient::~BaseClient()
{
    if (client != nullptr)
        delete client;
};

int BaseClient::DownloadProgressCallback(void* ptr, double dTotalToDownload, double dNowDownloaded, double dTotalToUpload, double dNowUploaded)
{
    CHTTPClient::ProgressFnStruct *progress_data = (CHTTPClient::ProgressFnStruct*) ptr;
    download_status *status = (download_status *) progress_data->pOwner;
    *(status->bytes_transfered) = status->offset + (uint64_t)dNowDownloaded;
    sceSystemServicePowerTick();
    return 0;
}

size_t BaseClient::WriteCallback(void *pCurlData, size_t usBlockCount, size_t usBlockSize, void *pUserData)
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

int BaseClient::Connect(const std::string &url, const std::string &username, const std::string &password)
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
    client->SetBufferSize(524288L);

    this->connected = true;

    return 1;
}

int BaseClient::Get(const std::string &outputfile, const std::string &path, uint64_t offset)
{
    long status = 0;
    *g_bytes_transfered = offset;

    CHTTPClient::HeadersMap headers;
    download_status *dl_status = (download_status*)malloc(sizeof(download_status));
    dl_status->offset = offset;
    dl_status->bytes_transfered = g_bytes_transfered;

    client->SetProgressFnCallback(dl_status, DownloadProgressCallback);
    std::string encoded_url = this->host_url + CHTTPClient::EncodeUrl(GetFullPath(path));
    if (client->DownloadFile(outputfile, encoded_url, status, offset))
    {
        free(dl_status);
        if ((offset > 0 && status == 206) || (offset == 0 && HTTP_SUCCESS(status)))
            return 1;

        sprintf(this->response, "HTTP status %ld", status);
        return 0;
    }
    else
    {
        sprintf(this->response, "HTTP status %ld", status);
    }
    free(dl_status);
    
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
    RangeTransferContext out = {&res, &sink, offset, size, 0, false, false};
    if (client->Get(encoded_url, headers, res, (void*)&WriteCallback, (void*)&out))
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
