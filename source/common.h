#ifndef EZ_COMMON_H
#define EZ_COMMON_H

#include <string>
#include <vector>
#include <string.h>
#include <algorithm>
#include <lexbor/html/parser.h>
#include <lexbor/dom/interfaces/element.h>

#define HTTP_SUCCESS(x) (x >= 200 && x < 300)
#define MIN(a,b) (((a)<(b))?(a):(b))
#define MAX(a,b) (((a)>(b))?(a):(b))

enum DownloadState { STATE_PENDING, STATE_DOWNLOADING, STATE_RESUMED, STATE_FAILED, STATE_SUCCESS };

enum ExtractState { EXTRACT_STATE_PENDING, EXTRACT_STATE_EXTRACTING, EXTRACT_STATE_FAILED, EXTRACT_STATE_SUCCESS };

enum FileOpState { FILEOP_STATE_PENDING, FILEOP_STATE_PROCESSING, FILEOP_STATE_FAILED, FILEOP_STATE_SUCCESS };

enum FileOpType { FILEOP_COPY, FILEOP_MOVE, FILEOP_DELETE };

static const char* state_strings[] = {"Pending", "Downloading", "Resumed", "Failed", "Success"};

typedef struct
{
    uint16_t year;
    uint8_t month;
    uint8_t day;
    uint8_t dayOfWeek;
    uint8_t hours;
    uint8_t minutes;
    uint8_t seconds;
    uint32_t microsecond;
} DateTime;

struct DirEntry
{
    char directory[512];
    char name[256];
    char display_size[48];
    char display_date[48];
    char path[768];
    uint64_t file_size;
    bool isDir;
    bool isLink;
    DateTime modified;
    bool selectable;

    friend bool operator<(DirEntry const &a, DirEntry const &b)
    {
        return strcmp(a.name, b.name) < 0;
    }

    static void Sort(std::vector<DirEntry> &list, int sort_by = 0)
    {
        std::sort(list.begin(), list.end(), [sort_by](const DirEntry &p1, const DirEntry &p2) {
            if (strcasecmp(p1.name, "..") == 0) return strcasecmp(p2.name, "..") != 0;
            if (strcasecmp(p2.name, "..") == 0) return false;

            if (p1.isDir && !p2.isDir) return true;
            if (!p1.isDir && p2.isDir) return false;

            if (sort_by == 1) { // Size
                if (p1.isDir && p2.isDir) {
                    return strcasecmp(p1.name, p2.name) < 0;
                }
                if (p1.file_size == p2.file_size) return strcasecmp(p1.name, p2.name) < 0;
                return p1.file_size > p2.file_size; // descending
            }
            return strcasecmp(p1.name, p2.name) < 0;
        });
    }

    static void SetDisplaySize(DirEntry *entry)
    {
        if (entry->file_size < 1024)
        {
            sprintf(entry->display_size, "%ldB", entry->file_size);
        }
        else if (entry->file_size < 1024 * 1024)
        {
            sprintf(entry->display_size, "%.2fKB", entry->file_size * 1.0f / 1024);
        }
        else if (entry->file_size < 1024 * 1024 * 1024)
        {
            sprintf(entry->display_size, "%.2fMB", entry->file_size * 1.0f / (1024 * 1024));
        }
        else
        {
            sprintf(entry->display_size, "%.2fGB", entry->file_size * 1.0f / (1024 * 1024 * 1024));
        }
    }
};

struct DownloadProgress
{
    std::string path;
    std::string state;
    std::string fail_reason;
    uint64_t bytes_transfered;
    uint64_t file_size;
    time_t timestamp;
    bool cancel_requested = false;
};

struct BgFileOpData
{
    uint64_t id;
    FileOpType type;
    std::vector<std::string> items;
    std::string dest_path;
    std::string fail_reason;
    FileOpState state;
    uint64_t items_processed;
    uint64_t total_items;
    time_t timestamp;
    uint64_t finished_timestamp = 0;
    int retry_count = 0;
    bool cancel_requested = false;
};

static lxb_dom_node_t *NextChildElement(lxb_dom_element_t *element)
{
    lxb_dom_node_t *node = element->node.first_child;
    while (node != nullptr && node->type != LXB_DOM_NODE_TYPE_ELEMENT)
    {
        node = node->next;
    }
    return node;
}

static lxb_dom_node_t *NextElement(lxb_dom_node_t *node)
{
    lxb_dom_node_t *next = node->next;
    while (next != nullptr && next->type != LXB_DOM_NODE_TYPE_ELEMENT)
    {
        next = next->next;
    }
    return next;
}

static lxb_dom_node_t *NextChildTextNode(lxb_dom_element_t *element)
{
    lxb_dom_node_t *node = element->node.first_child;
    while (node != nullptr && node->type != LXB_DOM_NODE_TYPE_TEXT)
    {
        node = node->next;
    }
    return node;
}

static lxb_dom_node_t *NextTextNode(lxb_dom_node_t *node)
{
    lxb_dom_node_t *next = node->next;
    while (next != nullptr && next->type != LXB_DOM_NODE_TYPE_TEXT)
    {
        next = next->next;
    }
    return next;
}

#endif
