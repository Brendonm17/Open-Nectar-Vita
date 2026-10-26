#ifndef PC_FILE_DIALOG_H
#define PC_FILE_DIALOG_H

#include <stddef.h>

// Native "open file" dialog for the desktop builds: the shell dialog on
// Windows, zenity or kdialog on Linux/BSD, osascript's chooser on macOS. The
// title gets the same "Open Nectar - " prefix as the launcher's dialogs.
//
// It does not block the game. pc_file_dialog_begin() opens the dialog and
// returns at once; the caller then calls pc_file_dialog_poll() once a frame
// until it returns PC_FILE_DIALOG_DONE. On macOS and Linux the dialog is a
// child process, so the game keeps drawing while it is open. On Windows the
// dialog is modal and begin() waits for it, so poll() answers on the next call.
//
// Only one dialog can be open: begin() returns false while another is.
// `tag` is the caller's own number for the request; poll() hands it back.
// `patterns` is a space-separated glob list ("*.zip *.rar").

enum PcFileDialogState {
    PC_FILE_DIALOG_IDLE,    // nothing open and no answer waiting
    PC_FILE_DIALOG_PENDING, // still open
    PC_FILE_DIALOG_DONE,    // answered: see `chosen` and `out`
};

bool pc_file_dialog_begin(int tag, const char* title, const char* filterName, const char* patterns);

// On DONE: `*tag` is the request's tag; `*chosen` is true with the path in
// `out`, or false when cancelled. When no dialog is available (Linux without
// zenity or kdialog) `*chosen` is false and `out` holds a short reason for the
// menu to show; otherwise `out` is empty on cancel. The answer is handed out once.
PcFileDialogState pc_file_dialog_poll(int* tag, bool* chosen, char* out, size_t outSize);

bool pc_file_dialog_busy(void);

// Closes an open dialog without an answer and drops any answer not yet
// polled. Also runs at exit, so quitting the game never leaves a picker open.
void pc_file_dialog_close(void);

#if !defined(_WIN32) && !defined(__ANDROID__)
// For the tests: run `shellCommand` as the dialog. Its stdout is the chosen
// path; no output means cancelled.
bool pc_file_dialog_begin_command(int tag, const char* shellCommand);
// For the tests: the command begin() would run, or "" when there is no dialog.
void pc_file_dialog_command(const char* title, const char* filterName, const char* patterns, char* out, size_t outSize);
#endif

#endif
