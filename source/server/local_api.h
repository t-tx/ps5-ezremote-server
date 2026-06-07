
// Auto-generated local_api.h for ps5-ezremote-server

// ---------------- LOCAL APIs ----------------
svr->Post("/__local__/list", [&](const Request &req, Response &res) {
    const char *path;
    bool onlyFolders = false;
    json_object *jobj = json_tokener_parse(req.body.c_str());
    if (jobj != nullptr) {
        path = json_object_get_string(json_object_object_get(jobj, "path"));
        const char *onlyFolders_text = json_object_get_string(json_object_object_get(jobj, "onlyFolders"));
        if (onlyFolders_text != nullptr && strcasecmp(onlyFolders_text, "true")==0) onlyFolders = true;
        if (path == nullptr) {
            bad_request(res, "Required path parameter missing");
            json_object_put(jobj);
            return;
        }
    } else {
        bad_request(res, "Invalid payload");
        return;
    }

    int err;
    std::vector<DirEntry> files = FS::ListDir(path, &err);
    DirEntry::Sort(files);
    json_object *json_files = json_object_new_array();
    for (auto& it : files) {
        if (((onlyFolders && it.isDir) || !onlyFolders) && strcmp(it.name, "..") != 0) {
            json_object *new_file = json_object_new_object();
            char display_date[32];
            sprintf(display_date, "%04d-%02d-%02d %02d:%02d:%02d", it.modified.year, it.modified.month, it.modified.day, it.modified.hours, it.modified.minutes, it.modified.seconds);
            json_object_object_add(new_file, "name", json_object_new_string(it.name));
            json_object_object_add(new_file, "rights", json_object_new_string(it.isDir ? "drwxrwxrwx" : "rw-rw-rw-"));
            json_object_object_add(new_file, "date", json_object_new_string(display_date));
            json_object_object_add(new_file, "size", json_object_new_string(it.isDir ? "" : std::to_string(it.file_size).c_str()));
            json_object_object_add(new_file, "type", json_object_new_string(it.isDir ? "dir" : "file"));
            json_object_array_add(json_files, new_file);
        }
    }
    json_object *results = json_object_new_object();
    json_object_object_add(results, "result", json_files);
    const char *results_str = json_object_to_json_string(results);

    res.status = 200;
    res.set_content(results_str, strlen(results_str), "application/json");
    json_object_put(results);
    json_object_put(jobj);
});

svr->Post("/__local__/rename", [&](const Request &req, Response &res) {
    const char *item;
    const char *newItemPath;
    json_object *jobj = json_tokener_parse(req.body.c_str());
    if (jobj != nullptr) {
        item = json_object_get_string(json_object_object_get(jobj, "item"));
        newItemPath = json_object_get_string(json_object_object_get(jobj, "newItemPath"));
        if (item == nullptr || newItemPath == nullptr) {
            bad_request(res, "Required parameters missing");
            json_object_put(jobj);
            return;
        }
        FS::Rename(item, newItemPath);
        success(res);
    } else {
        bad_request(res, "Invalid payload");
    }
    if (jobj) json_object_put(jobj);
});

svr->Post("/__local__/createFolder", [&](const Request &req, Response &res) {
    const char *newPath;
    json_object *jobj = json_tokener_parse(req.body.c_str());
    if (jobj != nullptr) {
        newPath = json_object_get_string(json_object_object_get(jobj, "newPath"));
        if (newPath == nullptr) {
            bad_request(res, "Required parameters missing");
            json_object_put(jobj);
            return;
        }
        FS::MkDirs(newPath);
        success(res);
    } else {
        bad_request(res, "Invalid payload");
    }
    if (jobj) json_object_put(jobj);
});

svr->Post("/__local__/install", [&](const Request &req, Response &res) {
    json_object *jobj = json_tokener_parse(req.body.c_str());
    if (jobj != nullptr) {
        json_object *items_arr = json_object_object_get(jobj, "items");
        if (!items_arr || json_object_get_type(items_arr) != json_type_array) {
            bad_request(res, "Invalid items array");
            json_object_put(jobj);
            return;
        }
        struct array_list *arr = json_object_get_array(items_arr);
        for (size_t i = 0; i < arr->length; i++) {
            const char *item = json_object_get_string((json_object *)array_list_get_idx(arr, i));
            if (item == nullptr || item[0] == '\0') {
                bad_request(res, "Invalid item path");
                json_object_put(jobj);
                return;
            }
            INSTALLER::InstallLocalPkg(item);
        }
        success(res);
    } else {
        bad_request(res, "Invalid payload");
    }
    if (jobj) json_object_put(jobj);
});

