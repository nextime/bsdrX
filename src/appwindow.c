/*
 * bsdrX — Bigscreen Remote Desktop agent.
 * Copyright (C) 2026 Stefy Lanza <stefy@nexlab.net>
 *
 * This program is free software: you can redistribute it and/or modify it under
 * the terms of the GNU General Public License as published by the Free Software
 * Foundation, either version 3 of the License, or (at your option) any later
 * version.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
 * FOR A PARTICULAR PURPOSE. See the GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along with
 * this program. If not, see <https://www.gnu.org/licenses/>.
 */
#include "bsdr/appwindow.h"
#include "bsdr/platform.h"
#include "bsdr/log.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(BSDR_PLATFORM_ANDROID)
/* Android drives the panel through an in-app WebView — there is no external
 * browser window or system tray, so this is a no-op (never called anyway, since
 * the JNI bridge sets open_browser=false). */
bsdr_appwindow *bsdr_appwindow_start(uint16_t port, bool app_mode,
                                     void (*on_quit)(void *), void *user) {
    (void)port; (void)app_mode; (void)on_quit; (void)user; return NULL;
}
void bsdr_appwindow_stop(bsdr_appwindow *w) { (void)w; }
#else

/* The control-panel URL. ?app=1 is a UI hint (the web server serves "/" for it);
 * the flags keep a fresh profile quiet (no first-run tabs / default-browser nag). */
static void appwin_url(char *out, size_t n, uint16_t port) {
    snprintf(out, n, "http://127.0.0.1:%u/?app=1", (unsigned)port);
}

struct bsdr_appwindow {
    uint16_t port;
    bool     app_mode;
    void   (*on_quit)(void *);
    void    *user;
    volatile int stop;         /* teardown requested (external) */
    int      tray_quit;        /* Quit was chosen from the tray */
    bsdr_thread *thr;
#if defined(_WIN32)
    void    *hwnd;             /* HWND of the hidden tray message window */
    void    *child;            /* HANDLE of the current window process (NULL = closed) */
    int      have_exe;
    char     exe[512];
    char     udd[512];
#else
    int      child;            /* pid of the tracked window (>0), or <=0 if none */
    char    *exe;              /* browser argv[0] (NULL = untracked/default browser) */
    char     udd[1024];        /* dedicated --user-data-dir */
#endif
};

/* ======================================================================== *
 *  Windows: system-tray icon + tracked chromeless window                    *
 * ======================================================================== */
#if defined(_WIN32)
#include <windows.h>
#include <shellapi.h>

#define WM_BSDR_TRAY (WM_APP + 1)
#define IDM_OPEN 1
#define IDM_QUIT 2

/* Only one app window exists per process; the window proc reaches it here. */
static bsdr_appwindow *g_win;

static int win_child_alive(bsdr_appwindow *w) {
    return w->child && WaitForSingleObject((HANDLE)w->child, 0) == WAIT_TIMEOUT;
}

/* (Re)open the panel window. Launches the tracked app-window child if we have a
 * Chromium exe; otherwise opens the default browser (untracked, new tab). */
static void win_open(bsdr_appwindow *w) {
    if (win_child_alive(w)) return;           /* already open */
    if (w->child) { CloseHandle((HANDLE)w->child); w->child = NULL; }

    char url[128]; appwin_url(url, sizeof url, w->port);
    if (w->have_exe) {
        char cmd[2048];   /* exe(512) + udd(512) + url(128) + flags, with headroom */
        snprintf(cmd, sizeof cmd,
                 "\"%s\" --user-data-dir=\"%s\" --app=%s --no-first-run --no-default-browser-check",
                 w->exe, w->udd, url);
        STARTUPINFOA si; PROCESS_INFORMATION pi;
        memset(&si, 0, sizeof si); si.cb = sizeof si;
        memset(&pi, 0, sizeof pi);
        if (CreateProcessA(NULL, cmd, NULL, NULL, FALSE, 0, NULL, NULL, &si, &pi)) {
            CloseHandle(pi.hThread);
            w->child = pi.hProcess;
        } else {
            BSDR_WARN("bsdr.appwindow", "could not launch app window (err %lu); opening default browser",
                      (unsigned long)GetLastError());
            ShellExecuteA(NULL, "open", url, NULL, NULL, SW_SHOWNORMAL);
        }
    } else {
        ShellExecuteA(NULL, "open", url, NULL, NULL, SW_SHOWNORMAL);
    }
}

