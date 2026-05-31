#include "dpi_usecase.h"
#include <string.h>
#include <strings.h>
#include "sceAppInstUtil.h"
#include "dbglogger.h"

namespace DpiUseCase {

    static bool starts_with(const std::string &value, const char *prefix)
    {
        return value.rfind(prefix, 0) == 0;
    }

    static bool is_safe_content_id(const std::string &content_id)
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

    static std::string sanitize_dpi_field(const std::string& value)
    {
        if (value.empty())
            return "";

        std::string sanitized;
        sanitized.reserve(value.size());
        for (char p : value)
        {
            unsigned char ch = (unsigned char)p;
            if (p == '|' || ch < 0x20 || ch >= 0x7f)
                sanitized.push_back(' ');
            else
                sanitized.push_back(p);
        }

        if (sanitized.size() > 512)
            sanitized.resize(512);

        return sanitized;
    }

    static std::string validate_icon_url(const std::string& value)
    {
        if (value.empty() || value.size() > 2048)
            return "";

        for (char c : value)
        {
            unsigned char ch = (unsigned char)c;
            if (c == '|' || ch <= 0x20 || ch == 0x7f)
                return "";
        }

        size_t scheme_len = 0;
        if (starts_with(value, "http://"))
            scheme_len = 7;
        else if (starts_with(value, "https://"))
            scheme_len = 8;
        else
            return "";

        size_t slash_pos = value.find('/', scheme_len);
        size_t host_len = (slash_pos == std::string::npos ? value.size() : slash_pos) - scheme_len;
        if (host_len == 0)
            return "";

        return value;
    }

    static std::string validate_content_id(const std::string& value)
    {
        return is_safe_content_id(value) ? value : "";
    }

    int Initialize() {
        return sceAppInstUtilInitialize();
    }

    int InstallPackage(const std::string& uri, const std::string& title, const std::string& icon_url, const std::string& content_id) {
        PlayGoInfo playgo_info;
        SceAppInstallPkgInfo pkg_info;
        MetaInfo metainfo;
        
        memset(&pkg_info, 0, sizeof(pkg_info));
        memset(&playgo_info, 0, sizeof(playgo_info));

        for (size_t i = 0; i < SCE_NUM_LANGUAGES; i++)
        {
            strncpy(playgo_info.languages[i], "", sizeof(language_t) - 1);
        }
    
        for (size_t i = 0; i < SCE_NUM_IDS; i++)
        {
            strncpy(playgo_info.playgo_scenario_ids[i], "", sizeof(playgo_scenario_id_t) - 1);
            strncpy(playgo_info.content_ids[i], "", sizeof(content_id_t) - 1);
        }

        std::string safe_title = sanitize_dpi_field(title);
        std::string safe_icon = validate_icon_url(icon_url);
        std::string safe_id = validate_content_id(content_id);

        metainfo.uri = uri.c_str();
        metainfo.ex_uri = "";
        metainfo.playgo_scenario_id = "";
        metainfo.content_name = !safe_title.empty() ? safe_title.c_str() : uri.c_str();
        metainfo.icon_url = safe_icon.c_str();
        metainfo.content_id = safe_id.c_str();

        dbglogger_log("[DPI] ===== Sending Payload to sceAppInstUtilInstallByPackage =====");
        dbglogger_log("[DPI] uri=%s", metainfo.uri);
        dbglogger_log("[DPI] ex_uri=%s", metainfo.ex_uri);
        dbglogger_log("[DPI] content_name=%s", metainfo.content_name);
        dbglogger_log("[DPI] icon_url=%s", metainfo.icon_url);
        dbglogger_log("[DPI] content_id=%s", metainfo.content_id);
        dbglogger_log("[DPI] playgo_scenario_id=%s", metainfo.playgo_scenario_id);
        dbglogger_log("[DPI] ===========================================================");

        int ret = sceAppInstUtilInstallByPackage(&metainfo, &pkg_info, &playgo_info);

        dbglogger_log("[DPI] sceAppInstUtilInstallByPackage returned: 0x%08X", ret);

        return ret;
    }
}