svr->Post("/__local__/getContent", [&](const Request &req, Response &res) {
    const char *item;
    json_object *jobj = json_tokener_parse(req.body.c_str());
    if (jobj != nullptr) {
        item = json_object_get_string(json_object_object_get(jobj, "item"));
        if (item == nullptr) {
            bad_request(res, "Required parameters missing");
            json_object_put(jobj);
            return;
        }
        FILE* fp = FS::OpenRead(item);
        if (fp) {
            fseek(fp, 0, SEEK_END);
            size_t size = ftell(fp);
            fseek(fp, 0, SEEK_SET);
            char* buf = (char*)malloc(size + 1);
            if (buf) {
                fread(buf, 1, size, fp);
                buf[size] = 0;
                dbglogger_log("[api] /api/sitelist finished generating response");
        json_object *results = json_object_new_object();
                json_object_object_add(results, "result", json_object_new_string(buf));
                const char *results_str = json_object_to_json_string(results);
                dbglogger_log("[api] /api/sitelist returning response");
        res.status = 200;
                res.set_content(results_str, strlen(results_str), "application/json");
                json_object_put(results);
                free(buf);
            } else {
                failed(res, 500, "Memory allocation failed");
            }
            FS::Close(fp);
        } else {
            failed(res, 500, "Read file failed");
        }
    } else {
        bad_request(res, "Invalid payload");
    }
    if (jobj) json_object_put(jobj);
});

svr->Post("/__local__/edit", [&](const Request &req, Response &res) {
    const char *item;
    const char *content;
    json_object *jobj = json_tokener_parse(req.body.c_str());
    if (jobj != nullptr) {
        item = json_object_get_string(json_object_object_get(jobj, "item"));
        content = json_object_get_string(json_object_object_get(jobj, "content"));
        if (item == nullptr || content == nullptr) {
            bad_request(res, "Required parameters missing");
            json_object_put(jobj);
            return;
        }
        FILE* fp = fopen(item, "w");
        if (fp) {
            fwrite(content, 1, strlen(content), fp);
            fclose(fp);
            success(res);
        } else {
            failed(res, 500, "Write file failed");
        }
    } else {
        bad_request(res, "Invalid payload");
    }
    if (jobj) json_object_put(jobj);
});

svr->Post("/__local__/check_exists", [&](const Request &req, Response &res) {
    const char *path;
    json_object *jobj = json_tokener_parse(req.body.c_str());
    if (jobj != nullptr) {
        path = json_object_get_string(json_object_object_get(jobj, "path"));
        if (path == nullptr) {
            bad_request(res, "Required parameters missing");
            json_object_put(jobj);
            return;
        }
        bool exists = FS::FileExists(path) || FS::FolderExists(path);
        dbglogger_log("[api] /api/sitelist finished generating response");
        json_object *results = json_object_new_object();
        json_object *result_obj = json_object_new_object();
        json_object_object_add(result_obj, "exists", json_object_new_boolean(exists));
        json_object_object_add(results, "result", result_obj);
        const char *results_str = json_object_to_json_string(results);
        dbglogger_log("[api] /api/sitelist returning response");
        res.status = 200;
        res.set_content(results_str, strlen(results_str), "application/json");
        json_object_put(results);
    } else {
        bad_request(res, "Invalid payload");
    }
    if (jobj) json_object_put(jobj);
});

// ---------------- SITE APIs ----------------

svr->Get("/api/sites", [&](const Request &req, Response &res) {
    dbglogger_log("[api] /api/sites called");
    json_object *json_sites = json_object_new_array();
    dbglogger_log("[api] json_sites array created. %zu sites configured", configured_sites.size());
    for (size_t i = 0; i < configured_sites.size(); i++) {
        dbglogger_log("[api] /api/sites processing site %zu", i);
        RemoteSettings& s = configured_sites[i];
        json_object *site = json_object_new_object();
        json_object_object_add(site, "site_idx", json_object_new_int(i));
        json_object_object_add(site, "name", json_object_new_string(s.site_name));
        json_object_object_add(site, "server", json_object_new_string(s.server));
        json_object_object_add(site, "username", json_object_new_string(s.username));
        json_object_object_add(site, "password", json_object_new_string(s.password));
        json_object_object_add(site, "type", json_object_new_int(s.type));
        json_object_object_add(site, "http_server_type", json_object_new_string(s.http_server_type));
        json_object_object_add(site, "default_directory", json_object_new_string(s.default_directory));
        json_object_object_add(site, "enable_rpi", json_object_new_boolean(s.enable_rpi));
        json_object_array_add(json_sites, site);
    }
    dbglogger_log("[api] /api/sites creating results object");
    json_object *results = json_object_new_object();
    json_object_object_add(results, "result", json_sites);
    dbglogger_log("[api] /api/sites converting to json string");
    const char *results_str = json_object_to_json_string(results);
    dbglogger_log("[api] /api/sites setting content");
    res.status = 200;
    res.set_content(results_str, strlen(results_str), "application/json");
    dbglogger_log("[api] /api/sites putting results");
    json_object_put(results);
    dbglogger_log("[api] /api/sites done");
});