static LRESULT CALLBACK win_proc(HWND h, UINT m, WPARAM wp, LPARAM lp) {
    bsdr_appwindow *w = g_win;
    switch (m) {
    case WM_BSDR_TRAY:
        if (LOWORD(lp) == WM_RBUTTONUP || LOWORD(lp) == WM_CONTEXTMENU) {
            POINT p; GetCursorPos(&p);
            HMENU mn = CreatePopupMenu();
            AppendMenuA(mn, MF_STRING, IDM_OPEN, "Open bsdrX");
            AppendMenuA(mn, MF_SEPARATOR, 0, NULL);
            AppendMenuA(mn, MF_STRING, IDM_QUIT, "Quit bsdrX");
            SetForegroundWindow(h);           /* so the menu dismisses on click-away */
            TrackPopupMenu(mn, TPM_RIGHTBUTTON, p.x, p.y, 0, h, NULL);
            DestroyMenu(mn);
        } else if (LOWORD(lp) == WM_LBUTTONDBLCLK) {
            win_open(w);
        }
        return 0;
    case WM_COMMAND:
        if (LOWORD(wp) == IDM_OPEN) { win_open(w); return 0; }
        if (LOWORD(wp) == IDM_QUIT) { w->tray_quit = 1; w->stop = 1; PostQuitMessage(0); return 0; }
        break;
    }
    return DefWindowProcA(h, m, wp, lp);
}

/* Probe the usual install locations for Edge (system) then Chrome (system/user). */
static int win_find_browser(char *out, size_t n) {
    static const char *rel[] = {
        "\\Microsoft\\Edge\\Application\\msedge.exe",
        "\\Google\\Chrome\\Application\\chrome.exe",
        NULL
    };
    static const char *envs[] = { "ProgramFiles(x86)", "ProgramFiles", "LOCALAPPDATA", NULL };
    for (int e = 0; envs[e]; e++) {
        const char *base = getenv(envs[e]);
        if (!base || !*base) continue;
        for (int r = 0; rel[r]; r++) {
            snprintf(out, n, "%s%s", base, rel[r]);
            if (GetFileAttributesA(out) != INVALID_FILE_ATTRIBUTES) return 1;
        }
    }
    return 0;
}

static void win_thread(void *arg) {
    bsdr_appwindow *w = arg;
    g_win = w;

    WNDCLASSA wc; memset(&wc, 0, sizeof wc);
    wc.lpfnWndProc   = win_proc;
    wc.hInstance     = GetModuleHandleA(NULL);
    wc.lpszClassName = "bsdrXTrayWnd";
    RegisterClassA(&wc);
    HWND hwnd = CreateWindowA("bsdrXTrayWnd", "bsdrX", 0, 0, 0, 0, 0,
                              HWND_MESSAGE, NULL, wc.hInstance, NULL);
    w->hwnd = hwnd;

    NOTIFYICONDATAA nid; memset(&nid, 0, sizeof nid);
    nid.cbSize = sizeof nid;
    nid.hWnd   = hwnd;
    nid.uID    = 1;
    nid.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
    nid.uCallbackMessage = WM_BSDR_TRAY;
    nid.hIcon  = LoadIconA(wc.hInstance, MAKEINTRESOURCEA(1));   /* the app icon (rc ID 1) */
    if (!nid.hIcon) nid.hIcon = LoadIconA(NULL, IDI_APPLICATION);
    strcpy(nid.szTip, "bsdrX — Bigscreen Remote Desktop");
    Shell_NotifyIconA(NIM_ADD, &nid);

    win_open(w);                              /* first launch of the panel window */

    for (;;) {
        HANDLE handles[1]; DWORD nCount = 0;
        if (w->child) handles[nCount++] = (HANDLE)w->child;
        DWORD r = MsgWaitForMultipleObjects(nCount, handles, FALSE, INFINITE, QS_ALLINPUT);
        if (nCount && r == WAIT_OBJECT_0) {   /* the window process exited -> stay in tray */
            CloseHandle((HANDLE)w->child);
            w->child = NULL;
            continue;
        }
        MSG msg;
        while (PeekMessageA(&msg, NULL, 0, 0, PM_REMOVE)) {
            if (msg.message == WM_QUIT) goto done;
            TranslateMessage(&msg);
            DispatchMessageA(&msg);
        }
        if (w->stop) goto done;
    }
done:
    Shell_NotifyIconA(NIM_DELETE, &nid);
    if (w->child) { TerminateProcess((HANDLE)w->child, 0); CloseHandle((HANDLE)w->child); w->child = NULL; }
    if (hwnd) { DestroyWindow(hwnd); w->hwnd = NULL; }
    if (w->tray_quit && w->on_quit) w->on_quit(w->user);   /* Quit chosen from the tray */
}

