#include <algorithm>
#include <string>
#include <cstdio>
#include <cerrno>
#include <cstddef>
#include <cstring>
#include <atomic>
#include <map>
#include <mutex>
#include <vector>
#include <fstream>
#include <cstring>
#include <iomanip>
#include <thread>
#include <atomic>
#include <sstream>
#include <sys/resource.h>
#include <sys/sysctl.h>
#include <sys/types.h>
#include <sys/user.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <unistd.h>
#include <sys/statvfs.h>
#include <json-c/json.h>
#include "http/httplib.h"
#include "ps5_api/fs.h"
#include "clients/remote_client.h"
#include "wrapper/zip_util.h"
#include "clients/baseclient.h"
#include "clients/ftpclient.h"
#include "clients/iis.h"
#include "config.h"
#include "ps5_api/fs.h"
#include "util.h"
#include "windows.h"
#include "usecase/dpi_usecase.h"
#include "dbglogger.h"
#include "wrapper/installer.h"
#include "usecase/pkg_install_usecase.h"
#include "usecase/file_manager_usecase.h"
static Usecase::PkgInstallUseCase g_pkg_installer;
static Usecase::FileManagerUseCase g_file_manager;

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

static pthread_t bg_extract_thread;
static std::atomic<bool> bg_extract_running{false};
static bool bg_extract_thread_started = false;
static std::atomic<int> bg_transfer_active_jobs{0};
static uint64_t g_dl_offset;

std::vector<RemoteSettings> configured_sites;

namespace HttpServer
{
    static const int MAX_ACTIVE_TRANSFER_JOBS = 1;
    static const unsigned short PAYLOAD_LOADER_PORT = 9021;
    static const char *RESTART_SERVER_ARG = "restart-server";

    static bool WriteAllToSocket(int fd, const char *data, size_t size)
    {
        while (size > 0)
        {
            ssize_t written = write(fd, data, size);
            if (written <= 0)
            {
                return false;
            }
            data += written;
            size -= static_cast<size_t>(written);
        }
        return true;
    }

    static bool LaunchRestartClient()
    {
        if (access(CLIENT_ELF_PATH, R_OK) != 0)
        {
            dbglogger_log("[Restart] Client ELF not readable: %s", CLIENT_ELF_PATH);
            return false;
        }

        int sockfd = socket(AF_INET, SOCK_STREAM, 0);
        if (sockfd < 0)
        {
            dbglogger_log("[Restart] Failed to create payload loader socket: %s", strerror(errno));
            return false;
        }

        struct sockaddr_in addr;
        memset(&addr, 0, sizeof(addr));
        addr.sin_family = AF_INET;
        addr.sin_port = htons(PAYLOAD_LOADER_PORT);
        addr.sin_addr.s_addr = inet_addr("127.0.0.1");

        if (connect(sockfd, reinterpret_cast<struct sockaddr *>(&addr), sizeof(addr)) != 0)
        {
            dbglogger_log("[Restart] Failed to connect to payload loader: %s", strerror(errno));
            close(sockfd);
            return false;
        }

        std::string uri = std::string("file:") + CLIENT_ELF_PATH + "?args=" + RESTART_SERVER_ARG + "\n";
        bool ok = WriteAllToSocket(sockfd, uri.c_str(), uri.size());
        close(sockfd);

        if (!ok)
        {
            dbglogger_log("[Restart] Failed to send restart client URI to payload loader.");
            return false;
        }

        dbglogger_log("[Restart] Launched ezRemote Client restart helper.");
        return true;
    }

    static bool TryReserveBackgroundTransferSlot()
    {
        int active = bg_transfer_active_jobs.load();
        while (active < MAX_ACTIVE_TRANSFER_JOBS)
        {
            if (bg_transfer_active_jobs.compare_exchange_weak(active, active + 1))
                return true;
        }
        return false;
    }

    static void ReleaseBackgroundTransferSlot()
    {
        int active = bg_transfer_active_jobs.load();
        while (active > 0 && !bg_transfer_active_jobs.compare_exchange_weak(active, active - 1))
        {
        }
    }

    struct BackgroundTransferSlotGuard
    {
        ~BackgroundTransferSlotGuard()
        {
            ReleaseBackgroundTransferSlot();
        }
    };

    static int FtpCallback(int64_t xfered, void *arg)
    {
        return 1;
    }

    struct FtpDownloadProgress {
        uint64_t* bytes_transfered;
        uint64_t offset;
        bool* cancel_flag;
    };

