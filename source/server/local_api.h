
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
                json_object *results = json_object_new_object();
                json_object_object_add(results, "result", json_object_new_string(buf));
                const char *results_str = json_object_to_json_string(results);
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
        json_object *results = json_object_new_object();
        json_object *result_obj = json_object_new_object();
        json_object_object_add(result_obj, "exists", json_object_new_boolean(exists));
        json_object_object_add(results, "result", result_obj);
        const char *results_str = json_object_to_json_string(results);
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
    json_object *json_sites = json_object_new_array();
    for (size_t i = 0; i < configured_sites.size(); i++) {
        RemoteSettings& s = configured_sites[i];
        json_object *site = json_object_new_object();
        json_object_object_add(site, "name", json_object_new_string(s.site_name));
        json_object_object_add(site, "url", json_object_new_string(s.server));
        json_object_object_add(site, "username", json_object_new_string(s.username));
        json_object_object_add(site, "password", json_object_new_string(s.password));
        json_object_object_add(site, "type", json_object_new_int(s.type));
        json_object_array_add(json_sites, site);
    }
    json_object *results = json_object_new_object();
    json_object_object_add(results, "result", json_sites);
    const char *results_str = json_object_to_json_string(results);
    res.status = 200;
    res.set_content(results_str, strlen(results_str), "application/json");
    json_object_put(results);
});

svr->Post("/api/sitesave", [&](const Request &req, Response &res) {
    json_object *jobj = json_tokener_parse(req.body.c_str());
    if (jobj != nullptr) {
        RemoteSettings s;
        memset(&s, 0, sizeof(s));
        
        const char* name = json_object_get_string(json_object_object_get(jobj, "name"));
        const char* url = json_object_get_string(json_object_object_get(jobj, "url"));
        const char* username = json_object_get_string(json_object_object_get(jobj, "username"));
        const char* password = json_object_get_string(json_object_object_get(jobj, "password"));
        int type = json_object_get_int(json_object_object_get(jobj, "type"));
        
        if (name) strncpy(s.site_name, name, sizeof(s.site_name)-1);
        if (url) strncpy(s.server, url, sizeof(s.server)-1);
        if (username) strncpy(s.username, username, sizeof(s.username)-1);
        if (password) strncpy(s.password, password, sizeof(s.password)-1);
        s.type = (ClientType)type;
        
        configured_sites.push_back(s);
        success(res);
    } else {
        bad_request(res, "Invalid payload");
    }
    if (jobj) json_object_put(jobj);
});

svr->Post("/api/sitelist", [&](const Request &req, Response &res) {
    json_object *jobj = json_tokener_parse(req.body.c_str());
    if (jobj != nullptr) {
        int site_idx = json_object_get_int(json_object_object_get(jobj, "site_idx"));
        const char* path = json_object_get_string(json_object_object_get(jobj, "path"));
        
        if (site_idx < 0 || site_idx >= configured_sites.size() || !path) {
            bad_request(res, "Invalid parameters");
            json_object_put(jobj);
            return;
        }

        RemoteClient* client = GetRemoteClientForSite(site_idx);
        if (!client) {
            failed(res, 500, "Failed to initialize client");
            json_object_put(jobj);
            return;
        }
        
        std::vector<DirEntry> files = client->ListDir(path);
        DirEntry::Sort(files);
        
        json_object *json_files = json_object_new_array();
        for (auto& it : files) {
            json_object *new_file = json_object_new_object();
            char display_date[32];
            sprintf(display_date, "%04d-%02d-%02d %02d:%02d:%02d", it.modified.year, it.modified.month, it.modified.day, it.modified.hours, it.modified.minutes, it.modified.seconds);
            json_object_object_add(new_file, "name", json_object_new_string(it.name));
            json_object_object_add(new_file, "date", json_object_new_string(display_date));
            json_object_object_add(new_file, "size", json_object_new_string(it.isDir ? "" : std::to_string(it.file_size).c_str()));
            json_object_object_add(new_file, "type", json_object_new_string(it.isDir ? "dir" : "file"));
            json_object_array_add(json_files, new_file);
        }
        
        json_object *results = json_object_new_object();
        json_object_object_add(results, "result", json_files);
        const char *results_str = json_object_to_json_string(results);
        res.status = 200;
        res.set_content(results_str, strlen(results_str), "application/json");
        json_object_put(results);
        
        delete client;
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

                        new_file = destination + "/" + item.filename;
                        
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

                if (items == nullptr || destination == nullptr || compressedFilename == nullptr)
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
            std::string zip_file = std::string(destination) + "/" + compressedFilename;
            zipFile zf = zipOpen64(zip_file.c_str(), APPEND_STATUS_CREATE);
            if (zf != NULL)
            {
                size_t len = json_object_array_length(items);
                for (size_t i=0; i < len; i++)
                {
                    const char *item = json_object_get_string(json_object_array_get_idx(items, i));
                    std::string src = std::string(item);
                    size_t slash_pos = src.find_last_of("/");
                    int ret = ZipUtil::ZipAddPath(zf, src, (slash_pos != std::string::npos ? slash_pos + 1 : 1), Z_DEFAULT_COMPRESSION);
                    if (ret != 1)
                    {
                        zipClose(zf, NULL);
                        FS::Rm(zip_file);
                        failed(res, 200, "Failed to create zip");
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

            /* ExtractQueuedResponse(res, job_id); */
            json_object_put(jobj); });

svr->Post("/api/siteextract", [&](const Request &req, Response &res) {
            json_object *jobj = json_tokener_parse(req.body.c_str());
            if (!jobj) { bad_request(res, "Invalid payload"); return; }

            json_object *site_obj = json_object_object_get(jobj, "site_idx");
            const char *item = json_object_get_string(json_object_object_get(jobj, "item"));
            const char *folderName = json_object_get_string(json_object_object_get(jobj, "folderName"));
            if (site_obj == nullptr || item == nullptr)
            {
                bad_request(res, "Required site_idx or item parameter missing");
                json_object_put(jobj);
                return;
            }

            uint64_t job_id = 0;
            std::string error;
            int site_idx = json_object_get_int(site_obj);
            if (!StartExtractJob(site_idx, item, "/data", folderName != nullptr ? folderName : "", &error, &job_id))
            {
                failed(res, 200, error);
                json_object_put(jobj);
                return;
            }

            /* ExtractQueuedResponse(res, job_id); */
            json_object_put(jobj);
        });