bsdr_appwindow *bsdr_appwindow_start(uint16_t port, bool app_mode,
                                     void (*on_quit)(void *), void *user) {
    char url[128]; appwin_url(url, sizeof url, port);
    if (!app_mode) {                          /* plain browser: open once, no tray/tracking */
        ShellExecuteA(NULL, "open", url, NULL, NULL, SW_SHOWNORMAL);
        return NULL;
    }
    bsdr_appwindow *w = calloc(1, sizeof *w);
    if (!w) return NULL;
    w->port = port; w->app_mode = true; w->on_quit = on_quit; w->user = user;
    w->have_exe = win_find_browser(w->exe, sizeof w->exe);
    const char *la = getenv("LOCALAPPDATA");
    snprintf(w->udd, sizeof w->udd, "%s\\bsdrX\\webview", (la && *la) ? la : ".");
    w->thr = bsdr_thread_start(win_thread, w);
    if (!w->thr) { free(w); return NULL; }
    return w;
}

void bsdr_appwindow_stop(bsdr_appwindow *w) {
    if (!w) return;
    w->stop = 1;
    if (w->hwnd) PostMessageA((HWND)w->hwnd, WM_NULL, 0, 0);   /* wake the loop; it re-checks stop */
    if (w->thr) bsdr_thread_join(w->thr);
    free(w);
}

/* ======================================================================== *
 *  POSIX (Linux/macOS): tracked chromeless window                           *
 *   - Linux: a StatusNotifier tray via AppIndicator IF one is available     *
 *     (dlopen'd at runtime, no build dependency); closing the window keeps   *
 *     the app in the tray with an Open/Quit menu.                            *
 *   - No tray available (or macOS): closing the tracked window quits.        *
 * ======================================================================== */
#else
#include <unistd.h>
#include <sys/wait.h>
#include <signal.h>

/* Locate a Chromium-family browser. On Linux they're on PATH; on macOS they
 * live inside .app bundles, so probe the standard bundle exe paths. Returns a
 * malloc'd path the caller frees, or NULL. */
static char *posix_find_browser(void) {
#if defined(__APPLE__)
    static const char *cand[] = {
        "/Applications/Google Chrome.app/Contents/MacOS/Google Chrome",
        "/Applications/Microsoft Edge.app/Contents/MacOS/Microsoft Edge",
        "/Applications/Chromium.app/Contents/MacOS/Chromium",
        NULL
    };
    for (int i = 0; cand[i]; i++)
        if (access(cand[i], X_OK) == 0) return strdup(cand[i]);
    return NULL;
#else
    static const char *cand[] = { "chromium", "google-chrome", "chromium-browser", "chrome", NULL };
    const char *path = getenv("PATH");
    if (!path) return NULL;
    for (int i = 0; cand[i]; i++) {
        char *p = strdup(path), *save = NULL;
        for (char *dir = strtok_r(p, ":", &save); dir; dir = strtok_r(NULL, ":", &save)) {
            char full[1024];
            snprintf(full, sizeof full, "%s/%s", dir, cand[i]);
            if (access(full, X_OK) == 0) { free(p); return strdup(full); }
        }
        free(p);
    }
    return NULL;
#endif
}

/* Per-user profile dir so the browser opens a NEW instance we can track. */
static void posix_udd(char *out, size_t n) {
#if defined(__APPLE__)
    const char *home = getenv("HOME");
    snprintf(out, n, "%s/Library/Application Support/bsdrX/webview", home ? home : ".");
#else
    const char *xdg = getenv("XDG_CONFIG_HOME");
    if (xdg && *xdg) snprintf(out, n, "%s/bsdrX/webview", xdg);
    else { const char *home = getenv("HOME"); snprintf(out, n, "%s/.config/bsdrX/webview", home ? home : "."); }
#endif
}

static int posix_child_alive(bsdr_appwindow *w) {
    if (w->child <= 0) return 0;
    int st;
    pid_t r = waitpid(w->child, &st, WNOHANG);   /* reap if it already exited */
    if (r == w->child || (r < 0)) { w->child = -1; return 0; }
    return 1;
}