    static int DownloadFtpCallback(int64_t xfered, void *arg)
    {
        FtpDownloadProgress* prog = (FtpDownloadProgress*)arg;
        if (prog && prog->bytes_transfered) {
            *(prog->bytes_transfered) = prog->offset + xfered;
        }
        if (prog && prog->cancel_flag && *(prog->cancel_flag)) {
            return 0; // Abort download
        }
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
    
    void set_cors_header(httplib::Response &res)
    {
        res.set_header("Access-Control-Allow-Origin", "*");
        res.set_header("Access-Control-Allow-Methods", "POST, GET, OPTIONS, PUT, DELETE");
        res.set_header("Access-Control-Allow-Headers", "Content-Type, Accept, Origin, Referer, User-Agent, Connection, Cache-Control, Pragma, Accept-Language");
    }

    static RemoteClient *GetRemoteClient(HostInfo *host_info)
    {
        if (host_info == nullptr)
            return nullptr;

        RemoteClient *tmp_client = nullptr;

        if (host_info->type == CLIENT_TYPE_HTTP_SERVER)
        {
            if (host_info->http_server_type.compare(HTTP_SERVER_MS_IIS) == 0)
                tmp_client = new IISClient();
            else
                tmp_client = new BaseClient();
        }
        else if (host_info->type == CLIENT_TYPE_FILEHOST)
        {
            tmp_client = new BaseClient();
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

    struct ProcessMemoryStats
    {
        pid_t pid;
        long page_size;
        bool has_current;
        bool has_peak;
        uint64_t rss_bytes;
        uint64_t virtual_bytes;
        uint64_t text_bytes;
        uint64_t data_bytes;
        uint64_t stack_bytes;
        long peak_rss_kb;
        uint64_t peak_rss_bytes;
        int current_errno;
        size_t kern_proc_pid_size;
        const char *current_source;
        bool has_cpu;
        bool has_cpu_percent;
        uint64_t user_cpu_us;
        uint64_t system_cpu_us;
        uint64_t total_cpu_us;
        uint64_t cpu_delta_us;
        uint64_t cpu_sample_interval_us;
        double cpu_percent_since_last_sample;
    };

    static std::mutex cpu_stats_mutex;
    static uint64_t previous_cpu_total_us = 0;
    static uint64_t previous_cpu_wall_us = 0;

    static uint64_t TimevalToMicros(const struct timeval &value)
    {
        return (static_cast<uint64_t>(value.tv_sec) * 1000000ULL) + static_cast<uint64_t>(value.tv_usec);
    }

    static double BytesToMiB(uint64_t bytes)
    {
        return static_cast<double>(bytes) / 1048576.0;
    }

    static double MicrosToSeconds(uint64_t micros)
    {
        return static_cast<double>(micros) / 1000000.0;
    }

    static void UpdateCpuPercent(ProcessMemoryStats &stats)
    {
        uint64_t now_us = Util::GetTick();
        std::lock_guard<std::mutex> lock(cpu_stats_mutex);

        if (previous_cpu_wall_us > 0 && now_us > previous_cpu_wall_us && stats.total_cpu_us >= previous_cpu_total_us)
        {
            stats.cpu_delta_us = stats.total_cpu_us - previous_cpu_total_us;
            stats.cpu_sample_interval_us = now_us - previous_cpu_wall_us;
            stats.cpu_percent_since_last_sample = (static_cast<double>(stats.cpu_delta_us) * 100.0) / static_cast<double>(stats.cpu_sample_interval_us);
            stats.has_cpu_percent = true;
        }

        previous_cpu_total_us = stats.total_cpu_us;
        previous_cpu_wall_us = now_us;
    }

    static bool ApplyProcessMemoryStats(ProcessMemoryStats &stats, const struct kinfo_proc *proc, size_t proc_size, const char *source)
    {
        size_t min_proc_size = offsetof(struct kinfo_proc, ki_ssize) + sizeof(proc->ki_ssize);
        if (proc == nullptr || proc_size < min_proc_size || proc->ki_pid != stats.pid)
            return false;

        uint64_t page_size = static_cast<uint64_t>(stats.page_size);
        stats.has_current = true;
        stats.rss_bytes = static_cast<uint64_t>(proc->ki_rssize) * page_size;
        stats.virtual_bytes = static_cast<uint64_t>(proc->ki_size);
        stats.text_bytes = static_cast<uint64_t>(proc->ki_tsize) * page_size;
        stats.data_bytes = static_cast<uint64_t>(proc->ki_dsize) * page_size;
        stats.stack_bytes = static_cast<uint64_t>(proc->ki_ssize) * page_size;
        stats.current_source = source;
        return true;
    }

    static bool GetCurrentProcessMemoryFromProcessList(ProcessMemoryStats &stats)
    {
        int mib[4] = {CTL_KERN, KERN_PROC, KERN_PROC_PROC, 0};
        size_t buf_size = 0;
        if (sysctl(mib, 4, nullptr, &buf_size, nullptr, 0) != 0 || buf_size == 0)
        {
            stats.current_errno = errno;
            return false;
        }

        std::vector<unsigned char> buf(buf_size);
        if (sysctl(mib, 4, buf.data(), &buf_size, nullptr, 0) != 0)
        {
            stats.current_errno = errno;
            return false;
        }

        for (size_t offset = 0; offset + sizeof(int) <= buf_size;)
        {
            struct kinfo_proc *proc = reinterpret_cast<struct kinfo_proc *>(buf.data() + offset);
            if (proc->ki_structsize <= 0 || offset + static_cast<size_t>(proc->ki_structsize) > buf_size)
                break;

            if (ApplyProcessMemoryStats(stats, proc, static_cast<size_t>(proc->ki_structsize), "kern_proc_proc"))
                return true;

            offset += static_cast<size_t>(proc->ki_structsize);
        }

        return false;
    }

    static ProcessMemoryStats GetProcessMemoryStats()
    {
        ProcessMemoryStats stats = {};
        stats.pid = getpid();
        stats.page_size = getpagesize();
        stats.current_source = "unavailable";
        if (stats.page_size <= 0)
            stats.page_size = 4096;

        struct rusage usage;
        if (getrusage(RUSAGE_SELF, &usage) == 0)
        {
            stats.has_peak = true;
            stats.peak_rss_kb = usage.ru_maxrss;
            stats.peak_rss_bytes = static_cast<uint64_t>(usage.ru_maxrss) * 1024ULL;
            stats.has_cpu = true;
            stats.user_cpu_us = TimevalToMicros(usage.ru_utime);
            stats.system_cpu_us = TimevalToMicros(usage.ru_stime);
            stats.total_cpu_us = stats.user_cpu_us + stats.system_cpu_us;
            UpdateCpuPercent(stats);
        }

        struct kinfo_proc proc;
        memset(&proc, 0, sizeof(proc));
        size_t proc_size = sizeof(proc);
        int mib[4] = {CTL_KERN, KERN_PROC, KERN_PROC_PID, stats.pid};
        if (sysctl(mib, 4, &proc, &proc_size, nullptr, 0) == 0)
        {
            stats.kern_proc_pid_size = proc_size;
            ApplyProcessMemoryStats(stats, &proc, proc_size, "kern_proc_pid");
        }
        else
        {
            stats.current_errno = errno;
        }

        if (!stats.has_current)
            GetCurrentProcessMemoryFromProcessList(stats);

        return stats;
    }

    static std::string ProcessMemoryStatsJson()
    {
        ProcessMemoryStats stats = GetProcessMemoryStats();
        std::ostringstream payload;
        payload << std::fixed << std::setprecision(2)
                << "{"
                << "\"mem\":{"
                << "\"rss_mib\":" << BytesToMiB(stats.rss_bytes) << ","
                << "\"peak_rss_mib\":" << BytesToMiB(stats.peak_rss_bytes) << ","
                << "\"virtual_mib\":" << BytesToMiB(stats.virtual_bytes) << ","
                << "\"data_mib\":" << BytesToMiB(stats.data_bytes) << ","
                << "\"stack_mib\":" << BytesToMiB(stats.stack_bytes) << ","
                << "\"has_current\":" << (stats.has_current ? "true" : "false") << ","
                << "\"source\":\"" << stats.current_source << "\","
                << "\"rss_bytes\":" << stats.rss_bytes << ","
                << "\"virtual_bytes\":" << stats.virtual_bytes << ","
                << "\"text_bytes\":" << stats.text_bytes << ","
                << "\"data_bytes\":" << stats.data_bytes << ","
                << "\"stack_bytes\":" << stats.stack_bytes << ","
                << "\"has_peak\":" << (stats.has_peak ? "true" : "false") << ","
                << "\"peak_rss_kb\":" << stats.peak_rss_kb << ","
                << "\"peak_rss_bytes\":" << stats.peak_rss_bytes << ","
                << "\"page_size\":" << stats.page_size << ","
                << "\"current_errno\":" << stats.current_errno << ","
                << "\"kern_proc_pid_size\":" << stats.kern_proc_pid_size
                << "},"
                << "\"cpu\":{"
                << "\"percent\":" << stats.cpu_percent_since_last_sample << ","
                << "\"has_percent\":" << (stats.has_cpu_percent ? "true" : "false") << ","
                << "\"total_seconds\":" << MicrosToSeconds(stats.total_cpu_us) << ","
                << "\"user_seconds\":" << MicrosToSeconds(stats.user_cpu_us) << ","
                << "\"system_seconds\":" << MicrosToSeconds(stats.system_cpu_us) << ","
                << "\"has_cpu\":" << (stats.has_cpu ? "true" : "false") << ","
                << "\"total_us\":" << stats.total_cpu_us << ","
                << "\"user_us\":" << stats.user_cpu_us << ","
                << "\"system_us\":" << stats.system_cpu_us << ","
                << "\"delta_us\":" << stats.cpu_delta_us << ","
                << "\"sample_interval_us\":" << stats.cpu_sample_interval_us << ","
                << "\"pid\":" << static_cast<int>(stats.pid)
                << "}"
                << "}";
        return payload.str();
    }

    struct DownloadThreadArgs {
        uint64_t id;
    };

    struct FileToDownload {
        std::string src_path;
        std::string dest_path;
        uint64_t size;
    };

    static void BuildDownloadList(RemoteClient* client, const std::string& src_dir, const std::string& dest_dir, std::vector<FileToDownload>& list, uint64_t& total_size, BgDownloadData* active_dl) {
        if (active_dl && active_dl->cancel_requested) return;
        dbglogger_log("[BuildDownloadList] Listing %s", src_dir.c_str());
        std::vector<DirEntry> entries = client->ListDir(src_dir.c_str());
        dbglogger_log("[BuildDownloadList] Found %zu entries in %s. Last response: %s", entries.size(), src_dir.c_str(), client->LastResponse());
        FS::MkDirs(dest_dir.c_str());
        for (const auto& entry : entries) {
            if (active_dl && active_dl->cancel_requested) return;
            if (entry.isDir && strcmp(entry.name, "..") == 0) continue;
            std::string new_dest = dest_dir + "/" + entry.name;
            if (entry.isDir) {
                BuildDownloadList(client, entry.path, new_dest, list, total_size, active_dl);
            } else {
                dbglogger_log("[BuildDownloadList] Adding file %s (size %lu)", entry.path, entry.file_size);
                list.push_back({entry.path, new_dest, entry.file_size});
                total_size += entry.file_size;
            }
        }
    }


    void *DownloadSingleFileThread(void *argp)
    {
        pthread_detach(pthread_self());
        BackgroundTransferSlotGuard slot_guard;
        DownloadThreadArgs *args = static_cast<DownloadThreadArgs *>(argp);
        uint64_t id = args->id;
        delete args;

        char temp_file[2049];
        uint64_t tmp_file_size;
        int ret;
        BgDownloadData* active_dl = nullptr;

        CONFIG::LockDownloadList();
        for (auto it = bg_download_list.begin(); it != bg_download_list.end(); ++it)
        {
            if (it->id == id)
            {
                active_dl = &(*it);
                break;
            }
        }
        CONFIG::UnlockDownloadList();

        if (active_dl == nullptr) return nullptr;

        RemoteClient *tmp_client = GetRemoteClient(&(active_dl->host_info));
        if (tmp_client == nullptr)
        {
            CONFIG::LockDownloadList();
            active_dl->state = STATE_FAILED;
            active_dl->fail_reason = "Failed to connect to host";
            active_dl->finished_timestamp = Util::GetTick();
            CONFIG::UnlockDownloadList();
            CONFIG::SaveBgDownloadData();
            Util::RichNotify(active_dl->id, "Failed to connect for download %s", active_dl->dest_path.c_str());
            return nullptr;
        }
        tmp_client->SetCancelFlag(&(active_dl->cancel_requested));

        FtpDownloadProgress progress_struct;
        progress_struct.bytes_transfered = &(active_dl->bytes_transfered);
        progress_struct.offset = 0;
        progress_struct.cancel_flag = &(active_dl->cancel_requested);

        if (active_dl->is_dir) {
            std::vector<FileToDownload> files;
            uint64_t total_size = 0;
            
            BuildDownloadList(tmp_client, active_dl->src_path, active_dl->dest_path, files, total_size, active_dl);
            dbglogger_log("[DownloadSingleFileThread] Folder traversal complete. Files to download: %zu. Total size: %lu", files.size(), total_size);

            uint64_t new_completed_bytes = 0;
            for (const auto& file : files) {
                if (FS::FileExists(file.dest_path.c_str())) {
                    new_completed_bytes += file.size;
                }
            }

            size_t num_threads = std::min((size_t)10, files.size());
            
            CONFIG::LockDownloadList();
            if (total_size > 0) {
                active_dl->file_size = total_size;
            }
            active_dl->completed_bytes = new_completed_bytes;
            active_dl->active_files.assign(num_threads, "");
            CONFIG::UnlockDownloadList();

            std::atomic<bool> failed(false);
            std::atomic<size_t> current_file_idx(0);
            
            std::vector<uint64_t> thread_progress(num_threads, 0);
            std::vector<std::thread> workers;
            std::atomic<int> active_threads(num_threads);

            for (size_t t = 0; t < num_threads; ++t) {
                workers.emplace_back([&, t]() {
                    RemoteClient *worker_client = GetRemoteClient(&(active_dl->host_info));
                    if (!worker_client) {
                        failed = true;
                        active_threads--;
                        return;
                    }
                    worker_client->SetCancelFlag(&(active_dl->cancel_requested));

                    FtpDownloadProgress progress_struct;
                    progress_struct.bytes_transfered = &thread_progress[t];
                    progress_struct.offset = 0;
                    progress_struct.cancel_flag = &(active_dl->cancel_requested);

                    if (active_dl->host_info.type == CLIENT_TYPE_FTP) {
                        FtpClient *ftpclient = (FtpClient*)worker_client;
                        ftpclient->SetCallbackBytes(1);
                        ftpclient->SetCallbackArg(&progress_struct);
                        ftpclient->SetCallbackXferFunction(DownloadFtpCallback);
                    }

                    while (!failed && !active_dl->cancel_requested) {
                        size_t idx = current_file_idx.fetch_add(1);
                        if (idx >= files.size()) break;

                        const auto& file = files[idx];
                        if (FS::FileExists(file.dest_path.c_str())) {
                            // File already completed in a previous run
                            continue;
                        }
                        
                        char temp_file[1024];
                        snprintf(temp_file, sizeof(temp_file), "%s.tmp", file.dest_path.c_str());

                        CONFIG::LockDownloadList();
                        if (t < active_dl->active_files.size()) {
                            active_dl->active_files[t] = file.src_path;
                        }
                        CONFIG::UnlockDownloadList();
                        
                        dbglogger_log("[DownloadSingleFileThread] Thread %zu downloading %s to %s", t, file.src_path.c_str(), temp_file);
                        
                        bool is_resume = FS::FileExists(temp_file);
                        uint64_t tmp_file_size = is_resume ? FS::GetSize(temp_file) : 0;
                        progress_struct.offset = tmp_file_size;
                        
                        int ret = worker_client->Get(temp_file, file.src_path, tmp_file_size);
                        
                        if (ret == 0) {
                            failed = true;
                            dbglogger_log("[DownloadSingleFileThread] File download failed: %s", file.src_path.c_str());
                            break;
                        }
                        
                        if (!CompleteDownloadedFile(temp_file, file.dest_path, file.size)) {
                            failed = true;
                            dbglogger_log("[DownloadSingleFileThread] CompleteDownloadedFile failed for %s", temp_file);
                            break;
                        }

                        CONFIG::LockDownloadList();
                        active_dl->completed_bytes += file.size;
                        if (t < active_dl->active_files.size()) {
                            active_dl->active_files[t] = "";
                        }
                        CONFIG::UnlockDownloadList();
                        thread_progress[t] = 0; // reset for next file
                    }

                    CONFIG::LockDownloadList();
                    if (t < active_dl->active_files.size()) {
                        active_dl->active_files[t] = "";
                    }
                    CONFIG::UnlockDownloadList();
                    delete worker_client;
                    active_threads--;
                });
            }

            while (active_threads > 0) {
                uint64_t current_transfer = 0;
                for (size_t t = 0; t < num_threads; ++t) {
                    current_transfer += thread_progress[t];
                }
                CONFIG::LockDownloadList();
                active_dl->bytes_transfered = current_transfer;
                CONFIG::UnlockDownloadList();
                usleep(100000); // 100ms
            }

            for (auto& w : workers) {
                if (w.joinable()) {
                    w.join();
                }
            }

            CONFIG::LockDownloadList();
            if (active_dl->cancel_requested) {
                active_dl->state = STATE_FAILED;
                active_dl->fail_reason = "Cancelled by user";
                Util::RichNotify(active_dl->id, "Cancelled download %s", active_dl->dest_path.c_str());
            }
            else if (failed) {
                active_dl->state = STATE_FAILED;
                active_dl->fail_reason = "Failed to download some files";
                Util::RichNotify(active_dl->id, "Failed to download folder %s", active_dl->dest_path.c_str());
                FS::RmRecursive(active_dl->dest_path);
            }
            else {
                active_dl->state = STATE_SUCCESS;
                Util::RichNotify(active_dl->id, "Completed folder download %s", active_dl->dest_path.c_str());
            }
            active_dl->active_files.clear();
            active_dl->finished_timestamp = Util::GetTick();
            CONFIG::UnlockDownloadList();
        } else {
            CONFIG::LockDownloadList();
            active_dl->active_files.assign(1, active_dl->src_path);
            CONFIG::UnlockDownloadList();

            if (active_dl->host_info.type == CLIENT_TYPE_FTP)
            {
                FtpClient *ftpclient = (FtpClient*)tmp_client;
                ftpclient->SetCallbackBytes(1);
                ftpclient->SetCallbackArg(&progress_struct);
                ftpclient->SetCallbackXferFunction(DownloadFtpCallback);
            }

            snprintf(temp_file, sizeof(temp_file), "%s.tmp", active_dl->dest_path.c_str());

            bool is_resume = FS::FileExists(temp_file);
            if (is_resume)
            {
                tmp_file_size = FS::GetSize(temp_file);
                progress_struct.offset = tmp_file_size;
                Util::RichNotify(active_dl->id, "Resuming download %s", active_dl->dest_path.c_str());
                ret = tmp_client->Get(temp_file, active_dl->src_path, tmp_file_size);
            }
            else
            {
                Util::RichNotify(active_dl->id, "Started download %s", active_dl->dest_path.c_str());
                ret = tmp_client->Get(temp_file, active_dl->src_path);
            }

            CONFIG::LockDownloadList();
            active_dl->active_files.clear();
            if (active_dl->cancel_requested)
            {
                active_dl->state = STATE_FAILED;
                active_dl->fail_reason = "Cancelled by user";
                Util::RichNotify(active_dl->id, "Cancelled download %s", active_dl->dest_path.c_str());
                dbglogger_log("[Download Task] Cancelled task ID %lu", active_dl->id);
            }
            else if (ret == 0 || !CompleteDownloadedFile(temp_file, active_dl->dest_path, active_dl->file_size))
            {
                active_dl->state = STATE_FAILED;
                active_dl->fail_reason = "Failed to download or write file";
                Util::RichNotify(active_dl->id, "Failed to download %s", active_dl->dest_path.c_str());
                dbglogger_log("[Download Task] Failed task ID %lu", active_dl->id);
            }
            else
            {
                Util::RichNotify(active_dl->id, "Completed download %s", active_dl->dest_path.c_str());
                active_dl->state = STATE_SUCCESS;
                dbglogger_log("[Download Task] Completed task ID %lu", active_dl->id);
            }
            active_dl->finished_timestamp = Util::GetTick();
            CONFIG::UnlockDownloadList();
        }

        CONFIG::SaveBgDownloadData();

        DeleteRemoteClient(tmp_client);
        return nullptr;
    }

    void *DownloadFilesThread(void *argp)
    {
        dbglogger_log("[DownloadFilesThread] Started");
        while (bg_download_running.load())
        {
            std::vector<uint64_t> pending_jobs;

            CONFIG::LockDownloadList();
            for (auto it = bg_download_list.begin(); it != bg_download_list.end(); ++it)
            {
                if (it->state == STATE_PENDING || it->state == STATE_RESUMED)
                {
                    if (!TryReserveBackgroundTransferSlot())
                        break;

                    it->state = STATE_DOWNLOADING;
                    pending_jobs.push_back(it->id);
                    break;
                }
            }
            CONFIG::UnlockDownloadList();

            for (uint64_t id : pending_jobs)
            {
                DownloadThreadArgs *args = new DownloadThreadArgs();
                args->id = id;
                pthread_t thread;
                if (pthread_create(&thread, NULL, DownloadSingleFileThread, args) != 0)
                {
                    delete args;
                    ReleaseBackgroundTransferSlot();
                    CONFIG::LockDownloadList();
                    for (auto it = bg_download_list.begin(); it != bg_download_list.end(); ++it)
                    {
                        if (it->id == id)
                        {
                            it->state = STATE_FAILED;
                            it->fail_reason = "Failed to start download thread";
                            it->finished_timestamp = Util::GetTick();
                            break;
                        }
                    }
                    CONFIG::UnlockDownloadList();
                    CONFIG::SaveBgDownloadData();
                }
            }

            sleep(1);
        }

        return nullptr;
    }

    struct ExtractThreadArgs {
        uint64_t id;
    };

    static std::string ExtractBaseName(const std::string &path)
    {
        size_t end = path.find_last_not_of('/');
        if (end == std::string::npos) return "extract";
        size_t slash = path.find_last_of('/', end);
        if (slash == std::string::npos) return path.substr(0, end + 1);
        return path.substr(slash + 1, end - slash);
    }

    void *ExtractSingleFileThread(void *argp)
    {
        pthread_detach(pthread_self());
        BackgroundTransferSlotGuard slot_guard;
        ExtractThreadArgs *args = static_cast<ExtractThreadArgs *>(argp);
        uint64_t id = args->id;
        delete args;

        BgExtractData* active_ext = nullptr;

        CONFIG::LockExtractList();
        for (auto it = bg_extract_list.begin(); it != bg_extract_list.end(); ++it)
        {
            if (it->id == id)
            {
                active_ext = &(*it);
                break;
            }
        }
        CONFIG::UnlockExtractList();

        if (active_ext == nullptr) return nullptr;

        DirEntry entry;
        memset(&entry, 0, sizeof(entry));
        snprintf(entry.name, sizeof(entry.name), "%s", ExtractBaseName(active_ext->src_path).c_str());
        snprintf(entry.path, sizeof(entry.path), "%s", active_ext->src_path.c_str());
        entry.isDir = false;

        if (FS::FileExists(active_ext->dest_path + "/" + active_ext->folder_name) || FS::FolderExists(active_ext->dest_path + "/" + active_ext->folder_name))
        {
            CONFIG::LockExtractList();
            active_ext->state = EXTRACT_STATE_FAILED;
            active_ext->fail_reason = "Destination already exists";
            active_ext->finished_timestamp = Util::GetTick();
            CONFIG::UnlockExtractList();
            CONFIG::SaveBgExtractData();
            Util::RichNotify(active_ext->id, "Extraction failed: Destination exists");
            return nullptr;
        }

        std::string staging_path = active_ext->dest_path + "/.extract_" + std::to_string(id);
        std::string final_path = active_ext->dest_path + "/" + active_ext->folder_name;

        FS::MkDirs(staging_path);

        RemoteClient *tmp_client = nullptr;
        int ret = 0;
        bytes_transfered = 0;
        bytes_to_download = 0;
        
        if (!active_ext->host_info.url.empty())
        {
            tmp_client = GetRemoteClient(&(active_ext->host_info));
            if (tmp_client == nullptr || !tmp_client->IsConnected())
            {
                if (tmp_client) DeleteRemoteClient(tmp_client);
                CONFIG::LockExtractList();
                active_ext->state = EXTRACT_STATE_FAILED;
                active_ext->fail_reason = "Failed to connect to remote site";
                active_ext->finished_timestamp = Util::GetTick();
                CONFIG::UnlockExtractList();
                CONFIG::SaveBgExtractData();
                FS::RmRecursive(staging_path);
                Util::RichNotify(active_ext->id, "Extraction failed: Cannot connect to remote host");
                return nullptr;
            }
            tmp_client->SetCancelFlag(&(active_ext->cancel_requested));
            dbglogger_log("[Extract Task] Starting remote extraction for task ID %lu: %s", id, entry.name);
            ret = ZipUtil::Extract(entry, staging_path, tmp_client, &(active_ext->cancel_requested));
            DeleteRemoteClient(tmp_client);
        }
        else
        {
            dbglogger_log("[Extract Task] Starting local extraction for task ID %lu: %s", id, entry.name);
            ret = ZipUtil::Extract(entry, staging_path, nullptr, &(active_ext->cancel_requested));
        }

        if (active_ext->cancel_requested)
        {
            CONFIG::LockExtractList();
            active_ext->state = EXTRACT_STATE_FAILED;
            active_ext->fail_reason = "Cancelled by user";
            active_ext->finished_timestamp = Util::GetTick();
            CONFIG::UnlockExtractList();
            CONFIG::SaveBgExtractData();
            FS::RmRecursive(staging_path);
            Util::RichNotify(active_ext->id, "Cancelled extraction %s", active_ext->folder_name.c_str());
            dbglogger_log("[Extract Task] Cancelled task ID %lu", id);
            return nullptr;
        }
        else if (ret <= 0)
        {
            std::string fail_reason = ret == -1 ? "Unsupported compressed file format" : "Failed to extract file";
            if (strlen(status_message) > 0)
                fail_reason = status_message;
            CONFIG::LockExtractList();
            active_ext->state = EXTRACT_STATE_FAILED;
            active_ext->fail_reason = fail_reason;
            active_ext->bytes_transfered = bytes_transfered;
            active_ext->file_size = bytes_to_download;
            active_ext->finished_timestamp = Util::GetTick();
            CONFIG::UnlockExtractList();
            CONFIG::SaveBgExtractData();
            FS::RmRecursive(staging_path);
            Util::RichNotify(active_ext->id, "Failed to extract %s", active_ext->folder_name.c_str());
            dbglogger_log("[Extract Task] Failed task ID %lu", id);
            return nullptr;
        }

        if (FS::FolderExists(final_path) || FS::FileExists(final_path))
        {
            CONFIG::LockExtractList();
            active_ext->state = EXTRACT_STATE_FAILED;
            active_ext->fail_reason = "Destination already exists";
            active_ext->finished_timestamp = Util::GetTick();
            CONFIG::UnlockExtractList();
            CONFIG::SaveBgExtractData();
            FS::RmRecursive(staging_path);
            Util::RichNotify(active_ext->id, "Extraction failed: Destination exists");
            return nullptr;
        }

        FS::MkDirs(final_path, true);
        if (rename(staging_path.c_str(), final_path.c_str()) != 0)
        {
            CONFIG::LockExtractList();
            active_ext->state = EXTRACT_STATE_FAILED;
            active_ext->fail_reason = "Failed to move extracted directory";
            active_ext->finished_timestamp = Util::GetTick();
            CONFIG::UnlockExtractList();
            CONFIG::SaveBgExtractData();
            FS::RmRecursive(staging_path);
            Util::RichNotify(active_ext->id, "Extraction failed: Could not move directory");
            return nullptr;
        }

        CONFIG::LockExtractList();
        active_ext->state = EXTRACT_STATE_SUCCESS;
        active_ext->bytes_transfered = bytes_transfered;
        active_ext->file_size = bytes_to_download;
        active_ext->finished_timestamp = Util::GetTick();
        CONFIG::UnlockExtractList();
        CONFIG::SaveBgExtractData();

        Util::RichNotify(active_ext->id, "Completed extraction to %s", active_ext->folder_name.c_str());
        dbglogger_log("[Extract Task] Completed task ID %lu", id);
        return nullptr;
    }

    void *ExtractFilesThread(void *argp)
    {
        while (bg_extract_running.load())
        {
            std::vector<uint64_t> pending_jobs;

            CONFIG::LockExtractList();
            for (auto it = bg_extract_list.begin(); it != bg_extract_list.end(); ++it)
            {
                if (it->state == EXTRACT_STATE_PENDING)
                {
                    if (!TryReserveBackgroundTransferSlot())
                        break;

                    it->state = EXTRACT_STATE_EXTRACTING;
                    pending_jobs.push_back(it->id);
                    break;
                }
            }
            CONFIG::UnlockExtractList();

            for (uint64_t id : pending_jobs)
            {
                ExtractThreadArgs *args = new ExtractThreadArgs();
                args->id = id;
                pthread_t thread;
                if (pthread_create(&thread, NULL, ExtractSingleFileThread, args) != 0)
                {
                    delete args;
                    ReleaseBackgroundTransferSlot();
                    CONFIG::LockExtractList();
                    for (auto it = bg_extract_list.begin(); it != bg_extract_list.end(); ++it)
                    {
                        if (it->id == id)
                        {
                            it->state = EXTRACT_STATE_FAILED;
                            it->fail_reason = "Failed to start extraction thread";
                            it->finished_timestamp = Util::GetTick();
                            break;
                        }
                    }
                    CONFIG::UnlockExtractList();
                    CONFIG::SaveBgExtractData();
                }
            }

            sleep(1);
        }

        return nullptr;
    }

    void StopDownloadThread();
    void StopExtractThread();
    
    struct FileOpThreadArgs {
        uint64_t id;
    };

    void *FileOpSingleThread(void *argp)
    {
        pthread_detach(pthread_self());
        FileOpThreadArgs *args = (FileOpThreadArgs *)argp;
        uint64_t op_id = args->id;
        delete args;

        BgFileOpData *active_op = nullptr;

        CONFIG::LockFileOpList();
        for (auto it = bg_fileop_list.begin(); it != bg_fileop_list.end(); ++it)
        {
            if (it->id == op_id)
            {
                active_op = &(*it);
                break;
            }
        }
        CONFIG::UnlockFileOpList();

        if (!active_op)
            return nullptr;

        Util::RichNotify(active_op->id, "Started %s to %s", 
            active_op->type == FILEOP_COPY ? "copy" : (active_op->type == FILEOP_MOVE ? "move" : "delete"), 
            active_op->dest_path.c_str());

        bool all_success = true;
        for (const std::string& item : active_op->items)
        {
            if (active_op->cancel_requested)
            {
                all_success = false;
                break;
            }

            if (active_op->type == FILEOP_DELETE)
            {
                if (FS::FolderExists(item)) FS::RmRecursive(item, &(active_op->cancel_requested));
                else FS::Rm(item);
            }
            else
            {
                std::string basename = item.substr(item.find_last_of('/') + 1);
                std::string target_path = active_op->dest_path + "/" + basename;
                
                int ret = 0;
                if (active_op->type == FILEOP_COPY)
                {
                    ret = FS::Copy(item, target_path, &(active_op->cancel_requested)) ? 0 : -1;
                }
                else if (active_op->type == FILEOP_MOVE)
                {
                    if (rename(item.c_str(), target_path.c_str()) != 0) {
                        ret = FS::Move(item, target_path, &(active_op->cancel_requested)) ? 0 : -1;
                    }
                }
                
                if (ret != 0)
                {
                    all_success = false;
                    break;
                }
            }

            CONFIG::LockFileOpList();
            active_op->items_processed++;
            CONFIG::UnlockFileOpList();
        }

        CONFIG::LockFileOpList();
        if (active_op->cancel_requested) {
            active_op->state = FILEOP_STATE_FAILED;
            active_op->fail_reason = "Cancelled by user";
            Util::RichNotify(active_op->id, "Cancelled %s", active_op->type == FILEOP_COPY ? "copy" : (active_op->type == FILEOP_MOVE ? "move" : "delete"));
            dbglogger_log("[FileOp Task] Cancelled task ID %lu", op_id);
        } else if (all_success) {
            active_op->state = FILEOP_STATE_SUCCESS;
            Util::RichNotify(active_op->id, "Completed %s", active_op->type == FILEOP_COPY ? "copy" : (active_op->type == FILEOP_MOVE ? "move" : "delete"));
            dbglogger_log("[FileOp Task] Completed task ID %lu", op_id);
        } else {
            active_op->state = FILEOP_STATE_FAILED;
            active_op->fail_reason = "Failed to process all items";
            Util::RichNotify(active_op->id, "Failed %s", active_op->type == FILEOP_COPY ? "copy" : (active_op->type == FILEOP_MOVE ? "move" : "delete"));
            dbglogger_log("[FileOp Task] Failed task ID %lu", op_id);
        }
        active_op->finished_timestamp = Util::GetTick();
        CONFIG::UnlockFileOpList();
        CONFIG::SaveBgFileOpData();

        return nullptr;
    }

    std::atomic<bool> bg_fileop_running{false};
    pthread_t fileop_thread;
    static bool bg_fileop_thread_started = false;

    void *FileOpFilesThread(void *argp)
    {
        while (bg_fileop_running.load())
        {
            std::vector<uint64_t> pending_jobs;

            CONFIG::LockFileOpList();
            for (auto it = bg_fileop_list.begin(); it != bg_fileop_list.end(); ++it)
            {
                if (it->state == FILEOP_STATE_PENDING)
                {
                    it->state = FILEOP_STATE_PROCESSING;
                    pending_jobs.push_back(it->id);
                }
            }
            CONFIG::UnlockFileOpList();

            for (uint64_t id : pending_jobs)
            {
                FileOpThreadArgs *args = new FileOpThreadArgs();
                args->id = id;
                pthread_t thread;
                pthread_create(&thread, NULL, FileOpSingleThread, args);
            }

            sleep(1);
        }

        return nullptr;
    }

    void StartFileOpThread()
    {
        if (bg_fileop_thread_started)
            return;

        bg_fileop_running.store(true);
        if (pthread_create(&fileop_thread, NULL, FileOpFilesThread, NULL) == 0)
        {
            bg_fileop_thread_started = true;
        }
        else
        {
            bg_fileop_running.store(false);
        }
    }

    void StopFileOpThread()
    {
        if (!bg_fileop_thread_started)
            return;

        bg_fileop_running.store(false);
        pthread_join(fileop_thread, NULL);
        bg_fileop_thread_started = false;
    }



RemoteClient *GetRemoteClientForSite(int site_idx) {
    if (site_idx < 0 || site_idx >= configured_sites.size()) return nullptr;
    RemoteSettings& s = configured_sites[site_idx];
    HostInfo host_info;
    host_info.type = s.type;
    host_info.url = s.server;
    host_info.username = s.username;
    host_info.password = s.password;
    host_info.http_server_type = s.http_server_type;
    return GetRemoteClient(&host_info);
}

bool StartExtractJob(int site_idx, const std::string& item, const std::string& destination, const std::string& folderName, std::string* error, uint64_t* job_id) {
    BgExtractData extract_data;
    extract_data.id = Util::GetTick();
    if (job_id) *job_id = extract_data.id;

    std::string safe_folderName = folderName;
    size_t pos = safe_folderName.find_last_of("/\\");
    if (pos != std::string::npos) {
        safe_folderName = safe_folderName.substr(pos + 1);
    }

    if (site_idx >= 0 && site_idx < configured_sites.size()) {
        RemoteSettings& s = configured_sites[site_idx];
        extract_data.host_info.type = s.type;
        extract_data.host_info.url = s.server;
        extract_data.host_info.username = s.username;
        extract_data.host_info.password = s.password;
        extract_data.host_info.http_server_type = s.http_server_type;
    } else {
        extract_data.host_info.type = 0;
        extract_data.host_info.url = "";
    }
    
    extract_data.host_info.client = nullptr;
    extract_data.src_path = item;
    extract_data.dest_path = destination;
    extract_data.folder_name = safe_folderName;
    extract_data.file_size = 0;
    extract_data.state = EXTRACT_STATE_PENDING;
    extract_data.bytes_transfered = 0;
    extract_data.timestamp = Util::GetTick();
    extract_data.finished_timestamp = 0;

    CONFIG::AddBgExtractData(extract_data);
    CONFIG::SaveBgExtractData();
    return true;
}

    void *ServerThread(void *argp)
    {
        auto serve_log_file = [&](const std::string& path, Response &res) {
            if (!FS::FileExists(path.c_str())) {
                res.status = 404;
                res.set_content("Log file not found", "text/plain");
                return;
            }
            FILE *in = FS::OpenRead(path.c_str());
            if (in == nullptr) {
                res.status = 500;
                res.set_content("Could not open log file", "text/plain");
                return;
            }
            size_t size = FS::GetSize(path.c_str());
            res.set_content_provider(
                size, "text/plain",
                [in](size_t offset, size_t length, DataSink &sink) {
                    size_t size_to_read = std::min(static_cast<size_t>(length), (size_t)1048576);
                    std::vector<char> buff(size_to_read);
                    size_t read_len;
                    FS::Seek(in, offset);
                    read_len = FS::Read(in, buff.data(), size_to_read);
                    if (read_len > 0)
                    {
                        sink.write(buff.data(), read_len);
                        return true;
                    }
                    sink.done();
                    return true;
                },
                [in](bool) {
                    FS::Close(in);
                });
        };

        svr->Get("/debug/client.log", [&](const Request &req, Response &res)
                 { serve_log_file("/data/homebrew/ezremote-client/client.log", res); });

        svr->Get("/debug/server.log", [&](const Request &req, Response &res)
                 { serve_log_file("/data/homebrew/ezremote-client/server.log", res); });

        svr->Get("/debug/log", [&](const Request &req, Response &res)
                 { res.set_redirect("/debug/client.log"); });

        svr->Get("/", [&](const Request &req, Response &res)
                 { res.set_redirect("/index.html"); });
                 
        extern void RegisterNewEndpoints(httplib::Server* svr);
        RegisterNewEndpoints(svr);

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
            bool is_dir_param = false;

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
                json_object *is_dir_obj = json_object_object_get(jobj, "is_dir");
                file_size_param = file_size_obj != nullptr ? json_object_get_uint64(file_size_obj) : 0;
                id_param = id_obj != nullptr ? json_object_get_uint64(id_obj) : Util::GetTick();
                is_dir_param = is_dir_obj != nullptr ? json_object_get_boolean(is_dir_obj) : false;

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
                download_data.completed_bytes = 0;
                download_data.timestamp = Util::GetTick();
                download_data.finished_timestamp = 0;
                download_data.is_dir = is_dir_param;

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

        svr->Post("/extract_url", [&](const Request &req, Response &res)
        {
            int type_param;
            const char *url_param;
            const char *username_param;
            const char *password_param;
            const char *http_server_type_param;
            const char *src_path_param;
            const char *dest_path_param;
            const char *folder_name_param;
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
                folder_name_param = json_object_get_string(json_object_object_get(jobj, "folder_name"));
                json_object *id_obj = json_object_object_get(jobj, "id");
                id_param = id_obj != nullptr ? json_object_get_uint64(id_obj) : Util::GetTick();

                if (src_path_param == nullptr || dest_path_param == nullptr || folder_name_param == nullptr)
                {
                    bad_request(res, "Required parameters are missing");
                    json_object_put(jobj);
                    return;
                }

                std::string safe_folder_name = folder_name_param;
                size_t pos = safe_folder_name.find_last_of("/\\");
                if (pos != std::string::npos) {
                    safe_folder_name = safe_folder_name.substr(pos + 1);
                }

                BgExtractData extract_data;
                extract_data.host_info.type = type_param;
                extract_data.host_info.url = url_param != nullptr ? url_param : "";
                extract_data.host_info.client = nullptr;
                extract_data.src_path = src_path_param;
                extract_data.dest_path = dest_path_param;
                extract_data.folder_name = safe_folder_name;
                extract_data.file_size = 0; // Not strictly tracked for extracts yet
                extract_data.state = EXTRACT_STATE_PENDING;
                extract_data.id = id_param;
                extract_data.bytes_transfered = 0;
                extract_data.timestamp = Util::GetTick();
                extract_data.finished_timestamp = 0;

                if (username_param != nullptr)
                    extract_data.host_info.username = username_param;
                if (password_param != nullptr)
                    extract_data.host_info.password = password_param;
                if (http_server_type_param != nullptr)
                    extract_data.host_info.http_server_type = http_server_type_param;

                CONFIG::AddBgExtractData(extract_data);
                CONFIG::SaveBgExtractData();
                success(res);
                json_object_put(jobj);
                return;
            }

            bad_request(res, "Invalid payload");
        });

        svr->Get("/get_download_state", [&](const Request &req, Response &res)
        {
            json_object *download_list = json_object_new_array();
            uint64_t now = Util::GetTick();

            CONFIG::LockDownloadList();
            for (auto it = bg_download_list.begin(); it != bg_download_list.end(); ++it)
            {
                json_object *download_item_obj = json_object_new_object();
                json_object_object_add(download_item_obj, "path", json_object_new_string(it->dest_path.c_str()));
                json_object_object_add(download_item_obj, "id", json_object_new_uint64(it->id));
                json_object_object_add(download_item_obj, "retry_count", json_object_new_int(it->retry_count));
                uint64_t total_transferred = it->bytes_transfered;
                if (it->is_dir) {
                    total_transferred += it->completed_bytes;
                }
                json_object_object_add(download_item_obj, "bytes_transfered", json_object_new_uint64(total_transferred));
                json_object_object_add(download_item_obj, "file_size", json_object_new_uint64(it->file_size));
                json_object_object_add(download_item_obj, "state", json_object_new_int(it->state));
                if (it->state == STATE_FAILED)
                    json_object_object_add(download_item_obj, "fail_reason", json_object_new_string(it->fail_reason.c_str()));
                json_object_object_add(download_item_obj, "timestamp", json_object_new_uint64(it->timestamp/1000000));
                json_object_object_add(download_item_obj, "finished_timestamp", json_object_new_uint64(it->finished_timestamp/1000000));
                bool terminal_state = it->state == STATE_FAILED || it->state == STATE_SUCCESS;
                uint64_t end_time = terminal_state ? (it->finished_timestamp > 0 ? it->finished_timestamp : it->timestamp) : now;
                json_object_object_add(download_item_obj, "elapsed_seconds", json_object_new_uint64(end_time > it->timestamp ? (end_time - it->timestamp) / 1000000 : 0));

                json_object *active_files = json_object_new_array();
                for (const auto& active_file : it->active_files) {
                    if (!active_file.empty()) {
                        json_object_array_add(active_files, json_object_new_string(active_file.c_str()));
                    }
                }
                json_object_object_add(download_item_obj, "active_files", active_files);
                json_object_array_add(download_list, download_item_obj);
            }
            CONFIG::UnlockDownloadList();
            
            const char *payload_str = json_object_to_json_string(download_list);

            res.status = 200;
            res.set_content(payload_str, "application/json");
            json_object_put(download_list);
        });

        svr->Get("/get_extract_state", [&](const Request &req, Response &res)
        {
            json_object *extract_list = json_object_new_array();
            uint64_t now = Util::GetTick();

            CONFIG::LockExtractList();
            for (auto it = bg_extract_list.begin(); it != bg_extract_list.end(); ++it)
            {
                json_object *extract_item_obj = json_object_new_object();
                uint64_t transferred = it->bytes_transfered;
                uint64_t total = it->file_size;
                if (it->state == EXTRACT_STATE_EXTRACTING)
                {
                    transferred = bytes_transfered;
                    total = bytes_to_download;
                }
                json_object_object_add(extract_item_obj, "path", json_object_new_string(it->src_path.c_str()));
                json_object_object_add(extract_item_obj, "id", json_object_new_uint64(it->id));
                json_object_object_add(extract_item_obj, "retry_count", json_object_new_int(it->retry_count));
                json_object_object_add(extract_item_obj, "dest_path", json_object_new_string((it->dest_path + "/" + it->folder_name).c_str()));
                json_object_object_add(extract_item_obj, "bytes_transfered", json_object_new_uint64(transferred));
                json_object_object_add(extract_item_obj, "file_size", json_object_new_uint64(total));
                json_object_object_add(extract_item_obj, "state", json_object_new_int(it->state));
                if (it->state == EXTRACT_STATE_FAILED)
                    json_object_object_add(extract_item_obj, "fail_reason", json_object_new_string(it->fail_reason.c_str()));
                json_object_object_add(extract_item_obj, "timestamp", json_object_new_uint64(it->timestamp/1000000));
                json_object_object_add(extract_item_obj, "finished_timestamp", json_object_new_uint64(it->finished_timestamp/1000000));
                bool terminal_state = it->state == EXTRACT_STATE_FAILED || it->state == EXTRACT_STATE_SUCCESS;
                uint64_t end_time = terminal_state ? (it->finished_timestamp > 0 ? it->finished_timestamp : it->timestamp) : now;
                json_object_object_add(extract_item_obj, "elapsed_seconds", json_object_new_uint64(end_time > it->timestamp ? (end_time - it->timestamp) / 1000000 : 0));
                json_object_array_add(extract_list, extract_item_obj);
            }
            CONFIG::UnlockExtractList();
            
            const char *payload_str = json_object_to_json_string(extract_list);

            res.status = 200;
            res.set_content(payload_str, "application/json");
            json_object_put(extract_list);
        });
        svr->Get("/get_fileop_state", [&](const Request &req, Response &res)
        {
            json_object *fileop_list = json_object_new_array();
            uint64_t now = Util::GetTick();

            CONFIG::LockFileOpList();
            for (auto it = bg_fileop_list.begin(); it != bg_fileop_list.end(); ++it)
            {
                json_object *fileop_item_obj = json_object_new_object();
                json_object_object_add(fileop_item_obj, "type", json_object_new_int(it->type));
                json_object_object_add(fileop_item_obj, "id", json_object_new_uint64(it->id));
                json_object_object_add(fileop_item_obj, "retry_count", json_object_new_int(it->retry_count));
                json_object_object_add(fileop_item_obj, "dest_path", json_object_new_string(it->dest_path.c_str()));
                json_object_object_add(fileop_item_obj, "items_processed", json_object_new_uint64(it->items_processed));
                json_object_object_add(fileop_item_obj, "total_items", json_object_new_uint64(it->total_items));
                json_object_object_add(fileop_item_obj, "state", json_object_new_int(it->state));
                if (it->state == FILEOP_STATE_FAILED)
                    json_object_object_add(fileop_item_obj, "fail_reason", json_object_new_string(it->fail_reason.c_str()));
                json_object_object_add(fileop_item_obj, "timestamp", json_object_new_uint64(it->timestamp/1000000));
                json_object_object_add(fileop_item_obj, "finished_timestamp", json_object_new_uint64(it->finished_timestamp/1000000));
                bool terminal_state = it->state == FILEOP_STATE_FAILED || it->state == FILEOP_STATE_SUCCESS;
                uint64_t end_time = terminal_state ? (it->finished_timestamp > 0 ? it->finished_timestamp : it->timestamp) : now;
                json_object_object_add(fileop_item_obj, "elapsed_seconds", json_object_new_uint64(end_time > it->timestamp ? (end_time - it->timestamp) / 1000000 : 0));
                
                json_object *items_arr = json_object_new_array();
                for (const auto& item : it->items) {
                    json_object_array_add(items_arr, json_object_new_string(item.c_str()));
                }
                json_object_object_add(fileop_item_obj, "items", items_arr);

                json_object_array_add(fileop_list, fileop_item_obj);
            }
            CONFIG::UnlockFileOpList();
            
            const char *payload_str = json_object_to_json_string(fileop_list);

            res.status = 200;
            res.set_content(payload_str, "application/json");
            json_object_put(fileop_list);
        });

#include "local_api.h"
        svr->Post("/fileop_start", [&](const Request &req, Response &res)
        {
            json_object *jobj = json_tokener_parse(req.body.c_str());
            if (jobj != nullptr)
            {
                int type_param = json_object_get_int(json_object_object_get(jobj, "type"));
                const char *dest_path_param = json_object_get_string(json_object_object_get(jobj, "newPath"));
                json_object *items_arr = json_object_object_get(jobj, "items");

                if (type_param < FILEOP_COPY || type_param > FILEOP_DELETE)
                {
                    bad_request(res, "Invalid file operation type");
                    json_object_put(jobj);
                    return;
                }

                if (!items_arr || json_object_get_type(items_arr) != json_type_array)
                {
                    bad_request(res, "Missing or invalid items array");
                    json_object_put(jobj);
                    return;
                }

                if ((type_param == FILEOP_COPY || type_param == FILEOP_MOVE) &&
                    (dest_path_param == nullptr || dest_path_param[0] == '\0'))
                {
                    bad_request(res, "Missing destination path");
                    json_object_put(jobj);
                    return;
                }

                BgFileOpData op_data;
                op_data.id = Util::GetTick();
                op_data.type = static_cast<FileOpType>(type_param);
                op_data.dest_path = dest_path_param ? dest_path_param : "";
                op_data.state = FILEOP_STATE_PENDING;
                op_data.items_processed = 0;
                op_data.timestamp = Util::GetTick();
                op_data.finished_timestamp = 0;

                struct array_list *arr = json_object_get_array(items_arr);
                op_data.total_items = arr->length;
                for (size_t i = 0; i < arr->length; i++) {
                    const char *item = json_object_get_string((json_object*)array_list_get_idx(arr, i));
                    if (item == nullptr || item[0] == '\0')
                    {
                        bad_request(res, "Invalid item path");
                        json_object_put(jobj);
                        return;
                    }
                    op_data.items.push_back(item);
                }

                CONFIG::AddBgFileOpData(op_data);
                CONFIG::SaveBgFileOpData();
                success(res);
                json_object_put(jobj);
                return;
            }

            bad_request(res, "Invalid payload");
        });

        svr->Post("/retry_task", [&](const Request &req, Response &res)
        {
            json_object *jobj = json_tokener_parse(req.body.c_str());
            if (!jobj)
            {
                bad_request(res, "Invalid JSON");
                return;
            }

            const char *type_param = json_object_get_string(json_object_object_get(jobj, "type"));
            uint64_t id_param = json_object_get_uint64(json_object_object_get(jobj, "id"));

            if (!type_param || id_param == 0)
            {
                bad_request(res, "Missing type or id");
                json_object_put(jobj);
                return;
            }

            std::string type = type_param;
            bool found = false;

            if (type == "download")
            {
                CONFIG::LockDownloadList();
                for (auto &it : bg_download_list)
                {
                    if (it.id == id_param && it.state == STATE_FAILED)
                    {
                        it.state = STATE_RESUMED;
                        it.retry_count = 0;
                        it.fail_reason = "";
                        it.finished_timestamp = 0;
                        found = true;
                        break;
                    }
                }
                CONFIG::UnlockDownloadList();
                if (found) CONFIG::SaveBgDownloadData();
            }
            else if (type == "extract")
            {
                CONFIG::LockExtractList();
                for (auto &it : bg_extract_list)
                {
                    if (it.id == id_param && it.state == EXTRACT_STATE_FAILED)
                    {
                        it.state = EXTRACT_STATE_PENDING;
                        it.retry_count = 0;
                        it.fail_reason = "";
                        it.bytes_transfered = 0;
                        it.finished_timestamp = 0;
                        std::string tmp_folder = it.dest_path + "/.extract_" + std::to_string(it.id);
                        if (FS::FolderExists(tmp_folder)) {
                            FS::RmRecursive(tmp_folder);
                        }
                        found = true;
                        break;
                    }
                }
                CONFIG::UnlockExtractList();
                if (found) CONFIG::SaveBgExtractData();
            }
            else if (type == "fileop")
            {
                CONFIG::LockFileOpList();
                for (auto &it : bg_fileop_list)
                {
                    if (it.id == id_param && it.state == FILEOP_STATE_FAILED)
                    {
                        it.state = FILEOP_STATE_PENDING;
                        it.retry_count = 0;
                        it.fail_reason = "";
                        it.finished_timestamp = 0;
                        found = true;
                        break;
                    }
                }
                CONFIG::UnlockFileOpList();
                if (found) CONFIG::SaveBgFileOpData();
            }

            if (found) {
                success(res);
            } else {
                bad_request(res, "Task not found or not in failed state");
            }
            json_object_put(jobj);
        });



        svr->Post("/clean_tasks", [&](const Request &req, Response &res)
        {
            CONFIG::LockDownloadList();
            bg_download_list.remove_if([](const BgDownloadData& item) {
                if (item.state == STATE_FAILED || item.state == STATE_SUCCESS) {
                    if (item.state == STATE_FAILED) {
                        std::string temp_file = item.dest_path + ".tmp";
                        if (FS::FileExists(temp_file)) FS::Rm(temp_file);
                    }
                    return true;
                }
                return false;
            });
            CONFIG::UnlockDownloadList();
            CONFIG::SaveBgDownloadData();

            CONFIG::LockExtractList();
            bg_extract_list.remove_if([](const BgExtractData& item) {
                if (item.state == EXTRACT_STATE_FAILED || item.state == EXTRACT_STATE_SUCCESS) {
                    if (item.state == EXTRACT_STATE_FAILED) {
                        std::string tmp_folder = item.dest_path + "/.extract_" + std::to_string(item.id);
                        if (FS::FolderExists(tmp_folder)) FS::RmRecursive(tmp_folder);
                    }
                    return true;
                }
                return false;
            });
            CONFIG::UnlockExtractList();
            CONFIG::SaveBgExtractData();

            CONFIG::LockFileOpList();
            bg_fileop_list.remove_if([](const BgFileOpData& item) {
                return (item.state == FILEOP_STATE_FAILED || item.state == FILEOP_STATE_SUCCESS);
            });
            CONFIG::UnlockFileOpList();
            CONFIG::SaveBgFileOpData();

            success(res);
        });

        svr->Post("/stop_task", [&](const Request &req, Response &res)
        {
            json_object *jobj = json_tokener_parse(req.body.c_str());
            if (!jobj) { bad_request(res, "Invalid JSON"); return; }

            json_object *id_obj, *type_obj;
            if (!json_object_object_get_ex(jobj, "id", &id_obj) ||
                !json_object_object_get_ex(jobj, "type", &type_obj))
            {
                json_object_put(jobj);
                bad_request(res, "Missing id or type");
                return;
            }

            uint64_t id_param = json_object_get_uint64(id_obj);
            const char *type_param = json_object_get_string(type_obj);
            if (type_param == nullptr || type_param[0] == '\0')
            {
                json_object_put(jobj);
                bad_request(res, "Missing type");
                return;
            }
            std::string type = type_param;
            bool found = false;

            if (type == "download")
            {
                CONFIG::LockDownloadList();
                for (auto &it : bg_download_list)
                {
                    if (it.id == id_param)
                    {
                        it.cancel_requested = true;
                        if (it.state == STATE_PENDING || it.state == STATE_RESUMED) {
                            it.state = STATE_FAILED;
                            it.fail_reason = "Cancelled by user";
                            it.finished_timestamp = Util::GetTick();
                        }
                        found = true;
                        break;
                    }
                }
                CONFIG::UnlockDownloadList();
                if (found) CONFIG::SaveBgDownloadData();
            }
            else if (type == "extract")
            {
                CONFIG::LockExtractList();
                for (auto &it : bg_extract_list)
                {
                    if (it.id == id_param)
                    {
                        it.cancel_requested = true;
                        if (it.state == EXTRACT_STATE_PENDING) {
                            it.state = EXTRACT_STATE_FAILED;
                            it.fail_reason = "Cancelled by user";
                            it.finished_timestamp = Util::GetTick();
                        }
                        found = true;
                        break;
                    }
                }
                CONFIG::UnlockExtractList();
                if (found) CONFIG::SaveBgExtractData();
            }
            else if (type == "fileop")
            {
                CONFIG::LockFileOpList();
                for (auto &it : bg_fileop_list)
                {
                    if (it.id == id_param)
                    {
                        it.cancel_requested = true;
                        if (it.state == FILEOP_STATE_PENDING) {
                            it.state = FILEOP_STATE_FAILED;
                            it.fail_reason = "Cancelled by user";
                            it.finished_timestamp = Util::GetTick();
                        }
                        found = true;
                        break;
                    }
                }
                CONFIG::UnlockFileOpList();
                if (found) CONFIG::SaveBgFileOpData();
            }

            if (found) {
                success(res);
            } else {
                bad_request(res, "Task not found");
            }
            json_object_put(jobj);
        });

        svr->Get("/get_destinations", [&](const Request &req, Response &res)
        {
            json_object *dest_list = json_object_new_array();
            
            auto add_dest = [&](const std::string& path) {
                json_object *dest_obj = json_object_new_object();
                json_object_object_add(dest_obj, "path", json_object_new_string(path.c_str()));
                
                struct statvfs stat;
                if (statvfs(path.c_str(), &stat) == 0) {
                    uint64_t free_space = (uint64_t)stat.f_bavail * (uint64_t)stat.f_frsize;
                    uint64_t total_space = (uint64_t)stat.f_blocks * (uint64_t)stat.f_frsize;
                    json_object_object_add(dest_obj, "free", json_object_new_uint64(free_space));
                    json_object_object_add(dest_obj, "total", json_object_new_uint64(total_space));
                }
                
                json_object_array_add(dest_list, dest_obj);
            };
            
            add_dest("/data/etaHEN/games");

            int err = 0;
            std::vector<DirEntry> mnt_dirs = FS::ListDir("/mnt", &err);
            for (const auto& entry : mnt_dirs) {
                if (entry.isDir) {
                    std::string name = entry.name;
                    if (name.find("ext") == 0 || name.find("usb") == 0) {
                        std::string path = "/mnt/" + name;
                        int err2 = 0;
                        std::vector<DirEntry> subdirs = FS::ListDir(path, &err2);
                        bool is_empty = true;
                        for (const auto& subentry : subdirs) {
                            if (strcmp(subentry.name, ".") != 0 && strcmp(subentry.name, "..") != 0 && strcmp(subentry.name, "System Volume Information") != 0) {
                                is_empty = false;
                                break;
                            }
                        }
                        if (!is_empty) {
                            add_dest(path);
                        }
                    }
                }
            }

            const char *payload_str = json_object_to_json_string(dest_list);
            res.status = 200;
            res.set_content(payload_str, "application/json");
            json_object_put(dest_list);
        });

        svr->Get("/__local__/restart_daemon", [&](const Request & /*req*/, Response & res) {
            set_cors_header(res);
            if (LaunchRestartClient()) {
                res.status = 200;
                res.set_content("{\"status\":\"success\"}", "application/json");
            } else {
                res.status = 500;
                res.set_content("{\"status\":\"error\",\"error\":\"Failed to launch restart helper\"}", "application/json");
            }
        });

        svr->Get("/stop", [&](const Request & /*req*/, Response & res)
        {
            set_cors_header(res);
            res.status = 200;
            res.set_content("{\"status\":\"success\"}", "application/json");
            svr->stop();
        });

        svr->Get("/version", [&](const Request & req, Response &res)
        {
            res.status = 200;
            char version[20];
            sprintf(version, "%.2f", EZREMOTE_VERSION);
            res.set_content(version, "text/html");
        });

        svr->Get("/mem", [&](const Request & req, Response &res)
        {
            res.status = 200;
            res.set_content(ProcessMemoryStatsJson(), "application/json");
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
            if (res.status != 200) {
                dbglogger_log("[ezremote-server] [%s] %s -> %d", req.method.c_str(), req.path.c_str(), res.status);
            }
            if (res.status >= 400 && !res.body.empty()) {
                dbglogger_log("[ezremote-server] Error body: %s", res.body.c_str());
            }
        });
       
        svr->set_payload_max_length(1024 * 1024 * 12);
        svr->set_tcp_nodelay(true);
        FS::MkDirs("/data/homebrew/ezremote-client/game-icons");
        svr->set_mount_point("/game-icons", "/data/homebrew/ezremote-client/game-icons");
        svr->set_mount_point("/", "/data/homebrew/ezremote-client/assets/");

        svr->Options(R"(.*)", [&](const Request &req, Response &res) {
            res.status = 200;
        });

        svr->set_post_routing_handler([](const Request &req, Response &res) {
            set_cors_header(res);
        });

        svr->listen("0.0.0.0", http_server_port);

        return NULL;
    }

    void Start()
    {
        if (svr == nullptr)
            svr = new Server();
        if (!svr->is_valid())
        {
            StopDownloadThread();
            StopExtractThread();
            StopFileOpThread();
            delete svr;
            svr = nullptr;
            return;
        }

        Util::Notify("Starting ezRemote Server %.2f on port %d", EZREMOTE_VERSION, http_server_port);
        ServerThread(nullptr);
        StopDownloadThread();
        StopExtractThread();
        StopFileOpThread();

        delete svr;
        svr = nullptr;
    }

    void Stop()
    {
        if (svr != nullptr)
            svr->stop();
        StopDownloadThread();
        StopExtractThread();
        StopFileOpThread();
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

    void StartExtractThread()
    {
        if (bg_extract_thread_started)
            return;

        bg_extract_running.store(true);
        if (pthread_create(&bg_extract_thread, NULL, ExtractFilesThread, NULL) == 0)
        {
            bg_extract_thread_started = true;
        }
        else
        {
            bg_extract_running.store(false);
        }
    }

    void StopExtractThread()
    {
        if (!bg_extract_thread_started)
            return;

        bg_extract_running.store(false);
        pthread_join(bg_extract_thread, NULL);
        bg_extract_thread_started = false;
    }
}

namespace HttpServer { void RegisterNewEndpoints(httplib::Server* svr) {
    svr->Post("/api/install_remote_pkg", [&](const httplib::Request &req, httplib::Response &res) {
        json_object *jobj = json_tokener_parse(req.body.c_str());
        if (!jobj) {
            char buf[256];
            sprintf(buf, FAILURE_MSG, "Invalid JSON");
            res.set_content(buf, "application/json");
            return;
        }

        const char *url = json_object_get_string(json_object_object_get(jobj, "url"));
        const char *path = json_object_get_string(json_object_object_get(jobj, "path"));
        
        if (!url || !path) {
            char buf[256];
            sprintf(buf, FAILURE_MSG, "Missing url or path");
            res.set_content(buf, "application/json");
            json_object_put(jobj);
            return;
        }

        RemoteSettings settings; // Dummy settings
        memset(&settings, 0, sizeof(settings));

        // Use a default HTTP client for now
        RemoteClient* client = nullptr; // Would be resolved based on URL/Settings
        if (client == nullptr) {
            char buf[256];
            sprintf(buf, FAILURE_MSG, "Remote install endpoint is not initialized");
            res.set_content(buf, "application/json");
            json_object_put(jobj);
            return;
        }

        bool started = g_pkg_installer.StartRemoteInstall(client, url, path, &settings);
        if (started) {
            res.set_content(SUCCESS_MSG, "application/json");
        } else {
            char buf[256];
            sprintf(buf, FAILURE_MSG, "Already installing");
            res.set_content(buf, "application/json");
        }
        json_object_put(jobj);
    });

    svr->Get("/api/get_install_progress", [&](const httplib::Request &req, httplib::Response &res) {
        Usecase::InstallProgress prog = g_pkg_installer.GetProgress();
        char buf[1024];
        sprintf(buf, "{\"status\": %d, \"message\": \"%s\", \"downloaded\": %lu, \"total\": %lu}", 
                prog.status, prog.activity_message.c_str(), prog.bytes_downloaded, prog.bytes_total);
        res.set_content(buf, "application/json");
    });
}
}
