#include "windows.h"
#include "lang.h"
#include "ime_dialog.h"
#include <string.h>

char status_message[256] = {0};
char activity_message[256] = {0};
char confirm_message[256] = {0};
bool activity_inprogess = false;
bool stop_activity = false;
int confirm_state = 0;
bool is_server_started = true;
bool ezremote_server_version_match = true;
uint64_t bytes_transfered = 0;
uint64_t bytes_to_download = 0;
uint64_t prev_tick = 0;
char remote_filter[256] = {0};
bool file_transfering = false;

RemoteSettings *remote_settings = nullptr;
RemoteClient *remoteclient = nullptr;
bool enable_direct_download_redirect = false;

char lang_strings[LANG_STRINGS_NUM][LANG_STR_SIZE] = {0};
char lang_identifiers[LANG_STRINGS_NUM][LANG_ID_SIZE] = {0};
bool needs_extended_font = false;

namespace Windows
{
    void SingleValueImeCallback(int ime_result) {}
    void AfterZipFileCallback(int ime_result) {}
    void SetModalMode(bool modal) {}
}

namespace Dialog
{
    int initImeDialog(const char *Title, const char *initialTextBuffer, int max_text_length, int type, float posx, float posy) { return 0; }
    int updateImeDialog() { return 0; }
    uint8_t *getImeDialogInputText() { return (uint8_t *)""; }
}

char CACERT_FILE[256] = "/data/homebrew/ezremote-client/cacert.pem";
bool show_hidden_files = false;
char realdebrid_api_key[64] = {0};
char alldebrid_api_key[64] = {0};

namespace Lang
{
    void SetTranslation(int32_t lang_idx) {}
}

int InitImeDialog(const char *title, const char *initial_text, int max_length, int type, int option, void (*handler)(int)) { return 0; }
int sites[256];
RemoteSettings site_settings[256];
