#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <arpa/inet.h>
#include <ifaddrs.h>
#include <json-c/json.h>
#include "httpclient/HTTPClient.h"
#include "clients/remote_client.h"
#include "clients/ftpclient.h"
#include "clients/iis.h"
#include "dbglogger.h"

#include "server/http_server.h"
#include "util.h"
#include "config.h"
#include "windows.h"
#include "lang.h"
#include "fs.h"
#include "sfo.h"
#include "sceAppInstUtil.h"
#include "sceUserService.h"
#include "sceSystemService.h"
#include "installer.h"

struct BgProgressCheck
{
	ArchivePkgInstallData *archive_pkg_data;
	SplitPkgInstallData *split_pkg_data;
	std::string content_id;
	std::string hash;
	std::string url;
	std::string title;
	std::string icon_url;
};

static bool sceAppInst_done = false;

static std::map<std::string, ArchivePkgInstallData *> archive_pkg_install_data_list;
static std::map<std::string, SplitPkgInstallData *> split_pkg_install_data_list;

static std::string GetLocalIP() {
    struct ifaddrs *ifaddr, *ifa;
    if (getifaddrs(&ifaddr) == -1) return "localhost";
    
    std::string result = "localhost";
    for (ifa = ifaddr; ifa != NULL; ifa = ifa->ifa_next) {
        if (ifa->ifa_addr == NULL) continue;
        if (ifa->ifa_addr->sa_family == AF_INET) {
            if (strcmp(ifa->ifa_name, "lo") != 0 && strcmp(ifa->ifa_name, "lo0") != 0) {
                char ip[INET_ADDRSTRLEN];
                inet_ntop(AF_INET, &((struct sockaddr_in *)ifa->ifa_addr)->sin_addr, ip, INET_ADDRSTRLEN);
                result = std::string(ip);
                break;
            }
        }
    }
    freeifaddrs(ifaddr);
    return result;
}

namespace INSTALLER
{
    static int FtpCallback(int64_t xfered, void *arg)
    {
        return 1;
    }

	static bool IsSafeContentId(const std::string &content_id)
	{
		if (content_id.empty() || content_id.size() >= CONTENTID_SIZE)
			return false;

		for (char c : content_id)
		{
			if (!((c >= 'A' && c <= 'Z') ||
				  (c >= 'a' && c <= 'z') ||
				  (c >= '0' && c <= '9') ||
				  c == '-' || c == '_'))
			{
				return false;
			}
		}

		return true;
	}

	static std::string GetPkgContentId(pkg_header *header)
	{
		if (header == nullptr)
			return "";

		std::string content_id((const char *)header->pkg_content_id, sizeof(header->pkg_content_id));
		size_t nul_pos = content_id.find('\0');
		if (nul_pos != std::string::npos)
			content_id.resize(nul_pos);

		while (!content_id.empty() && content_id.back() == ' ')
			content_id.pop_back();

		return IsSafeContentId(content_id) ? content_id : "";
	}

	static std::string GetTitleIdFromContentId(const std::string &content_id)
	{
		size_t dash_pos = content_id.find('-');
		if (dash_pos == std::string::npos || content_id.size() < dash_pos + 10)
			return "";

		std::string title_id = content_id.substr(dash_pos + 1, 9);
		for (char c : title_id)
		{
			if (!((c >= 'A' && c <= 'Z') ||
				  (c >= 'a' && c <= 'z') ||
				  (c >= '0' && c <= '9')))
			{
				return "";
			}
		}

		return title_id;
	}

	static bool StartsWith(const std::string &value, const char *prefix)
	{
		return value.rfind(prefix, 0) == 0;
	}

	static std::string SanitizeDpiField(const std::string &value)
	{
		std::string sanitized;
		sanitized.reserve(value.size());
		for (char c : value)
		{
			unsigned char ch = (unsigned char)c;
			if (c == '|' || ch < 0x20 || ch == 0x7f)
				sanitized.push_back(' ');
			else
				sanitized.push_back(c);
		}

		if (sanitized.size() > 512)
			sanitized.resize(512);

		return sanitized;
	}

	static std::string ValidateIconUrl(const std::string &icon_url)
	{
		if (icon_url.empty() || icon_url.size() > 2048)
			return "";

		for (char c : icon_url)
		{
			unsigned char ch = (unsigned char)c;
			if (c == '|' || ch <= 0x20 || ch == 0x7f)
				return "";
		}

		size_t scheme_len = 0;
		if (StartsWith(icon_url, "http://"))
			scheme_len = 7;
		else if (StartsWith(icon_url, "https://"))
			scheme_len = 8;
		else
			return "";

		size_t slash_pos = icon_url.find('/', scheme_len);
		size_t host_len = (slash_pos == std::string::npos ? icon_url.size() : slash_pos) - scheme_len;
		if (host_len == 0)
			return "";

		return icon_url;
	}

	struct DirectInstallRangeCheck
	{
		CHTTPClient::HttpResponse *response;
		uint64_t offset;
		uint64_t size;
		uint64_t written;
		bool checked;
		bool valid;
	};

	static bool IsHttpInstallUrl(const std::string &url)
	{
		return StartsWith(url, "http://") || StartsWith(url, "https://");
	}

	static bool HasInlineUrlCredentials(const std::string &url)
	{
		size_t scheme_pos = url.find("://");
		if (scheme_pos == std::string::npos)
			return false;

		size_t authority_start = scheme_pos + 3;
		size_t authority_end = url.find('/', authority_start);
		std::string authority = url.substr(authority_start, authority_end == std::string::npos ? std::string::npos : authority_end - authority_start);
		return authority.find('@') != std::string::npos;
	}

	bool IsSafeDirectInstallUrl(const std::string &url)
	{
		if (url.empty() || url.size() > 8192 || !IsHttpInstallUrl(url) || HasInlineUrlCredentials(url))
			return false;

		size_t scheme_len = StartsWith(url, "https://") ? 8 : 7;
		size_t slash_pos = url.find('/', scheme_len);
		size_t host_len = (slash_pos == std::string::npos ? url.size() : slash_pos) - scheme_len;
		if (host_len == 0)
			return false;

		// Check for raw unescaped unsafe characters
		for (char c : url)
		{
			unsigned char ch = (unsigned char)c;
			if (c == '|' || c == '[' || c == ']' || c == '(' || c == ')' || c == '{' || c == '}' || ch <= 0x20 || ch == 0x7f)
				return false;
		}

		// The PS5 package installer daemon chokes if the physical file saved on disk
		// contains %, +, or other special characters. bgft derives the filename
		// from the last segment of the URL.
		// Enforce a strict whitelist for the filename segment so it safely falls back to proxy otherwise.
		size_t last_slash = url.find_last_of('/');
		if (last_slash != std::string::npos)
		{
			std::string filename = url.substr(last_slash + 1);
			// Remove query parameters if any exist before checking
			size_t query_pos = filename.find('?');
			if (query_pos != std::string::npos)
				filename = filename.substr(0, query_pos);

			for (char c : filename)
			{
				if (!((c >= 'A' && c <= 'Z') ||
					  (c >= 'a' && c <= 'z') ||
					  (c >= '0' && c <= '9') ||
					  c == '-' || c == '_' || c == '.'))
				{
					return false;
				}
			}
		}

		return true;
	}

	static bool ExpectedDirectContentRange(const CHTTPClient::HttpResponse &res, uint64_t offset, uint64_t size)
	{
		auto it = res.mapHeadersLowercase.find("content-range");
		if (it == res.mapHeadersLowercase.end())
			return false;

		std::string expected = "bytes " + std::to_string(offset) + "-" + std::to_string(offset + size - 1) + "/";
		if (it->second.compare(0, expected.length(), expected) != 0)
			return false;

		std::string total = it->second.substr(expected.length());
		return !total.empty() && total != "*" && strtoull(total.c_str(), nullptr, 10) >= size;
	}

