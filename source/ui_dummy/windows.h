#ifndef EZ_WINDOWS_DUMMY_H
#define EZ_WINDOWS_DUMMY_H

#include <string>
#include <vector>

extern char status_message[256];
extern char activity_message[256];
extern char confirm_message[256];
extern bool activity_inprogess;
extern bool stop_activity;
extern int confirm_state;
extern bool is_server_started;
extern bool ezremote_server_version_match;
extern uint64_t bytes_transfered;
extern uint64_t bytes_to_download;
extern uint64_t prev_tick;
extern char remote_filter[256];
extern bool file_transfering;

namespace Windows
{
    void SingleValueImeCallback(int ime_result);
    void AfterZipFileCallback(int ime_result);
    void SetModalMode(bool modal);
}

#endif