svr->Post("/api/sitesave", [&](const Request &req, Response &res) {
    json_object *jobj = json_tokener_parse(req.body.c_str());
    if (jobj != nullptr) {
        RemoteSettings s;
        memset(&s, 0, sizeof(s));
        
        const char* server = json_object_get_string(json_object_object_get(jobj, "server"));
        const char* username = json_object_get_string(json_object_object_get(jobj, "username"));
        const char* password = json_object_get_string(json_object_object_get(jobj, "password"));
        const char* http_server_type = json_object_get_string(json_object_object_get(jobj, "http_server_type"));
        const char* default_dir = json_object_get_string(json_object_object_get(jobj, "default_directory"));
        json_object* enable_rpi_obj = json_object_object_get(jobj, "enable_rpi");
        
        if (server) {
            strncpy(s.server, server, sizeof(s.server)-1);
            strncpy(s.site_name, server, sizeof(s.site_name)-1); // Use server as name for now
            
            // Infer type from server URL prefix
            if (strncmp(server, "ftp://", 6) == 0) s.type = CLIENT_TYPE_FTP;
            else if (strncmp(server, "http://", 7) == 0 || strncmp(server, "https://", 8) == 0) s.type = CLIENT_TYPE_HTTP_SERVER;
            else s.type = CLIENT_TYPE_FTP; // Default
        }
        if (username) strncpy(s.username, username, sizeof(s.username)-1);
        if (password) strncpy(s.password, password, sizeof(s.password)-1);
        if (http_server_type) strncpy(s.http_server_type, http_server_type, sizeof(s.http_server_type)-1);
        if (default_dir) strncpy(s.default_directory, default_dir, sizeof(s.default_directory)-1);
        if (enable_rpi_obj) s.enable_rpi = json_object_get_boolean(enable_rpi_obj);
        
        configured_sites.push_back(s);
        CONFIG::SaveConfiguredSites();
        
        dbglogger_log("[api] /api/sitelist finished generating response");
        json_object *results = json_object_new_object();
        json_object *result_obj = json_object_new_object();
        json_object_object_add(result_obj, "site_idx", json_object_new_int(configured_sites.size() - 1));
        json_object_object_add(results, "result", result_obj);
        
        const char *results_str = json_object_to_json_string(results);
        dbglogger_log("[api] /api/sitelist returning response");
        res.status = 200;
        res.set_content(results_str, strlen(results_str), "application/json");
        json_object_put(results);
    } else {
        bad_request(res, "Invalid payload");
    }
    if (jobj) json_object_put(jobj);
});

svr->Post("/api/sitelist", [&](const Request &req, Response &res) {
    dbglogger_log("[api] /api/sitelist called");
    json_object *jobj = json_tokener_parse(req.body.c_str());
    if (jobj != nullptr) {
        json_object *site_idx_obj = json_object_object_get(jobj, "site_idx");
        if (site_idx_obj == nullptr) {
            bad_request(res, "Invalid parameters");
            json_object_put(jobj);
            return;
        }
        int site_idx = json_object_get_int(site_idx_obj);
        const char* path = json_object_get_string(json_object_object_get(jobj, "path"));
        
        dbglogger_log("[api] /api/sitelist parsed req, path: %s", path ? path : "null");
        if (site_idx < 0 || site_idx >= configured_sites.size() || !path || path[0] == '\0') {
            bad_request(res, "Invalid parameters");
            json_object_put(jobj);
            return;
        }

        auto normalize_list_path = [](const char *raw_path) {
            std::vector<std::string> parts;
            std::string input = raw_path;
            size_t start = 0;

            while (start < input.length()) {
                while (start < input.length() && input[start] == '/') start++;
                size_t end = input.find('/', start);
                if (end == std::string::npos) end = input.length();

                std::string part = input.substr(start, end - start);
                if (part == "..") {
                    if (!parts.empty()) parts.pop_back();
                } else if (!part.empty() && part != ".") {
                    parts.push_back(part);
                }

                start = end + 1;
            }

            std::string normalized = "/";
            for (size_t i = 0; i < parts.size(); i++) {
                if (i > 0) normalized += "/";
                normalized += parts[i];
            }
            return normalized;
        };

        std::string list_path = normalize_list_path(path);

        dbglogger_log("[api] /api/sitelist getting client for site %d", site_idx);
        RemoteClient* client = GetRemoteClientForSite(site_idx);
        if (!client) {
            failed(res, 500, "Failed to initialize client");
            json_object_put(jobj);
            return;
        }
        
        dbglogger_log("[api] /api/sitelist listing dir %s", list_path.c_str());
        std::vector<DirEntry> files = client->ListDir(list_path);
        dbglogger_log("[api] /api/sitelist sorting files");
        DirEntry::Sort(files);
        
        dbglogger_log("[api] /api/sitelist generating response");
        json_object *json_files = json_object_new_array();
        for (auto& it : files) {
            if (strcmp(it.name, ".") == 0 || strcmp(it.name, "..") == 0) continue;

            json_object *new_file = json_object_new_object();
            char display_date[32];
            sprintf(display_date, "%04d-%02d-%02d %02d:%02d:%02d", it.modified.year, it.modified.month, it.modified.day, it.modified.hours, it.modified.minutes, it.modified.seconds);
            json_object_object_add(new_file, "name", json_object_new_string(it.name));
            json_object_object_add(new_file, "date", json_object_new_string(display_date));
            json_object_object_add(new_file, "size", json_object_new_string(it.isDir ? "" : std::to_string(it.file_size).c_str()));
            json_object_object_add(new_file, "type", json_object_new_string(it.isDir ? "dir" : "file"));
            json_object_array_add(json_files, new_file);
        }
        
        dbglogger_log("[api] /api/sitelist finished generating response");
        json_object *results = json_object_new_object();
        json_object_object_add(results, "result", json_files);
        const char *results_str = json_object_to_json_string(results);
        dbglogger_log("[api] /api/sitelist returning response");
        res.status = 200;
        res.set_content(results_str, strlen(results_str), "application/json");
        json_object_put(results);
        
        dbglogger_log("[api] /api/sitelist deleting client");
        dbglogger_log("[api] /api/sitelist handler finished successfully");
        DeleteRemoteClient(client);
    } else {
        bad_request(res, "Invalid payload");
    }
    if (jobj) json_object_put(jobj);
});