	static size_t DirectInstallRangeCallback(void *data, size_t size, size_t nmemb, void *user_data)
	{
		DirectInstallRangeCheck *check = reinterpret_cast<DirectInstallRangeCheck *>(user_data);
		size_t bytes = size * nmemb;

		if (!check->checked)
		{
			check->valid = ExpectedDirectContentRange(*check->response, check->offset, check->size);
			check->checked = true;
		}

		if (!check->valid || check->written + bytes > check->size)
			return 0;

		check->written += bytes;
		return bytes;
	}

	bool CanDirectDownloadUrl(const std::string &url)
	{
		CHTTPClient::HttpResponse res;
		CHTTPClient::HeadersMap headers;
		CHTTPClient tmp_client([](const std::string& log){});
		tmp_client.InitSession(true, CHTTPClient::SettingsFlag::NO_FLAGS);
		tmp_client.SetCertificateFile(CACERT_FILE);
		tmp_client.SetTimeout(10);
		tmp_client.SetBufferSize(4096L);

		headers["Range"] = "bytes=0-1";
		DirectInstallRangeCheck check = {&res, 0, 2, 0, false, false};
		if (!tmp_client.Get(url, headers, res, (void *)&DirectInstallRangeCallback, (void *)&check))
			return false;

		return res.iCode == 206 && check.valid && check.written == check.size;
	}

	static std::string GetPassthroughInstallUrl(const std::string &path, std::string &direct_url_out)
	{
		if (remote_settings == nullptr || remoteclient == nullptr)
			return "";

		if (strlen(remote_settings->username) > 0 || strlen(remote_settings->password) > 0)
			return "";

		std::string direct_url = remoteclient->GetDirectUrl(path);
		if (direct_url.empty())
			return "";

		if (!CanDirectDownloadUrl(direct_url))
			return "";

		direct_url_out = direct_url;
		return direct_url;
	}

	static const char *PkgMagicName(uint32_t magic)
	{
		switch (magic)
		{
		case PS4_PKG_MAGIC:
			return "PS4";
		case PS5_PKG_MAGIC:
			return "PS5";
		default:
			return "unknown";
		}
	}

	static std::string HexString(const unsigned char *data, size_t size)
	{
		static const char hex[] = "0123456789abcdef";
		std::string out;
		out.reserve(size * 2);
		for (size_t i = 0; i < size; i++)
		{
			out.push_back(hex[(data[i] >> 4) & 0x0f]);
			out.push_back(hex[data[i] & 0x0f]);
		}
		return out;
	}

	static std::string GetRawPkgContentId(pkg_header *header)
	{
		if (header == nullptr)
			return "";

		std::string content_id((const char *)header->pkg_content_id, sizeof(header->pkg_content_id));
		size_t nul_pos = content_id.find('\0');
		if (nul_pos != std::string::npos)
			content_id.resize(nul_pos);

		while (!content_id.empty() && content_id.back() == ' ')
			content_id.pop_back();

		return SanitizeDpiField(content_id);
	}

	static bool FindParamSfoEntry(const void *entry_table_data, size_t entry_count, uint32_t &param_sfo_offset, uint32_t &param_sfo_size)
	{
		pkg_table_entry *entries = (pkg_table_entry *)entry_table_data;
		for (size_t i = 0; i < entry_count; ++i)
		{
			if (BE32(entries[i].id) == PKG_ENTRY_ID_PARAM_SFO)
			{
				param_sfo_offset = BE32(entries[i].offset);
				param_sfo_size = BE32(entries[i].size);
				return param_sfo_offset > 0 && param_sfo_size > 0 && param_sfo_size <= 1048576;
			}
		}

		return false;
	}

	static bool GetPkgEntryTableInfo(pkg_header *header, size_t &entry_count, uint32_t &entry_table_offset, uint64_t &entry_table_size)
	{
		if (header == nullptr)
			return false;

		uint32_t magic = BE32(header->pkg_magic);
		if (magic != PS4_PKG_MAGIC && magic != PS5_PKG_MAGIC)
			return false;

		entry_count = BE32(header->pkg_entry_count);
		entry_table_offset = BE32(header->pkg_table_offset);
		entry_table_size = entry_count * sizeof(pkg_table_entry);
		return entry_count > 0 && entry_table_offset > 0 && entry_table_size > 0 && entry_table_size <= 16777216;
	}

	static bool ReadLocalPkgSfoParams(const std::string &path, pkg_header *header, std::map<std::string, std::string> &params)
	{
		size_t entry_count;
		uint32_t entry_table_offset;
		uint64_t entry_table_size;
		if (!GetPkgEntryTableInfo(header, entry_count, entry_table_offset, entry_table_size))
			return false;

		FILE *fd = FS::OpenRead(path);
		if (fd == nullptr)
			return false;

		void *entry_table_data = malloc(entry_table_size);
		if (entry_table_data == nullptr)
		{
			FS::Close(fd);
			return false;
		}

		bool ok = false;
		FS::Seek(fd, entry_table_offset);
		if (FS::Read(fd, entry_table_data, (uint32_t)entry_table_size) == (int)entry_table_size)
		{
			uint32_t param_sfo_offset = 0;
			uint32_t param_sfo_size = 0;
			if (FindParamSfoEntry(entry_table_data, entry_count, param_sfo_offset, param_sfo_size))
			{
				char *param_sfo_data = (char *)malloc(param_sfo_size + 1);
				if (param_sfo_data != nullptr)
				{
					FS::Seek(fd, param_sfo_offset);
					if (FS::Read(fd, param_sfo_data, param_sfo_size) == (int)param_sfo_size)
					{
						param_sfo_data[param_sfo_size] = 0;
						params = SFO::GetParams(param_sfo_data, param_sfo_size);
						ok = !params.empty();
					}
					free(param_sfo_data);
				}
			}
		}

		free(entry_table_data);
		FS::Close(fd);
		return ok;
	}

	static bool ReadRemotePkgSfoParams(RemoteClient *client, const std::string &path, pkg_header *header, std::map<std::string, std::string> &params)
	{
		if (client == nullptr || path.empty())
			return false;

		size_t entry_count;
		uint32_t entry_table_offset;
		uint64_t entry_table_size;
		if (!GetPkgEntryTableInfo(header, entry_count, entry_table_offset, entry_table_size))
			return false;

		void *entry_table_data = malloc(entry_table_size);
		if (entry_table_data == nullptr)
			return false;

		bool ok = false;
		if (client->GetRange(path, entry_table_data, entry_table_size, entry_table_offset))
		{
			uint32_t param_sfo_offset = 0;
			uint32_t param_sfo_size = 0;
			if (FindParamSfoEntry(entry_table_data, entry_count, param_sfo_offset, param_sfo_size))
			{
				char *param_sfo_data = (char *)malloc(param_sfo_size + 1);
				if (param_sfo_data != nullptr)
				{
					if (client->GetRange(path, param_sfo_data, param_sfo_size, param_sfo_offset))
					{
						param_sfo_data[param_sfo_size] = 0;
						params = SFO::GetParams(param_sfo_data, param_sfo_size);
						ok = !params.empty();
					}
					free(param_sfo_data);
				}
			}
		}

		free(entry_table_data);
		return ok;
	}

	static bool ReadSplitPkgSfoParams(SplitFile *split_file, pkg_header *header, std::map<std::string, std::string> &params)
	{
		if (split_file == nullptr)
			return false;

		size_t entry_count;
		uint32_t entry_table_offset;
		uint64_t entry_table_size;
		if (!GetPkgEntryTableInfo(header, entry_count, entry_table_offset, entry_table_size))
			return false;

		void *entry_table_data = malloc(entry_table_size);
		if (entry_table_data == nullptr)
			return false;

		bool ok = false;
		if (split_file->Read((char *)entry_table_data, entry_table_size, entry_table_offset) == static_cast<ssize_t>(entry_table_size))
		{
			uint32_t param_sfo_offset = 0;
			uint32_t param_sfo_size = 0;
			if (FindParamSfoEntry(entry_table_data, entry_count, param_sfo_offset, param_sfo_size))
			{
				char *param_sfo_data = (char *)malloc(param_sfo_size + 1);
				if (param_sfo_data != nullptr)
				{
					if (split_file->Read(param_sfo_data, param_sfo_size, param_sfo_offset) == static_cast<ssize_t>(param_sfo_size))
					{
						param_sfo_data[param_sfo_size] = 0;
						params = SFO::GetParams(param_sfo_data, param_sfo_size);
						ok = !params.empty();
					}
					free(param_sfo_data);
				}
			}
		}

		free(entry_table_data);
		return ok;
	}