/* (Re)launch the tracked app window. No-op if it's already open. */
static void posix_open(bsdr_appwindow *w) {
    char url[128]; appwin_url(url, sizeof url, w->port);
    if (!w->exe) {                               /* untracked: default browser */
        char cmd[256];
#if defined(__APPLE__)
        snprintf(cmd, sizeof cmd, "open %s >/dev/null 2>&1 &", url);
#else
        snprintf(cmd, sizeof cmd, "xdg-open %s >/dev/null 2>&1 &", url);
#endif
        if (system(cmd) != 0) { /* user opens it manually */ }
        return;
    }
    if (posix_child_alive(w)) return;
    char appflag[160]; snprintf(appflag, sizeof appflag, "--app=%s", url);
    char uddflag[1100]; snprintf(uddflag, sizeof uddflag, "--user-data-dir=%s", w->udd);
    pid_t pid = fork();
    if (pid < 0) return;
    if (pid == 0) {
        setsid();
        char *av[] = { w->exe, uddflag, appflag,
                       (char *)"--no-first-run", (char *)"--no-default-browser-check", NULL };
        execv(w->exe, av);
        _exit(127);
    }
    w->child = pid;
}

/* ---- Linux StatusNotifier tray via AppIndicator (dlopen, optional) -------- */
#if !defined(__APPLE__)
#include <dlfcn.h>

typedef int gboolean;
typedef void (*GCallback)(void);
struct bsdr_tray_fns {
    void *gtk, *gobj, *glib, *ind;
    gboolean (*gtk_init_check)(int *, char ***);
    void *(*gtk_menu_new)(void);
    void *(*gtk_menu_item_new_with_label)(const char *);
    void *(*gtk_separator_menu_item_new)(void);
    void  (*gtk_menu_shell_append)(void *, void *);
    void  (*gtk_widget_show)(void *);
    void  (*gtk_widget_show_all)(void *);
    void  (*gtk_main)(void);
    void  (*gtk_main_quit)(void);
    unsigned long (*g_signal_connect_data)(void *, const char *, GCallback, void *, void *, int);
    unsigned (*g_timeout_add)(unsigned, gboolean (*)(void *), void *);
    void *(*app_indicator_new)(const char *, const char *, int);
    void  (*app_indicator_set_status)(void *, int);
    void  (*app_indicator_set_menu)(void *, void *);
    void  (*app_indicator_set_title)(void *, const char *);
};
static struct bsdr_tray_fns TF;
static bsdr_appwindow *g_tray_w;

#define TFSYM(h, name) (*(void **)&TF.name = dlsym(TF.h, #name))

static int tray_load(void) {
    TF.gtk  = dlopen("libgtk-3.so.0", RTLD_NOW | RTLD_GLOBAL);
    TF.gobj = dlopen("libgobject-2.0.so.0", RTLD_NOW | RTLD_GLOBAL);
    TF.glib = dlopen("libglib-2.0.so.0", RTLD_NOW | RTLD_GLOBAL);
    TF.ind  = dlopen("libayatana-appindicator3.so.1", RTLD_NOW | RTLD_GLOBAL);
    if (!TF.ind) TF.ind = dlopen("libappindicator3.so.1", RTLD_NOW | RTLD_GLOBAL);
    if (!TF.gtk || !TF.gobj || !TF.glib || !TF.ind) return 0;
    TFSYM(gtk, gtk_init_check);
    TFSYM(gtk, gtk_menu_new);
    TFSYM(gtk, gtk_menu_item_new_with_label);
    TFSYM(gtk, gtk_separator_menu_item_new);
    TFSYM(gtk, gtk_menu_shell_append);
    TFSYM(gtk, gtk_widget_show);
    TFSYM(gtk, gtk_widget_show_all);
    TFSYM(gtk, gtk_main);
    TFSYM(gtk, gtk_main_quit);
    TFSYM(gobj, g_signal_connect_data);
    TFSYM(glib, g_timeout_add);
    TFSYM(ind, app_indicator_new);
    TFSYM(ind, app_indicator_set_status);
    TFSYM(ind, app_indicator_set_menu);
    TFSYM(ind, app_indicator_set_title);
    return TF.gtk_init_check && TF.gtk_menu_new && TF.gtk_menu_item_new_with_label &&
           TF.gtk_menu_shell_append && TF.gtk_widget_show_all && TF.gtk_main &&
           TF.gtk_main_quit && TF.g_signal_connect_data && TF.g_timeout_add &&
           TF.app_indicator_new && TF.app_indicator_set_status && TF.app_indicator_set_menu;
}

static void tray_open_cb(void *item, void *user) { (void)item; posix_open((bsdr_appwindow *)user); }
static void tray_quit_cb(void *item, void *user) {
    (void)item; bsdr_appwindow *w = user;
    w->tray_quit = 1; w->stop = 1; TF.gtk_main_quit();
}
/* Runs on the GTK thread: reap the window if it closed, and honor an external stop. */
static gboolean tray_poll_cb(void *user) {
    bsdr_appwindow *w = user;
    posix_child_alive(w);                 /* reap a closed window so Open works again */
    if (w->stop) { TF.gtk_main_quit(); return 0; }
    return 1;
}