// --- EXTRA HANDLERS ---
svr->Get("/__local__/uploadResumeSize", [&](const Request &req, Response &res)
        {
            std::string destination = req.get_param_value("destination");
            std::string filename = req.get_param_value("filename");
            std::string file_path = destination + "/" + filename;
            int64_t size = 0;
            if (FS::FileExists(file_path))
                size = FS::GetSize(file_path);
            std::string result_str = "{\"size\":" + std::to_string(size) + "}";
            dbglogger_log("[api] /api/sitelist returning response");
        res.status = 200;
            res.set_content(result_str.c_str(), result_str.length(), "application/json"); });

svr->Post("/__local__/upload", [&](const Request &req, Response &res, const ContentReader &content_reader)
        {
            auto start_time = std::chrono::high_resolution_clock::now();
            int64_t disk_write_time_ms = 0;
            int64_t network_read_time_ms = 0;
            int64_t file_open_time_ms = 0;
            int64_t file_close_time_ms = 0;
            
            MultipartFormDataItems items;
            std::string destination;
            size_t chunk_size = 0;
            size_t chunk_number = -1;
            size_t total_size = 0;
            size_t currentChunkSize = 0;
            int out_fd = -1;
            std::string new_file;
            bool upload_failed = false;
            bool saw_file = false;
            std::string upload_error;
            size_t total_disk_bytes = 0;

            auto fail_upload = [&](const std::string &msg) {
                upload_failed = true;
                if (upload_error.empty())
                    upload_error = msg;
                return false;
            };

            auto read_start = std::chrono::high_resolution_clock::now();
            bool read_ok = content_reader(
                [&](const MultipartFormData &item)
                {
                    if (upload_failed) return false;
                    
                    items.push_back(item);
                    if (item.name == "file")
                    {
                        saw_file = true;
                        if (item.filename.empty())
                            return fail_upload("Upload filename is missing.");

                        if (destination.empty())
                            return fail_upload("Upload destination is missing.");

                        std::string safe_filename = item.filename;
                        size_t pos = safe_filename.find_last_of("/\\");
                        if (pos != std::string::npos) {
                            safe_filename = safe_filename.substr(pos + 1);
                        }
                        new_file = destination + "/" + safe_filename;
                        
                        auto open_start = std::chrono::high_resolution_clock::now();
                        if (chunk_number == static_cast<size_t>(-1) || chunk_number == 0)
                            out_fd = open(new_file.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0666);
                        else if (chunk_number > 0)
                            out_fd = open(new_file.c_str(), O_WRONLY | O_CREAT | O_APPEND, 0666);
                        auto open_end = std::chrono::high_resolution_clock::now();
                        file_open_time_ms += std::chrono::duration_cast<std::chrono::milliseconds>(open_end - open_start).count();

                        if (out_fd == -1)
                            return fail_upload("Failed to open upload destination.");
                    }
                    return true;
                },
                [&](const char *data, size_t data_length)
                {
                    if (upload_failed) return false;
                    
                    if (items.empty()) return fail_upload("Invalid multipart upload data.");

                    if (items.back().name != "file")
                    {
                        items.back().content.append(data, data_length);
                        
                        if (items.back().name == "destination") {
                            destination = items.back().content;
                        } else if (items.back().name == "_chunkNumber") {
                            std::stringstream ss(items.back().content);
                            ss >> chunk_number;
                        }
                    }
                    else
                    {
                        if (out_fd != -1 && data_length > 0)
                        {
                            auto write_start = std::chrono::high_resolution_clock::now();
                            int written = write(out_fd, data, data_length);
                            auto write_end = std::chrono::high_resolution_clock::now();
                            disk_write_time_ms += std::chrono::duration_cast<std::chrono::milliseconds>(write_end - write_start).count();
                            
                            if (written != static_cast<int>(data_length)) {
                                return fail_upload("Failed to write uploaded file data.");
                            }
                            total_disk_bytes += written;
                        }
                    }
                    return true;
                });
            
            auto read_end = std::chrono::high_resolution_clock::now();
            network_read_time_ms = std::chrono::duration_cast<std::chrono::milliseconds>(read_end - read_start).count();

            if (!read_ok || upload_failed || !saw_file)
            {
                if (out_fd != -1)
                {
                    close(out_fd);
                    out_fd = -1;
                }

                if (upload_error.empty()) {
                    upload_error = res.status == 413 ? "Upload payload is too large." : "Failed to read uploaded file data.";
                }

                failed(res, 200, upload_error);
                return;
            }

            if (out_fd != -1)
            {
                auto close_start = std::chrono::high_resolution_clock::now();
                close(out_fd);
                auto close_end = std::chrono::high_resolution_clock::now();
                file_close_time_ms += std::chrono::duration_cast<std::chrono::milliseconds>(close_end - close_start).count();
            }
            auto end_time = std::chrono::high_resolution_clock::now();
            auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(end_time - start_time).count();
            
            std::string result_str = "{ \"result\": { \"success\": true, \"error\": null, "
                                     "\"duration_ms\": " + std::to_string(duration) + ", "
                                     "\"disk_write_time_ms\": " + std::to_string(disk_write_time_ms) + ", "
                                     "\"network_read_time_ms\": " + std::to_string(network_read_time_ms) + ", "
                                     "\"file_open_time_ms\": " + std::to_string(file_open_time_ms) + ", "
                                     "\"file_close_time_ms\": " + std::to_string(file_close_time_ms) + ", "
                                     "\"total_disk_bytes\": " + std::to_string(total_disk_bytes) + 
                                     "} }";
            dbglogger_log("[api] /api/sitelist returning response");
        res.status = 200;
            res.set_content(result_str.c_str(), result_str.length(), "application/json"); });