	static void LogPkgInstallMetadata(const char *source, const std::string &source_path, const std::string &install_uri,
		pkg_header *header, const std::string &display_title, const std::string &icon_url,
		const std::string &dpi_content_id, const std::map<std::string, std::string> &sfo_params)
	{
		if (header == nullptr)
			return;

		uint32_t magic = BE32(header->pkg_magic);
		std::string safe_source_path = SanitizeDpiField(source_path);
		std::string safe_install_uri = SanitizeDpiField(install_uri);
		std::string safe_title = SanitizeDpiField(display_title);
		std::string safe_icon_url = SanitizeDpiField(icon_url);
		std::string safe_dpi_content_id = SanitizeDpiField(dpi_content_id);
		std::string raw_content_id = GetRawPkgContentId(header);
		std::string safe_content_id = GetPkgContentId(header);
		std::string title_id = GetTitleIdFromContentId(safe_content_id);

		dbglogger_printf("[Installer][PKG Metadata] ===== BEGIN =====\n");
		dbglogger_printf("[Installer][PKG Metadata] source=%s\n", source != nullptr ? source : "");
		dbglogger_printf("[Installer][PKG Metadata] source_path=%s\n", safe_source_path.c_str());
		dbglogger_printf("[Installer][PKG Metadata] install_uri=%s\n", safe_install_uri.c_str());
		dbglogger_printf("[Installer][PKG Metadata] display_title=%s\n", safe_title.c_str());
		dbglogger_printf("[Installer][PKG Metadata] icon_url=%s\n", safe_icon_url.c_str());
		dbglogger_printf("[Installer][PKG Metadata] dpi_content_id=%s\n", safe_dpi_content_id.c_str());
		dbglogger_printf("[Installer][PKG Metadata] raw_header_content_id=%s\n", raw_content_id.c_str());
		dbglogger_printf("[Installer][PKG Metadata] safe_header_content_id=%s\n", safe_content_id.c_str());
		dbglogger_printf("[Installer][PKG Metadata] derived_title_id=%s\n", title_id.c_str());

		dbglogger_printf("[Installer][PKG Metadata][Header] pkg_magic=0x%08X (%s)\n", (unsigned int)magic, PkgMagicName(magic));
		dbglogger_printf("[Installer][PKG Metadata][Header] pkg_type=0x%08X pkg_0x008=0x%08X file_count=%u entry_count=%u sc_entry_count=%u entry_count_2=%u\n",
			(unsigned int)BE32(header->pkg_type), (unsigned int)BE32(header->pkg_0x008),
			(unsigned int)BE32(header->pkg_file_count), (unsigned int)BE32(header->pkg_entry_count),
			(unsigned int)BE16(header->pkg_sc_entry_count), (unsigned int)BE16(header->pkg_entry_count_2));
		dbglogger_printf("[Installer][PKG Metadata][Header] table_offset=%u entry_data_size=%u body_offset=%llu body_size=%llu content_offset=%llu content_size=%llu\n",
			(unsigned int)BE32(header->pkg_table_offset), (unsigned int)BE32(header->pkg_entry_data_size),
			(unsigned long long)BE64(header->pkg_body_offset), (unsigned long long)BE64(header->pkg_body_size),
			(unsigned long long)BE64(header->pkg_content_offset), (unsigned long long)BE64(header->pkg_content_size));
		dbglogger_printf("[Installer][PKG Metadata][Header] drm_type=0x%08X content_type=0x%08X content_flags=0x%08X promote_size=%u version_date=0x%08X version_hash=0x%08X\n",
			(unsigned int)BE32(header->pkg_drm_type), (unsigned int)BE32(header->pkg_content_type),
			(unsigned int)BE32(header->pkg_content_flags), (unsigned int)BE32(header->pkg_promote_size),
			(unsigned int)BE32(header->pkg_version_date), (unsigned int)BE32(header->pkg_version_hash));
		dbglogger_printf("[Installer][PKG Metadata][Header] pkg_0x088=0x%08X pkg_0x08C=0x%08X pkg_0x090=0x%08X pkg_0x094=0x%08X iro_tag=0x%08X drm_type_version=0x%08X\n",
			(unsigned int)BE32(header->pkg_0x088), (unsigned int)BE32(header->pkg_0x08C),
			(unsigned int)BE32(header->pkg_0x090), (unsigned int)BE32(header->pkg_0x094),
			(unsigned int)BE32(header->pkg_iro_tag), (unsigned int)BE32(header->pkg_drm_type_version));
		dbglogger_printf("[Installer][PKG Metadata][Header] pfs_image_count=%u pfs_image_flags=0x%016llX pfs_image_offset=%llu pfs_image_size=%llu mount_image_offset=%llu mount_image_size=%llu\n",
			(unsigned int)BE32(header->pfs_image_count), (unsigned long long)BE64(header->pfs_image_flags),
			(unsigned long long)BE64(header->pfs_image_offset), (unsigned long long)BE64(header->pfs_image_size),
			(unsigned long long)BE64(header->mount_image_offset), (unsigned long long)BE64(header->mount_image_size));
		dbglogger_printf("[Installer][PKG Metadata][Header] pkg_size=%llu pfs_signed_size=%u pfs_cache_size=%u pfs_split_size_nth_0=%llu pfs_split_size_nth_1=%llu\n",
			(unsigned long long)BE64(header->pkg_size), (unsigned int)BE32(header->pfs_signed_size),
			(unsigned int)BE32(header->pfs_cache_size), (unsigned long long)BE64(header->pfs_split_size_nth_0),
			(unsigned long long)BE64(header->pfs_split_size_nth_1));
		dbglogger_printf("[Installer][PKG Metadata][Digest] digest_entries1=%s\n", HexString(header->digest_entries1, sizeof(header->digest_entries1)).c_str());
		dbglogger_printf("[Installer][PKG Metadata][Digest] digest_entries2=%s\n", HexString(header->digest_entries2, sizeof(header->digest_entries2)).c_str());
		dbglogger_printf("[Installer][PKG Metadata][Digest] digest_table_digest=%s\n", HexString(header->digest_table_digest, sizeof(header->digest_table_digest)).c_str());
		dbglogger_printf("[Installer][PKG Metadata][Digest] digest_body_digest=%s\n", HexString(header->digest_body_digest, sizeof(header->digest_body_digest)).c_str());
		dbglogger_printf("[Installer][PKG Metadata][Digest] pfs_image_digest=%s\n", HexString(header->pfs_image_digest, sizeof(header->pfs_image_digest)).c_str());
		dbglogger_printf("[Installer][PKG Metadata][Digest] pfs_signed_digest=%s\n", HexString(header->pfs_signed_digest, sizeof(header->pfs_signed_digest)).c_str());
		dbglogger_printf("[Installer][PKG Metadata][Digest] pkg_digest=%s\n", HexString(header->pkg_digest, sizeof(header->pkg_digest)).c_str());

		if (sfo_params.empty())
		{
			dbglogger_printf("[Installer][PKG Metadata][SFO] unavailable\n");
		}
		else
		{
			for (auto it = sfo_params.begin(); it != sfo_params.end(); ++it)
			{
				dbglogger_printf("[Installer][PKG Metadata][SFO] %s=%s\n", SanitizeDpiField(it->first).c_str(), SanitizeDpiField(it->second).c_str());
			}
		}
		dbglogger_printf("[Installer][PKG Metadata] ===== END =====\n");
	}

	int Init(void)
	{
		int ret;

		if (sceAppInst_done)
		{
			return 0;
		}

		ret = sceAppInstUtilInitialize();
		if (ret)
		{
			return -1;
		}

		sceAppInst_done = true;

		return 0;
	}

	void Exit(void)
	{
	}