/* Try to run a tray. Returns 1 if a tray ran (and has now exited), 0 if no tray
 * is available (caller falls back to close->quit). */
static int tray_run(bsdr_appwindow *w) {
    if (!tray_load()) return 0;
    int argc = 0; char **argv = NULL;
    if (!TF.gtk_init_check(&argc, &argv)) return 0;   /* no display / GTK unusable */
    void *ind = TF.app_indicator_new("bsdrX", "video-display", 0 /*APPLICATION_STATUS*/);
    if (!ind) return 0;
    g_tray_w = w;
    void *menu = TF.gtk_menu_new();
    void *mi_open = TF.gtk_menu_item_new_with_label("Open bsdrX");
    void *mi_sep  = TF.gtk_separator_menu_item_new ? TF.gtk_separator_menu_item_new() : NULL;
    void *mi_quit = TF.gtk_menu_item_new_with_label("Quit bsdrX");
    TF.g_signal_connect_data(mi_open, "activate", (GCallback)tray_open_cb, w, NULL, 0);
    TF.g_signal_connect_data(mi_quit, "activate", (GCallback)tray_quit_cb, w, NULL, 0);
    TF.gtk_menu_shell_append(menu, mi_open);
    if (mi_sep) TF.gtk_menu_shell_append(menu, mi_sep);
    TF.gtk_menu_shell_append(menu, mi_quit);
    TF.gtk_widget_show_all(menu);
    TF.app_indicator_set_status(ind, 1 /*ACTIVE*/);
    if (TF.app_indicator_set_title) TF.app_indicator_set_title(ind, "bsdrX");
    TF.app_indicator_set_menu(ind, menu);
    TF.g_timeout_add(300, tray_poll_cb, w);   /* reap window + watch for external stop */
    TF.gtk_main();                            /* blocks until quit */
    return 1;
}
#else
static int tray_run(bsdr_appwindow *w) { (void)w; return 0; }   /* macOS: no tray yet */
#endif

static void posix_thread(void *arg) {
    bsdr_appwindow *w = arg;
    uint64_t launched = bsdr_now_ms();
    posix_open(w);                            /* launch the window */

    if (tray_run(w)) {                        /* a tray ran and has now exited */
        if (w->child > 0) { kill(w->child, SIGTERM); waitpid(w->child, NULL, 0); w->child = -1; }
        if (w->tray_quit && w->on_quit) w->on_quit(w->user);
        return;
    }

    /* No tray available: closing the window quits the app (the requested fallback). */
    if (w->child <= 0) return;                /* untracked (default browser): nothing to watch */
    int st;
    while (waitpid(w->child, &st, 0) < 0 && !w->stop) { /* EINTR: retry */ }
    if (w->stop) return;
    /* Distinguish a real user close from a failed launch: a browser that dies within a couple of
     * seconds never really opened (no display, crash, single-instance handoff) — don't quit the
     * agent over that; open the default browser once and keep running (panel still reachable). */
    if (bsdr_now_ms() - launched < 2500) {
        char url[128]; appwin_url(url, sizeof url, w->port);
        char cmd[256];
#if defined(__APPLE__)
        snprintf(cmd, sizeof cmd, "open %s >/dev/null 2>&1 &", url);
#else
        snprintf(cmd, sizeof cmd, "xdg-open %s >/dev/null 2>&1 &", url);
#endif
        if (system(cmd) != 0) { /* user opens it manually */ }
        return;
    }
    if (w->on_quit) w->on_quit(w->user);
}

bsdr_appwindow *bsdr_appwindow_start(uint16_t port, bool app_mode,
                                     void (*on_quit)(void *), void *user) {
    bsdr_appwindow *w = calloc(1, sizeof *w);
    if (!w) return NULL;
    w->port = port; w->app_mode = app_mode; w->on_quit = on_quit; w->user = user; w->child = -1;
    w->exe = app_mode ? posix_find_browser() : NULL;   /* NULL -> default browser, untracked */
    posix_udd(w->udd, sizeof w->udd);
    w->thr = bsdr_thread_start(posix_thread, w);
    if (!w->thr) { free(w->exe); free(w); return NULL; }
    return w;
}

void bsdr_appwindow_stop(bsdr_appwindow *w) {
    if (!w) return;
    w->stop = 1;
    if (w->child > 0) kill(w->child, SIGTERM);   /* wakes waitpid; the tray loop polls w->stop */
    if (w->thr) bsdr_thread_join(w->thr);
    free(w->exe);
    free(w);
}
#endif /* !_WIN32 */

#endif /* BSDR_PLATFORM_ANDROID */