svr->Post("/__local__/compress", [&](const Request &req, Response &res)
        {
            if (activity_inprogess)
            {
                failed(res, 200, "Activity in progress");
                return;
            }

            json_object *items;
            const char* destination;
            const char* compressedFilename;
            
            json_object *jobj = json_tokener_parse(req.body.c_str());
            if (jobj != nullptr)
            {
                items = json_object_object_get(jobj, "items");
                destination = json_object_get_string(json_object_object_get(jobj, "destination"));
                compressedFilename = json_object_get_string(json_object_object_get(jobj, "compressedFilename"));

                if (items == nullptr || json_object_get_type(items) != json_type_array || destination == nullptr || compressedFilename == nullptr)
                {
                    bad_request(res, "Required items,destination,compressedFilename parameter missing");
                    json_object_put(jobj);
                    return;
                }
            }
            else
            {
                bad_request(res, "Invalid payload");
                return;
            }

            if (!FS::FolderExists(destination))
                FS::MkDirs(destination);
            
            std::string safe_compressedFilename = compressedFilename;
            size_t pos = safe_compressedFilename.find_last_of("/\\");
            if (pos != std::string::npos) {
                safe_compressedFilename = safe_compressedFilename.substr(pos + 1);
            }
            std::string zip_file = std::string(destination) + "/" + safe_compressedFilename;
            zipFile zf = zipOpen64(zip_file.c_str(), APPEND_STATUS_CREATE);
            if (zf != NULL)
            {
                size_t len = json_object_array_length(items);
                for (size_t i=0; i < len; i++)
                {
                    const char *item = json_object_get_string(json_object_array_get_idx(items, i));
                    if (item == nullptr || item[0] == '\0')
                    {
                        zipClose(zf, NULL);
                        FS::Rm(zip_file);
                        bad_request(res, "Invalid item path");
                        json_object_put(jobj);
                        return;
                    }
                    std::string src = std::string(item);
                    size_t slash_pos = src.find_last_of("/");
                    int ret = ZipUtil::ZipAddPath(zf, src, (slash_pos != std::string::npos ? slash_pos + 1 : 1), Z_DEFAULT_COMPRESSION);
                    if (ret != 1)
                    {
                        zipClose(zf, NULL);
                        FS::Rm(zip_file);
                        failed(res, 200, "Failed to create zip");
                        json_object_put(jobj);
                        return;
                    }
                }
                zipClose(zf, NULL);
                success(res);
            }
            else
            {
                failed(res, 200, "Failed to create zip");
            }
            json_object_put(jobj); });