	std::string GetRemotePkgTitle(RemoteClient *client, const std::string &path, pkg_header *header)
	{
		if (BE32(header->pkg_magic) != PS4_PKG_MAGIC && BE32(header->pkg_magic) != PS5_PKG_MAGIC)
		{
			return GetPkgContentId(header);
		}

		size_t entry_count = BE32(header->pkg_entry_count);
		uint32_t entry_table_offset = BE32(header->pkg_table_offset);
		uint64_t entry_table_size = entry_count * sizeof(pkg_table_entry);
		void *entry_table_data = malloc(entry_table_size);

		int ret = client->GetRange(path, entry_table_data, entry_table_size, entry_table_offset);
		if (ret == 0)
		{
			free(entry_table_data);
			return "";
		}

		pkg_table_entry *entries = (pkg_table_entry *)entry_table_data;
		void *param_sfo_data = nullptr;
		uint32_t param_sfo_offset = 0;
		uint32_t param_sfo_size = 0;
		for (size_t i = 0; i < entry_count; ++i)
		{
			if (BE32(entries[i].id) == PKG_ENTRY_ID_PARAM_SFO)
			{
				param_sfo_offset = BE32(entries[i].offset);
				param_sfo_size = BE32(entries[i].size);
				break;
			}
		}
		free(entry_table_data);

		std::string title;
		if (param_sfo_offset > 0 && param_sfo_size > 0)
		{
			param_sfo_data = malloc(param_sfo_size);
			int ret = client->GetRange(path, param_sfo_data, param_sfo_size, param_sfo_offset);
			if (ret)
			{
				const char *tmp_title = SFO::GetString((const char *)param_sfo_data, param_sfo_size, "TITLE");
				if (tmp_title != nullptr)
					title = std::string(tmp_title);
			}
			free(param_sfo_data);
		}

		return title;
	}
 
	std::string GetLocalPkgTitle(const std::string &path, pkg_header *header)
	{
		size_t entry_count = BE32(header->pkg_entry_count);
		uint32_t entry_table_offset = BE32(header->pkg_table_offset);
		uint64_t entry_table_size = entry_count * sizeof(pkg_table_entry);
		void *entry_table_data = malloc(entry_table_size);
		if (entry_table_data == nullptr)
			return "";

		FILE *fd = FS::OpenRead(path);
		if (fd == nullptr)
		{
			free(entry_table_data);
			return "";
		}
		FS::Seek(fd, entry_table_offset);
		FS::Read(fd, entry_table_data, entry_table_size);

		pkg_table_entry *entries = (pkg_table_entry *)entry_table_data;
		void *param_sfo_data = NULL;
		uint32_t param_sfo_offset = 0;
		uint32_t param_sfo_size = 0;
		void *icon0_png_data = NULL;
		uint32_t icon0_png_offset = 0;
		uint32_t icon0_png_size = 0;
		for (size_t i = 0; i < entry_count; ++i)
		{
			if (BE32(entries[i].id) == PKG_ENTRY_ID_PARAM_SFO)
			{
				param_sfo_offset = BE32(entries[i].offset);
				param_sfo_size = BE32(entries[i].size);
				break;
			}
		}
		free(entry_table_data);

		std::string title;
		if (param_sfo_offset > 0 && param_sfo_size > 0)
		{
			param_sfo_data = malloc(param_sfo_size);
			FS::Seek(fd, param_sfo_offset);
			FS::Read(fd, param_sfo_data, param_sfo_size);
			const char *tmp_title = SFO::GetString((const char *)param_sfo_data, param_sfo_size, "TITLE");
			if (tmp_title != nullptr)
				title = std::string(tmp_title);
			free(param_sfo_data);
		}

		FS::Close(fd);
		return title;
	}

	std::string StoreBgInstallHostData(RemoteSettings *settings, const std::string &path, const std::string &direct_url = "")
	{
		std::string hash = Util::UrlHash(settings->server + path + settings->username + settings->password + std::to_string(settings->type));
		uint64_t file_size = 0;
		if (remoteclient != nullptr)
			remoteclient->Size(path, &file_size);

		json_object *history_item_obj = json_object_new_object();
		json_object_object_add(history_item_obj, "hash", json_object_new_string(hash.c_str()));
		json_object_object_add(history_item_obj, "url", json_object_new_string(settings->server));
		json_object_object_add(history_item_obj, "path", json_object_new_string(path.c_str()));
		json_object_object_add(history_item_obj, "username", json_object_new_string(settings->username));
		json_object_object_add(history_item_obj, "password", json_object_new_string(settings->password));
		json_object_object_add(history_item_obj, "type", json_object_new_int(settings->type));
		json_object_object_add(history_item_obj, "size", json_object_new_uint64(file_size));
		if (!direct_url.empty()) {
			json_object_object_add(history_item_obj, "direct_url", json_object_new_string(direct_url.c_str()));
		}

		if (settings->type == CLIENT_TYPE_HTTP_SERVER)
		{
			json_object_object_add(history_item_obj, "http_server_type", json_object_new_string(settings->http_server_type));
		}

		const char *params_str = json_object_to_json_string(history_item_obj);
		
		CHTTPClient::HttpResponse res;
		CHTTPClient::HeadersMap headers;
		CHTTPClient tmp_client([](const std::string& log){});
		tmp_client.InitSession(true, CHTTPClient::SettingsFlag::NO_FLAGS);
		tmp_client.SetCertificateFile(CACERT_FILE);
		headers["Content-Type"] = "application/json";

		std::string store_bg_install_data_url = std::string("http://localhost:") + std::to_string(http_int_server_port) + "/store_bg_install_data";
		if (tmp_client.Post(store_bg_install_data_url, headers, params_str, res))
		{
			if (HTTP_SUCCESS(res.iCode))
			{
	  		}
			else
			{
				json_object_put(history_item_obj);
				return "";
			}
		}
		else
		{
			json_object_put(history_item_obj);
			return "";
		}
		json_object_put(history_item_obj);
		return hash;
	}

	std::string getRemoteUrl(RemoteSettings* settings, const std::string path, bool encodeUrl)
	{
		(void)encodeUrl;
		std::string direct_url_out;
		std::string direct_url = GetPassthroughInstallUrl(path, direct_url_out);
		
		if (!direct_url.empty() && IsSafeDirectInstallUrl(direct_url))
			return direct_url;

		std::string hash = StoreBgInstallHostData(settings, path, direct_url_out);
		if (hash.empty())
			return "";

		if (enable_direct_download_redirect)
		{
			if (!direct_url_out.empty() && !IsSafeDirectInstallUrl(direct_url_out))
			{
				std::string full_url = std::string("http://") + GetLocalIP() + ":" + std::to_string(http_int_server_port) + "/bg_redirect/" + hash;
				return full_url;
			}
		}

		std::string full_url = std::string("http://") + GetLocalIP() + ":" + std::to_string(http_int_server_port) + "/bg_install/" + hash;
		return full_url;
	}

	void *CleanArchivePkgDataThread(void *argp)
	{
    dbglogger_log("Thread CleanArchivePkgDataThread started.");
    pthread_detach(pthread_self());
		ArchivePkgInstallData *archive_pkg_data = (ArchivePkgInstallData*)argp;
		archive_pkg_data->stop_write_thread = true;
		if (!archive_pkg_data->split_file->IsClosed())
			archive_pkg_data->split_file->Close();
		pthread_join(archive_pkg_data->thread, NULL);
		RemoteClient *archive_client = nullptr;
		if (archive_pkg_data->delete_client && archive_pkg_data->archive_entry != nullptr && archive_pkg_data->archive_entry->client_data != nullptr)
			archive_client = archive_pkg_data->archive_entry->client_data->client;
		if (archive_pkg_data->archive_entry != nullptr)
		{
			if (archive_pkg_data->archive_entry->archive != nullptr)
				archive_read_free(archive_pkg_data->archive_entry->archive);
			delete archive_pkg_data->archive_entry;
		}
		if (archive_client != nullptr)
		{
			archive_client->Quit();
			delete archive_client;
		}
		delete (archive_pkg_data->split_file);
		delete archive_pkg_data;
		return nullptr;
	}

	void *CleanSplitPkgDataThread(void *argp)
	{
    dbglogger_log("Thread CleanSplitPkgDataThread started.");
    pthread_detach(pthread_self());
		SplitPkgInstallData *split_pkg_data = (SplitPkgInstallData*)argp;
		if (!split_pkg_data->split_file->IsClosed())
			split_pkg_data->split_file->Close();
		pthread_join(split_pkg_data->thread, NULL);
		delete (split_pkg_data->split_file);
		if (split_pkg_data->delete_client)
			delete (split_pkg_data->remote_client);
		delete split_pkg_data;
		return nullptr;
	}

	void *CheckBgInstallTaskThread(void *argp)
	{
    dbglogger_log("Thread CheckBgInstallTaskThread started.");
    pthread_detach(pthread_self());
		bool completed = false;
		BgProgressCheck *bg_check_data = (BgProgressCheck *)argp;
		int ret;

		PlayGoInfo playgo_info;
		SceAppInstallPkgInfo pkg_info;
		memset(&playgo_info, 0, sizeof(playgo_info));
		
		for (size_t i = 0; i < SCE_NUM_LANGUAGES; i++) {
			strncpy(playgo_info.languages[i], "", sizeof(language_t) - 1);
		}	

		for (size_t i = 0; i < SCE_NUM_IDS; i++) {
			strncpy(playgo_info.playgo_scenario_ids[i], "", sizeof(playgo_scenario_id_t) - 1);
			strncpy(*playgo_info.content_ids, "", sizeof(content_id_t) - 1);
		}	

		MetaInfo metainfo = (MetaInfo){
			.uri = bg_check_data->url.c_str(),
			.ex_uri = "",
			.playgo_scenario_id = "",
			.content_id = "",
			.content_name = bg_check_data->title.c_str(),
			.icon_url = ""
		};

		SceAppInstallStatusInstalled progress_info{};
		dbglogger_printf("[Installer] Thread %p (Local PKG) started installing %s\n", (void*)pthread_self(), bg_check_data->title.c_str());
		ret = InstallWithDirectPackageInstaller(bg_check_data->url, bg_check_data->title, bg_check_data->icon_url, bg_check_data->content_id);
		if (ret != 0)
		{
			dbglogger_printf("[Installer] Thread %p (Local PKG) InstallWithDirectPackageInstaller failed (ret = %d)\n", (void*)pthread_self(), ret);
			goto finish;
		}

		while (strcmp(progress_info.status, "playable") != 0 && strcmp(progress_info.status, "none") != 0 )
		{
			ret = sceAppInstUtilGetInstallStatus(bg_check_data->content_id.c_str(), &progress_info);
			if (ret || (progress_info.error_info.error_code != 0))
			goto finish;

			bytes_to_download = progress_info.total_size;
			bytes_transfered = progress_info.downloaded_size;
			sceSystemServicePowerTick();
			usleep(500000);
		}

		dbglogger_printf("[Installer] Thread %p (Local PKG) wait finished with status %s\n", (void*)pthread_self(), progress_info.status);

	finish:
		if (bg_check_data->archive_pkg_data != nullptr)
		{
			dbglogger_printf("[Installer] Spawning CleanArchivePkgDataThread for %s\n", bg_check_data->hash.c_str());
			ret = pthread_create(&bk_clean_thid, NULL, CleanArchivePkgDataThread, bg_check_data->archive_pkg_data);
			RemoveArchivePkgInstallData(bg_check_data->hash);
			delete bg_check_data;
		}
		else if (bg_check_data->split_pkg_data != nullptr)
		{
			ret = pthread_create(&bk_clean_thid, NULL, CleanSplitPkgDataThread, bg_check_data->split_pkg_data);
			RemoveSplitPkgInstallData(bg_check_data->hash);
			delete bg_check_data;
		}
		activity_inprogess = false;
		file_transfering = false;
		Windows::SetModalMode(false);
		return nullptr;
	}

	bool canInstallRemotePkg(const std::string &url)
	{
		return true;
	}

	int InstallRemotePkg(RemoteClient* client, const std::string &url, pkg_header *header, std::string title, const std::string &path)
	{
		if (url.empty())
			return 0;

		std::string content_id = GetPkgContentId(header);
		std::string title_id = GetTitleIdFromContentId(content_id);
		std::string display_title = title.length() > 0 ? title : (!title_id.empty() ? title_id : content_id);

		int ret;	

		PlayGoInfo playgo_info;
		SceAppInstallPkgInfo pkg_info;
		memset(&playgo_info, 0, sizeof(playgo_info));
		
		for (size_t i = 0; i < SCE_NUM_LANGUAGES; i++) {
			strncpy(playgo_info.languages[i], "", sizeof(language_t) - 1);
		}	

		for (size_t i = 0; i < SCE_NUM_IDS; i++) {
			strncpy(playgo_info.playgo_scenario_ids[i], "", sizeof(playgo_scenario_id_t) - 1);
			strncpy(*playgo_info.content_ids, "", sizeof(content_id_t) - 1);
		}	

		std::string icon_url = "";
		if (!path.empty() && !title_id.empty()) {
			std::string icon_path = std::string("/data/homebrew/ezremote-client/game-icons/") + title_id + ".png";
			FS::MkDirs("/data/homebrew/ezremote-client/game-icons");
			if (ExtractRemotePkg(path, TMP_SFO_PATH, icon_path)) {
				if (FS::FileExists(icon_path)) {
					icon_url = std::string("http://") + GetLocalIP() + ":" + std::to_string(http_int_server_port) + "/game-icons/" + title_id + ".png";
				}
			}
		}

		MetaInfo metainfo = (MetaInfo){
			.uri = url.c_str(),
			.ex_uri = "",
			.playgo_scenario_id = "",
			.content_id = "",
			.content_name = display_title.c_str(),
			.icon_url = ""
		};

		std::map<std::string, std::string> sfo_params;
		if (!path.empty())
			ReadRemotePkgSfoParams(client, path, header, sfo_params);
		LogPkgInstallMetadata("remote", path, url, header, display_title, icon_url, content_id, sfo_params);

		ret = InstallWithDirectPackageInstaller(url, display_title, icon_url, content_id);
		if (ret != 0)
		{
			return 0;
		}

		return 1;
	}

	int InstallLocalPkg(const std::string &path)
	{
		int ret;
		pkg_header header;
		bool completed = false;

		memset(&header, 0, sizeof(header));
		if (FS::Head(path.c_str(), (void *)&header, sizeof(header)) == 0)
			return 0;

		if (BE32(header.pkg_magic) != PS4_PKG_MAGIC && BE32(header.pkg_magic) != PS5_PKG_MAGIC)
			return 0;

		return InstallLocalPkg(path, &header, false);
	}