svr->Post("/__local__/extract", [&](const Request &req, Response &res)
        {
            const char* item;
            const char* destination;
            const char* folderName;
            
            json_object *jobj = json_tokener_parse(req.body.c_str());
            if (jobj != nullptr)
            {
                item = json_object_get_string(json_object_object_get(jobj, "item"));
                destination = json_object_get_string(json_object_object_get(jobj, "destination"));
                folderName = json_object_get_string(json_object_object_get(jobj, "folderName"));

                if (item == nullptr || destination == nullptr || folderName == nullptr)
                {
                    bad_request(res, "Required item,destination,folderName parameter missing");
                    json_object_put(jobj);
                    return;
                }
            }
            else
            {
                bad_request(res, "Invalid payload");
                return;
            }

            uint64_t job_id = 0;
            std::string error;
            if (!StartExtractJob(-1, item, destination, folderName, &error, &job_id))
            {
                failed(res, 200, error);
                json_object_put(jobj);
                return;
            }

            success(res);
            json_object_put(jobj); });

svr->Post("/api/siteextract", [&](const Request &req, Response &res) {
            json_object *jobj = json_tokener_parse(req.body.c_str());
            if (!jobj) { bad_request(res, "Invalid payload"); return; }

            json_object *site_obj = json_object_object_get(jobj, "site_idx");
            const char *item = json_object_get_string(json_object_object_get(jobj, "item"));
            const char *destination = json_object_get_string(json_object_object_get(jobj, "destination"));
            const char *folderName = json_object_get_string(json_object_object_get(jobj, "folderName"));
            if (site_obj == nullptr || item == nullptr || item[0] == '\0' || destination == nullptr || destination[0] == '\0')
            {
                bad_request(res, "Required parameters missing");
                json_object_put(jobj);
                return;
            }

            uint64_t job_id = 0;
            std::string error;
            int site_idx = json_object_get_int(site_obj);
            if (site_idx < 0 || static_cast<size_t>(site_idx) >= configured_sites.size()) {
                bad_request(res, "Invalid site_idx");
                json_object_put(jobj);
                return;
            }
            if (!StartExtractJob(site_idx, item, destination, folderName != nullptr ? folderName : "", &error, &job_id))
            {
                failed(res, 200, error);
                json_object_put(jobj);
                return;
            }
            success(res);
            json_object_put(jobj);
        });

        svr->Get("/__local__/downloadFile", [&](const Request &req, Response &res) {
            std::string path = req.get_param_value("path", 0);
            if (path.empty()) { bad_request(res, "Failed to download"); return; }
            int64_t size = FS::GetSize(path.c_str());
            FILE *in = FS::OpenRead(path.c_str());
            if (!in) { bad_request(res, "Failed to download"); return; }
            size_t slash_pos = path.find_last_of("/");
            std::string name = (slash_pos != std::string::npos) ? path.substr(slash_pos+1) : path;
            res.set_header("Content-Disposition", "attachment; filename=\"" + name + "\"");
            res.set_content_provider(size, "application/octet-stream",
                [in](size_t offset, size_t length, DataSink &sink) {
                    size_t size_to_read = std::min(static_cast<size_t>(length), (size_t)1048576);
                    std::vector<char> buff(size_to_read);
                    FS::Seek(in, offset);
                    size_t read_len = FS::Read(in, buff.data(), size_to_read);
                    if (read_len > 0) sink.write(buff.data(), read_len);
                    return true;
                },
                [in](bool) { FS::Close(in); }
            );
        });

        svr->Post("/api/sitedownloaddest", [&](const Request &req, Response &res) {
            json_object *jobj = json_tokener_parse(req.body.c_str());
            if (!jobj) { bad_request(res, "Invalid payload"); return; }
            json_object *site_idx_obj = json_object_object_get(jobj, "site_idx");
            if (!site_idx_obj) { bad_request(res, "Missing parameters"); json_object_put(jobj); return; }
            int site_idx = json_object_get_int(site_idx_obj);
            if (site_idx < 0 || site_idx >= configured_sites.size()) { failed(res, 500, "Invalid site_idx"); json_object_put(jobj); return; }
            RemoteSettings& s = configured_sites[site_idx];
            
            const char* path = json_object_get_string(json_object_object_get(jobj, "path"));
            const char* dest = json_object_get_string(json_object_object_get(jobj, "destination"));
            bool isDir = json_object_get_boolean(json_object_object_get(jobj, "isDir"));
            if (path == nullptr || path[0] == '\0' || dest == nullptr || dest[0] == '\0') {
                bad_request(res, "Missing path or destination");
                json_object_put(jobj);
                return;
            }
            
            BgDownloadData download_data;
            download_data.host_info.type = s.type;
            download_data.host_info.url = s.server;
            download_data.host_info.username = s.username;
            download_data.host_info.password = s.password;
            download_data.host_info.http_server_type = s.http_server_type;
            download_data.host_info.client = nullptr;
            download_data.src_path = path;
            
            std::string temp = std::string(path);
            size_t slash_pos = temp.find_last_of("/");
            std::string filename = temp.substr(slash_pos+1);
            download_data.dest_path = std::string(dest) + "/" + filename;
            
            download_data.file_size = 0;
            download_data.state = STATE_PENDING;
            download_data.id = Util::GetTick();
            download_data.bytes_transfered = 0;
            download_data.completed_bytes = 0;
            download_data.timestamp = Util::GetTick();
            download_data.finished_timestamp = 0;
            download_data.is_dir = isDir;
            
            CONFIG::AddBgDownloadData(download_data);
            CONFIG::SaveBgDownloadData();
            success(res);
            json_object_put(jobj);
        });

        svr->Post("/api/siteinstall", [&](const Request &req, Response &res) {
            json_object *jobj = json_tokener_parse(req.body.c_str());
            if (!jobj) { bad_request(res, "Invalid payload"); return; }
            json_object *site_idx_obj = json_object_object_get(jobj, "site_idx");
            json_object *items = json_object_object_get(jobj, "items");
            if (!site_idx_obj || !items || json_object_get_type(items) != json_type_array) { bad_request(res, "Missing parameters"); json_object_put(jobj); return; }
            int site_idx = json_object_get_int(site_idx_obj);
            if (site_idx < 0 || site_idx >= configured_sites.size()) { failed(res, 500, "Invalid site_idx"); json_object_put(jobj); return; }
            RemoteSettings& s = configured_sites[site_idx];
            
            dbglogger_log("[api] /api/sitelist getting client for site %d", site_idx);
        RemoteClient* client = GetRemoteClientForSite(site_idx);
            if (!client || !client->IsConnected()) {
                if (client) { client->Quit(); delete client; }
                failed(res, 500, "Connection failed"); json_object_put(jobj); return;
            }

            size_t len = json_object_array_length(items);
            if (len > 0) {
                const char *item = json_object_get_string(json_object_array_get_idx(items, 0));
                if (item == nullptr || item[0] == '\0') {
                    client->Quit(); delete client;
                    bad_request(res, "Invalid item path"); json_object_put(jobj); return;
                }
                std::string url = client->GetDirectUrl(item);
                if (!g_pkg_installer.StartRemoteInstall(client, url.c_str(), item, &s)) {
                    client->Quit(); delete client;
                    failed(res, 500, "Already installing"); json_object_put(jobj); return;
                }
            } else {
                client->Quit(); delete client;
            }
            success(res);
            json_object_put(jobj);
        });

        svr->Post("/api/sitemkdir", [&](const Request &req, Response &res) {
            json_object *jobj = json_tokener_parse(req.body.c_str());
            if (!jobj) { bad_request(res, "Invalid payload"); return; }
            json_object *site_idx_obj = json_object_object_get(jobj, "site_idx");
            json_object *path_obj = json_object_object_get(jobj, "newPath");
            if (!site_idx_obj || !path_obj) { bad_request(res, "Missing parameters"); json_object_put(jobj); return; }
            int site_idx = json_object_get_int(site_idx_obj);
            const char* path = json_object_get_string(path_obj);
            if (path == nullptr || path[0] == '\0') { bad_request(res, "Missing path"); json_object_put(jobj); return; }
            if (site_idx < 0 || site_idx >= configured_sites.size()) { failed(res, 500, "Invalid site_idx"); json_object_put(jobj); return; }
            RemoteClient *client = GetRemoteClientForSite(site_idx);
            if (!client || !client->IsConnected()) { if (client) { client->Quit(); delete client; } failed(res, 500, "Connection failed"); json_object_put(jobj); return; }
            int r = client->Mkdir(path);
            client->Quit(); delete client;
            if (r != 0) success(res); else failed(res, 500, "Mkdir failed");
            json_object_put(jobj);
        });

        svr->Post("/api/siterename", [&](const Request &req, Response &res) {
            json_object *jobj = json_tokener_parse(req.body.c_str());
            if (!jobj) { bad_request(res, "Invalid payload"); return; }
            json_object *site_idx_obj = json_object_object_get(jobj, "site_idx");
            json_object *oldPath_obj = json_object_object_get(jobj, "oldPath");
            json_object *newPath_obj = json_object_object_get(jobj, "newPath");
            if (!site_idx_obj || !oldPath_obj || !newPath_obj) { bad_request(res, "Missing parameters"); json_object_put(jobj); return; }
            int site_idx = json_object_get_int(site_idx_obj);
            const char* oldPath = json_object_get_string(oldPath_obj);
            const char* newPath = json_object_get_string(newPath_obj);
            if (oldPath == nullptr || oldPath[0] == '\0' || newPath == nullptr || newPath[0] == '\0') { bad_request(res, "Missing path"); json_object_put(jobj); return; }
            if (site_idx < 0 || site_idx >= configured_sites.size()) { failed(res, 500, "Invalid site_idx"); json_object_put(jobj); return; }
            RemoteClient *client = GetRemoteClientForSite(site_idx);
            if (!client || !client->IsConnected()) { if (client) { client->Quit(); delete client; } failed(res, 500, "Connection failed"); json_object_put(jobj); return; }
            int r = client->Rename(oldPath, newPath);
            client->Quit(); delete client;
            if (r != 0) success(res); else failed(res, 500, "Rename failed");
            json_object_put(jobj);
        });

        svr->Post("/api/siteremove", [&](const Request &req, Response &res) {
            json_object *jobj = json_tokener_parse(req.body.c_str());
            if (!jobj) { bad_request(res, "Invalid payload"); return; }
            json_object *site_idx_obj = json_object_object_get(jobj, "site_idx");
            json_object *items_obj = json_object_object_get(jobj, "items");
            if (!site_idx_obj || !items_obj || json_object_get_type(items_obj) != json_type_array) { bad_request(res, "Missing parameters"); json_object_put(jobj); return; }
            int site_idx = json_object_get_int(site_idx_obj);
            if (site_idx < 0 || site_idx >= configured_sites.size()) { failed(res, 500, "Invalid site_idx"); json_object_put(jobj); return; }
            RemoteClient *client = GetRemoteClientForSite(site_idx);
            if (!client || !client->IsConnected()) { if (client) { client->Quit(); delete client; } failed(res, 500, "Connection failed"); json_object_put(jobj); return; }
            bool all_success = true;
            size_t len = json_object_array_length(items_obj);
            for (size_t i=0; i<len; i++) {
                const char* item = json_object_get_string(json_object_array_get_idx(items_obj, i));
                if (item == nullptr || item[0] == '\0') { all_success = false; continue; }
                if (client->Delete(item) == 0) {
                    if (client->Rmdir(item, true) == 0) all_success = false;
                }
            }
            client->Quit(); delete client;
            if (all_success) success(res); else failed(res, 500, "Some removes failed");
            json_object_put(jobj);
        });

        svr->Post("/api/pkginfo", [&](const Request &req, Response &res) {
            json_object *jobj = json_tokener_parse(req.body.c_str());
            if (!jobj) { bad_request(res, "Invalid payload"); return; }
            const char *path = json_object_get_string(json_object_object_get(jobj, "path"));
            json_object *site_idx_obj = json_object_object_get(jobj, "site_idx");
            int site_idx = -1;
            if (site_idx_obj && json_object_get_type(site_idx_obj) != json_type_null) site_idx = json_object_get_int(site_idx_obj);
            if (!path) { bad_request(res, "Missing path"); json_object_put(jobj); return; }
            RemoteClient *client = nullptr;
            if (site_idx >= 0 && site_idx < configured_sites.size()) {
                client = GetRemoteClientForSite(site_idx);
                if (!client || !client->IsConnected()) { if (client) { client->Quit(); delete client; } failed(res, 500, "Connection failed"); json_object_put(jobj); return; }
            }
            std::map<std::string, std::string> sfo_params;
            if (!INSTALLER::GetPkgSfoInfo(path, client, sfo_params)) {
                if (client) { client->Quit(); delete client; }
                failed(res, 400, "Could not read PKG metadata"); json_object_put(jobj); return;
            }
            std::string title_id = sfo_params["TITLE_ID"];
            std::string icon_url = "";
            if (!title_id.empty()) {
                std::string icon_path = std::string("/data/homebrew/ezremote-client/game-icons/") + title_id + ".png";
                FS::MkDirs("/data/homebrew/ezremote-client/game-icons");
                if (!FS::FileExists(icon_path)) {
                    if (client) INSTALLER::ExtractRemotePkg(client, path, "/data/homebrew/ezremote-client/temp.sfo", icon_path);
                    else INSTALLER::ExtractLocalPkg(path, "/data/homebrew/ezremote-client/temp.sfo", icon_path);
                }
                if (FS::FileExists(icon_path)) icon_url = "/game-icons/" + title_id + ".png";
            }
            if (client) { client->Quit(); delete client; }
            json_object *res_obj = json_object_new_object();
            for (auto const& [key, val] : sfo_params) json_object_object_add(res_obj, key.c_str(), json_object_new_string(val.c_str()));
            if (!icon_url.empty()) json_object_object_add(res_obj, "ICON_URL", json_object_new_string(icon_url.c_str()));
            const char *res_str = json_object_to_json_string(res_obj);
            dbglogger_log("[api] /api/sitelist returning response");
        res.status = 200;
            res.set_content(res_str, strlen(res_str), "application/json");
            json_object_put(res_obj);
            json_object_put(jobj);
        });

        svr->Get("/api/extract/status", [&](const Request &req, Response &res) {
            // Forward to existing background extract state logic
            json_object *extract_list = json_object_new_array();
            CONFIG::LockExtractList();
            for (auto it = bg_extract_list.begin(); it != bg_extract_list.end(); ++it) {
                json_object *extract_item_obj = json_object_new_object();
                json_object_object_add(extract_item_obj, "state", json_object_new_int(it->state));
                json_object_array_add(extract_list, extract_item_obj);
            }
            CONFIG::UnlockExtractList();
            const char *payload_str = json_object_to_json_string(extract_list);
            dbglogger_log("[api] /api/sitelist returning response");
        res.status = 200;
            res.set_content(payload_str, strlen(payload_str), "application/json");
            json_object_put(extract_list);
        });