	int InstallLocalPkg(const std::string &path, pkg_header *header, bool remove_after_install)
	{
		int ret;

		if (strncmp(path.c_str(), "/data/", 6) != 0 &&
			strncmp(path.c_str(), "/user/data/", 11) != 0 &&
			strncmp(path.c_str(), "/mnt/usb", 8) != 0)
			return -1;

		std::string filename = path.substr(path.find_last_of("/") + 1);
		char filepath[1024];
		snprintf(filepath, 1023, "%s", path.c_str());
		if (strncmp(path.c_str(), "/data/", 6) == 0)
			snprintf(filepath, 1023, "/user%s", path.c_str());

		PlayGoInfo playgo_info;
		SceAppInstallPkgInfo pkg_info;
		memset(&playgo_info, 0, sizeof(playgo_info));
		
		for (size_t i = 0; i < SCE_NUM_LANGUAGES; i++) {
			strncpy(playgo_info.languages[i], "", sizeof(language_t) - 1);
		}

		for (size_t i = 0; i < SCE_NUM_IDS; i++) {
			strncpy(playgo_info.playgo_scenario_ids[i], "", sizeof(playgo_scenario_id_t) - 1);
			strncpy(*playgo_info.content_ids, "", sizeof(content_id_t) - 1);
		}

		std::string content_id = GetPkgContentId(header);
		std::string title_id = GetTitleIdFromContentId(content_id);
		std::string title;
		std::string icon_url = "";
		if (BE32(header->pkg_magic) == PS4_PKG_MAGIC || BE32(header->pkg_magic) == PS5_PKG_MAGIC)
		{
			title = GetLocalPkgTitle(filepath, header);
			if (!title_id.empty()) {
				std::string icon_path = std::string("/data/homebrew/ezremote-client/game-icons/") + title_id + ".png";
				FS::MkDirs("/data/homebrew/ezremote-client/game-icons");
				ExtractLocalPkg(path, TMP_SFO_PATH, icon_path);
				if (FS::FileExists(icon_path)) {
					icon_url = std::string("http://") + GetLocalIP() + ":" + std::to_string(http_int_server_port) + "/game-icons/" + title_id + ".png";
				}
			}
		}
		else
		{
			title = filename;
		}
		MetaInfo metainfo = (MetaInfo){
			.uri = filepath,
			.ex_uri = "",
			.playgo_scenario_id = "",
			.content_id = "",
			.content_name = title.c_str(),
			.icon_url = ""
		};

		std::map<std::string, std::string> sfo_params;
		std::string remapped_path = filepath;
		if (!ReadLocalPkgSfoParams(path, header, sfo_params) && remapped_path != path)
			ReadLocalPkgSfoParams(remapped_path, header, sfo_params);
		LogPkgInstallMetadata("local", path, remapped_path, header, title, icon_url, content_id, sfo_params);

		SceAppInstallStatusInstalled progress_info{};
		dbglogger_printf("[Installer] Starting Direct Package Installer for Local PKG %s\n", title.c_str());
		ret = InstallWithDirectPackageInstaller(filepath, title, icon_url, content_id);
		if (ret != 0)
		{
			dbglogger_printf("[Installer] Direct Package Installer failed for %s (ret %d)\n", title.c_str(), ret);
			goto err;
		}

		if (!remove_after_install)
		{
			return 1;
		} 

		sprintf(activity_message, "%s", lang_strings[STR_WAIT_FOR_INSTALL_MSG]);
		bytes_to_download = header->pkg_content_size;
		bytes_transfered = 0;
		prev_tick = Util::GetTick();

		while (strcmp(progress_info.status, "playable") != 0 && strcmp(progress_info.status, "none") != 0 )
		{
			ret = sceAppInstUtilGetInstallStatus(content_id.c_str(), &progress_info);
			if (ret || (progress_info.error_info.error_code != 0))
				return -3;

			bytes_to_download = progress_info.total_size;
			bytes_transfered = progress_info.downloaded_size;
			sceSystemServicePowerTick();
			usleep(500000);
		}

		if (remove_after_install)
			FS::Rm(path);
		return 1;

	err:
		if (remove_after_install)
			FS::Rm(path);
		return 0;
	}

	bool ExtractLocalPkg(const std::string &path, const std::string sfo_path, const std::string icon_path)
	{
		pkg_header tmp_hdr;
		FS::Head(path, &tmp_hdr, sizeof(pkg_header));

		if (BE32(tmp_hdr.pkg_magic) != PS4_PKG_MAGIC && BE32(tmp_hdr.pkg_magic) != PS5_PKG_MAGIC)
			return false;

		size_t entry_count = BE32(tmp_hdr.pkg_entry_count);
		uint32_t entry_table_offset = BE32(tmp_hdr.pkg_table_offset);
		uint64_t entry_table_size = entry_count * sizeof(pkg_table_entry);
		void *entry_table_data = malloc(entry_table_size);

		FILE *fd = FS::OpenRead(path);
		FS::Seek(fd, entry_table_offset);
		FS::Read(fd, entry_table_data, entry_table_size);

		pkg_table_entry *entries = (pkg_table_entry *)entry_table_data;
		void *param_sfo_data = NULL;
		uint32_t param_sfo_offset = 0;
		uint32_t param_sfo_size = 0;
		void *icon0_png_data = NULL;
		uint32_t icon0_png_offset = 0;
		uint32_t icon0_png_size = 0;
		short items = 0;
		for (size_t i = 0; i < entry_count; ++i)
		{
			switch (BE32(entries[i].id))
			{
			case PKG_ENTRY_ID_PARAM_SFO:
				param_sfo_offset = BE32(entries[i].offset);
				param_sfo_size = BE32(entries[i].size);
				items++;
				break;
			case PKG_ENTRY_ID_ICON0_PNG:
				icon0_png_offset = BE32(entries[i].offset);
				icon0_png_size = BE32(entries[i].size);
				items++;
				break;
			default:
				continue;
			}

			if (items == 2)
				break;
		}
		free(entry_table_data);

		if (param_sfo_offset > 0 && param_sfo_size > 0)
		{
			param_sfo_data = malloc(param_sfo_size);
			FILE *out = FS::Create(sfo_path);
			FS::Seek(fd, param_sfo_offset);
			FS::Read(fd, param_sfo_data, param_sfo_size);
			FS::Write(out, param_sfo_data, param_sfo_size);
			FS::Close(out);
			free(param_sfo_data);
		}

		if (icon0_png_offset > 0 && icon0_png_size > 0)
		{
			icon0_png_data = malloc(icon0_png_size);
			FILE *out = FS::Create(icon_path);
			FS::Seek(fd, icon0_png_offset);
			FS::Read(fd, icon0_png_data, icon0_png_size);
			FS::Write(out, icon0_png_data, icon0_png_size);
			FS::Close(out);
			free(icon0_png_data);
		}

		FS::Close(fd);
		return true;
	}

	bool ExtractRemotePkg(const std::string &path, const std::string sfo_path, const std::string icon_path)
	{
		pkg_header tmp_hdr;
		if (!remoteclient->Head(path, &tmp_hdr, sizeof(pkg_header)))
			return false;

		if (BE32(tmp_hdr.pkg_magic) != PS4_PKG_MAGIC && BE32(tmp_hdr.pkg_magic) != PS5_PKG_MAGIC)
			return false;

		size_t entry_count = BE32(tmp_hdr.pkg_entry_count);
		uint32_t entry_table_offset = BE32(tmp_hdr.pkg_table_offset);
		uint64_t entry_table_size = entry_count * sizeof(pkg_table_entry);
		void *entry_table_data = malloc(entry_table_size);
		if (entry_table_data == nullptr)
			return false;

		if (!remoteclient->GetRange(path, entry_table_data, entry_table_size, entry_table_offset))
		{
			free(entry_table_data);
			return false;
		}

		pkg_table_entry *entries = (pkg_table_entry *)entry_table_data;
		void *param_sfo_data = NULL;
		uint32_t param_sfo_offset = 0;
		uint32_t param_sfo_size = 0;
		void *icon0_png_data = NULL;
		uint32_t icon0_png_offset = 0;
		uint32_t icon0_png_size = 0;
		short items = 0;
		for (size_t i = 0; i < entry_count; ++i)
		{
			switch (BE32(entries[i].id))
			{
			case PKG_ENTRY_ID_PARAM_SFO:
				param_sfo_offset = BE32(entries[i].offset);
				param_sfo_size = BE32(entries[i].size);
				items++;
				break;
			case PKG_ENTRY_ID_ICON0_PNG:
				icon0_png_offset = BE32(entries[i].offset);
				icon0_png_size = BE32(entries[i].size);
				items++;
				break;
			default:
				continue;
			}

			if (items == 2)
				break;
		}
		free(entry_table_data);

		if (param_sfo_offset > 0 && param_sfo_size > 0)
		{
			param_sfo_data = malloc(param_sfo_size);
			if (param_sfo_data == nullptr)
				return false;
			FILE *out = FS::Create(sfo_path);
			if (out == nullptr)
			{
				free(param_sfo_data);
				return false;
			}
			if (!remoteclient->GetRange(path, param_sfo_data, param_sfo_size, param_sfo_offset))
			{
				FS::Close(out);
				free(param_sfo_data);
				FS::Rm(sfo_path);
				return false;
			}
			FS::Write(out, param_sfo_data, param_sfo_size);
			FS::Close(out);
			free(param_sfo_data);
		}

		if (icon0_png_offset > 0 && icon0_png_size > 0)
		{
			icon0_png_data = malloc(icon0_png_size);
			if (icon0_png_data == nullptr)
				return false;
			FILE *out = FS::Create(icon_path);
			if (out == nullptr)
			{
				free(icon0_png_data);
				return false;
			}
			if (!remoteclient->GetRange(path, icon0_png_data, icon0_png_size, icon0_png_offset))
			{
				FS::Close(out);
				free(icon0_png_data);
				FS::Rm(icon_path);
				return false;
			}
			FS::Write(out, icon0_png_data, icon0_png_size);
			FS::Close(out);
			free(icon0_png_data);
		}

		return true;
	}

	ArchivePkgInstallData *GetArchivePkgInstallData(const std::string &hash)
	{
		return archive_pkg_install_data_list[hash];
	}

	void AddArchivePkgInstallData(const std::string &hash, ArchivePkgInstallData *pkg_data)
	{
		std::pair<std::string, ArchivePkgInstallData *> pair = std::make_pair(hash, pkg_data);
		archive_pkg_install_data_list.erase(hash);
		archive_pkg_install_data_list.insert(pair);
	}

	void RemoveArchivePkgInstallData(const std::string &hash)
	{
		archive_pkg_install_data_list.erase(hash);
	}

	bool InstallArchivePkg(const std::string &path, ArchivePkgInstallData *pkg_data, bool bg)
	{
		int ret = 0;
		pkg_header header{};
		if (pkg_data->split_file->Read((char *)&header, sizeof(pkg_header), 0) != static_cast<ssize_t>(sizeof(pkg_header)))
			return false;

		std::string content_id = GetPkgContentId(&header);
		std::string title_id = GetTitleIdFromContentId(content_id);
		std::string display_title = !title_id.empty() ? title_id : content_id;

		std::string hash = Util::UrlHash(path);
		std::string full_url = std::string("http://") + GetLocalIP() + ":" + std::to_string(http_server_port) + "/archive_inst/" + hash;
		AddArchivePkgInstallData(hash, pkg_data);

		std::string icon_url = "";
		if (!title_id.empty() && pkg_data && pkg_data->archive_entry && pkg_data->archive_entry->client_data && pkg_data->archive_entry->client_data->client) {
			std::string icon_path = std::string("/data/homebrew/ezremote-client/game-icons/") + title_id + ".png";
			FS::MkDirs("/data/homebrew/ezremote-client/game-icons");
			ExtractRemotePkg(pkg_data->archive_entry->client_data->path, TMP_SFO_PATH, icon_path);
			if (FS::FileExists(icon_path)) {
				icon_url = std::string("http://") + GetLocalIP() + ":" + std::to_string(http_int_server_port) + "/game-icons/" + title_id + ".png";
			}
		}

		std::map<std::string, std::string> sfo_params;
		ReadSplitPkgSfoParams(pkg_data->split_file, &header, sfo_params);
		LogPkgInstallMetadata(bg ? "archive-bg" : "archive", path, full_url, &header, display_title, icon_url, content_id, sfo_params);

		if (!bg)
		{
			PlayGoInfo playgo_info;
			SceAppInstallPkgInfo pkg_info;
			memset(&playgo_info, 0, sizeof(playgo_info));
			
			for (size_t i = 0; i < SCE_NUM_LANGUAGES; i++) {
				strncpy(playgo_info.languages[i], "", sizeof(language_t) - 1);
			}

			for (size_t i = 0; i < SCE_NUM_IDS; i++) {
				strncpy(playgo_info.playgo_scenario_ids[i], "", sizeof(playgo_scenario_id_t) - 1);
				strncpy(*playgo_info.content_ids, "", sizeof(content_id_t) - 1);
			}

			MetaInfo metainfo = (MetaInfo){
				.uri = full_url.c_str(),
				.ex_uri = "",
				.playgo_scenario_id = "",
				.content_id = "",
				.content_name = display_title.c_str(),
				.icon_url = ""
			};

			
			ret = InstallWithDirectPackageInstaller(full_url, display_title, icon_url, content_id);
			if (ret)
			{
				ret = 0;
				goto finish;
			}

			file_transfering = true;
			bytes_to_download = header.pkg_content_size;
			bytes_transfered = 0;
			prev_tick = Util::GetTick();

			SceAppInstallStatusInstalled progress_info{};
			while (strcmp(progress_info.status, "playable") != 0 && strcmp(progress_info.status, "none") != 0 )
			{
				ret = sceAppInstUtilGetInstallStatus(content_id.c_str(), &progress_info);
				if (ret || (progress_info.error_info.error_code != 0))
				{
					ret = 0;
					goto finish;
				}
	
				bytes_to_download = progress_info.total_size;
				bytes_transfered = progress_info.downloaded_size;
				sceSystemServicePowerTick();
				usleep(500000);
			}
		}
		else
		{
			BgProgressCheck *bg_check_data = new BgProgressCheck{};
			bg_check_data->archive_pkg_data = pkg_data;
			bg_check_data->split_pkg_data = nullptr;
			bg_check_data->url = full_url;
			bg_check_data->title = display_title;
			bg_check_data->content_id = content_id;
			bg_check_data->hash = hash;
			bg_check_data->icon_url = icon_url;
			ret = pthread_create(&bk_install_thid, NULL, CheckBgInstallTaskThread, bg_check_data);
			return 1;
		}
		ret = 1;
	finish:
		pthread_create(&bk_clean_thid, NULL, CleanArchivePkgDataThread, pkg_data);
		RemoveArchivePkgInstallData(hash);
		return ret;
	}

	SplitPkgInstallData *GetSplitPkgInstallData(const std::string &hash)
	{
		return split_pkg_install_data_list[hash];
	}

	void AddSplitPkgInstallData(const std::string &hash, SplitPkgInstallData *pkg_data)
	{
		std::pair<std::string, SplitPkgInstallData *> pair = std::make_pair(hash, pkg_data);
		split_pkg_install_data_list.erase(hash);
		split_pkg_install_data_list.insert(pair);
	}

	void RemoveSplitPkgInstallData(const std::string &hash)
	{
		split_pkg_install_data_list.erase(hash);
	}

	bool InstallSplitPkg(const std::string &path, SplitPkgInstallData *pkg_data, bool bg)
	{
		int ret = 0;
		pkg_header header{};
		if (pkg_data->split_file->Read((char *)&header, sizeof(pkg_header), 0) != static_cast<ssize_t>(sizeof(pkg_header)))
			return false;

		std::string content_id = GetPkgContentId(&header);
		std::string title_id = GetTitleIdFromContentId(content_id);
		std::string display_title = !title_id.empty() ? title_id : content_id;

		std::string hash = Util::UrlHash(path);
		std::string full_url = std::string("http://") + GetLocalIP() + ":" + std::to_string(http_server_port) + "/split_inst/" + hash;
		AddSplitPkgInstallData(hash, pkg_data);

		std::string icon_url = "";
		if (!title_id.empty() && pkg_data && pkg_data->remote_client) {
			std::string icon_path = std::string("/data/homebrew/ezremote-client/game-icons/") + title_id + ".png";
			FS::MkDirs("/data/homebrew/ezremote-client/game-icons");
			ExtractRemotePkg(pkg_data->path, TMP_SFO_PATH, icon_path);
			if (FS::FileExists(icon_path)) {
				icon_url = std::string("http://") + GetLocalIP() + ":" + std::to_string(http_int_server_port) + "/game-icons/" + title_id + ".png";
			}
		}

		std::map<std::string, std::string> sfo_params;
		ReadSplitPkgSfoParams(pkg_data->split_file, &header, sfo_params);
		LogPkgInstallMetadata(bg ? "split-bg" : "split", path, full_url, &header, display_title, icon_url, content_id, sfo_params);

		if (!bg)
		{
			PlayGoInfo playgo_info;
			SceAppInstallPkgInfo pkg_info;
			memset(&playgo_info, 0, sizeof(playgo_info));
			
			for (size_t i = 0; i < SCE_NUM_LANGUAGES; i++) {
				strncpy(playgo_info.languages[i], "", sizeof(language_t) - 1);
			}

			for (size_t i = 0; i < SCE_NUM_IDS; i++) {
				strncpy(playgo_info.playgo_scenario_ids[i], "", sizeof(playgo_scenario_id_t) - 1);
				strncpy(*playgo_info.content_ids, "", sizeof(content_id_t) - 1);
			}

			MetaInfo metainfo = (MetaInfo){
				.uri = full_url.c_str(),
				.ex_uri = "",
				.playgo_scenario_id = "",
				.content_id = "",
				.content_name = display_title.c_str(),
				.icon_url = ""
			};
			
			ret = InstallWithDirectPackageInstaller(full_url, display_title, icon_url, content_id);
			if (ret)
			{
				ret = 0;
				goto finish;
			}

			file_transfering = true;
			bytes_to_download = pkg_data->size;
			bytes_transfered = 0;
			prev_tick = Util::GetTick();

			SceAppInstallStatusInstalled progress_info{};
			while (strcmp(progress_info.status, "playable") != 0 && strcmp(progress_info.status, "none") != 0 )
			{
				ret = sceAppInstUtilGetInstallStatus(content_id.c_str(), &progress_info);
				if (ret || (progress_info.error_info.error_code != 0))
				{
					ret = 0;
					goto finish;
				}
	
				bytes_to_download = progress_info.total_size;
				bytes_transfered = progress_info.downloaded_size;
				sceSystemServicePowerTick();
				usleep(500000);
			}
		}
		else
		{
			BgProgressCheck *bg_check_data = new BgProgressCheck{};
			bg_check_data->split_pkg_data = pkg_data;
			bg_check_data->archive_pkg_data = nullptr;
			bg_check_data->url = full_url;
			bg_check_data->title = display_title;
			bg_check_data->content_id = content_id;
			bg_check_data->hash = hash;
			bg_check_data->icon_url = icon_url;
			ret = pthread_create(&bk_install_thid, NULL, CheckBgInstallTaskThread, bg_check_data);
			return 1;
		}
		ret = 1;
	finish:
		pthread_create(&bk_clean_thid, NULL, CleanSplitPkgDataThread, pkg_data);
		RemoveSplitPkgInstallData(hash);
		activity_inprogess = false;
		file_transfering = false;
		Windows::SetModalMode(false);
		return ret;
	}
	
	bool IsDirectPackageInstallerEnabled()
	{
		return !EzRemoteServerVersion().empty();
	}

	static std::string last_install_error = "";
	std::string GetLastInstallError()
	{
		return last_install_error;
	}

	int InstallWithDirectPackageInstaller(const std::string &url, const std::string &title, const std::string &icon, const std::string &content_id)
	{
		last_install_error = "";
		CHTTPClient::HttpResponse res;
		CHTTPClient::HeadersMap headers;
		CHTTPClient tmp_client([](const std::string& log){});
		tmp_client.InitSession(true, CHTTPClient::SettingsFlag::NO_FLAGS);
		headers["Content-Type"] = "application/json";

		json_object *jobj = json_object_new_object();
		json_object_object_add(jobj, "url", json_object_new_string(url.c_str()));
		json_object_object_add(jobj, "title", json_object_new_string(title.c_str()));
		json_object_object_add(jobj, "icon_url", json_object_new_string(icon.c_str()));
		json_object_object_add(jobj, "content_id", json_object_new_string(content_id.c_str()));

		std::string payload = json_object_to_json_string(jobj);
		std::string install_url = std::string("http://localhost:") + std::to_string(http_int_server_port) + "/install";

		bool success = tmp_client.Post(install_url, headers, payload.c_str(), res);
		json_object_put(jobj);

		if (success && HTTP_SUCCESS(res.iCode))
		{
			return 0;
		}

		if (!res.strBody.empty())
		{
			last_install_error = std::string((const char*)res.strBody.data(), res.strBody.size());
		}
		else if (!success)
		{
			last_install_error = "Failed to connect to ezremote-server proxy";
		}
		else
		{
			last_install_error = "Proxy server returned HTTP " + std::to_string(res.iCode);
		}

		// Try starting ezremote-server if it failed
		StartEzRemoteServer();
		usleep(1000000); // 1 sec

		// Retry once
		success = tmp_client.Post(install_url, headers, payload.c_str(), res);
		if (success && HTTP_SUCCESS(res.iCode))
		{
			last_install_error = "";
			return 0;
		}
		
		if (!res.strBody.empty())
		{
			last_install_error = std::string((const char*)res.strBody.data(), res.strBody.size());
		}
		else if (!success)
		{
			last_install_error = "Failed to connect to ezremote-server proxy";
		}
		else
		{
			last_install_error = "Proxy server returned HTTP " + std::to_string(res.iCode);
		}

		return -1;
	}

	std::string EzRemoteServerVersion()
	{
		CHTTPClient::HttpResponse res;
        CHTTPClient::HeadersMap headers;
        CHTTPClient tmp_client([](const std::string& log){});
        tmp_client.InitSession(true, CHTTPClient::SettingsFlag::NO_FLAGS);
        tmp_client.SetCertificateFile(CACERT_FILE);
		tmp_client.SetTimeout(1);

		std::string version_url = std::string("http://localhost:") + std::to_string(http_int_server_port) + "/version";
        if (tmp_client.Get(version_url, headers, res))
        {
			if (HTTP_SUCCESS(res.iCode))
            {
				std::string version = std::string(res.strBody.data(), res.strBody.size());
				return version;
			}
		}

		return "";
	}

	int StartEzRemoteServer()
	{
		char buffer[8192];
		in_addr_t in_addr;
		in_addr_t server_addr;
		int filefd = -1;
		int sockfd = -1;
		ssize_t i;
		ssize_t read_return;
		struct hostent *hostent;
		struct sockaddr_in sockaddr_in;
		unsigned short server_port = 9021;
	
		if (!EzRemoteServerVersion().empty())
			return 0;

		filefd = open(SERVER_ELF_PATH, O_RDONLY);
		if (filefd == -1)
		{
			goto err;
		}
	
		sockfd = socket(AF_INET, SOCK_STREAM, 0);
		if (sockfd == -1)
		{
			goto err;
		}
	
		/* Prepare sockaddr_in. */
		hostent = gethostbyname("127.0.0.1");
		if (hostent == NULL)
		{
			goto err;
		}
	
		in_addr = inet_addr(inet_ntoa(*(struct in_addr *)*(hostent->h_addr_list)));
		if (in_addr == (in_addr_t)-1)
		{
			goto err;
		}
	
		sockaddr_in.sin_addr.s_addr = in_addr;
		sockaddr_in.sin_family = AF_INET;
		sockaddr_in.sin_port = htons(server_port);
		/* Do the actual connection. */
		if (connect(sockfd, (struct sockaddr *)&sockaddr_in, sizeof(sockaddr_in)) == -1)
		{
			goto err;
		}
	
		while (1)
		{
			read_return = read(filefd, buffer, 8192);
			if (read_return == 0)
				break;
			if (read_return == -1)
			{
				goto err;
			}
			if (write(sockfd, buffer, read_return) == -1)
			{
				goto err;
			}
		}
	
		close(filefd);
		close(sockfd);

		return 0;

	err:
		if (filefd != -1)
			close(filefd);
		if (sockfd != -1)
			close(sockfd);
		return -1;
	}



    bool GetPkgSfoInfo(const std::string &path, RemoteClient *client, std::map<std::string, std::string> &sfo_params)
    {
        pkg_header header;
        if (client != nullptr)
        {
            if (!client->GetRange(path, &header, sizeof(pkg_header), 0))
                return false;
            return ReadRemotePkgSfoParams(client, path, &header, sfo_params);
        }
        else
        {
            if (FS::Head(path, &header, sizeof(pkg_header)) <= 0)
                return false;
            return ReadLocalPkgSfoParams(path, &header, sfo_params);
        }
    }
}
