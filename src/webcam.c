/*
 * bsdrX — Bigscreen Remote Desktop agent.
 * Copyright (C) 2026 Stefy Lanza <stefy@nexlab.net>
 *
 * This program is free software: you can redistribute it and/or modify it under
 * the terms of the GNU General Public License, version 3 or (at your option) any
 * later version. See <https://www.gnu.org/licenses/>.
 */
/* webcam.c — cross-platform camera enumeration for the source picker. See webcam.h.
 * Linux = V4L2 scan of /dev/video*; Windows = DirectShow COM; macOS = handled by avfoundation
 * indices (best-effort stub — the web UI lets the operator type an index too); Android enumerates
 * in Kotlin. */
#include "bsdr/webcam.h"
#include "bsdr/log.h"

#include <string.h>
#include <stdio.h>

#if defined(__linux__) && !defined(__ANDROID__)
#include <dirent.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <linux/videodev2.h>

/* Scan /dev/video* and keep the nodes that can actually capture video (UVC cams expose several
 * nodes per device — a capture node plus metadata nodes; only the former has VIDEO_CAPTURE). */
int bsdr_webcam_list(bsdr_webcam_dev *out, int max) {
    if (!out || max <= 0) return 0;
    /* Collect and sort the videoN indices so /dev/video0 comes before video10. */
    int idx[128], n = 0;
    DIR *d = opendir("/dev");
    if (d) {
        struct dirent *e;
        while ((e = readdir(d)) && n < 128) {
            int num;
            if (sscanf(e->d_name, "video%d", &num) == 1) idx[n++] = num;
        }
        closedir(d);
    }
    for (int i = 0; i < n; i++)          /* simple insertion sort (n is tiny) */
        for (int j = i + 1; j < n; j++)
            if (idx[j] < idx[i]) { int t = idx[i]; idx[i] = idx[j]; idx[j] = t; }

    int found = 0;
    for (int i = 0; i < n && found < max; i++) {
        char path[64];
        snprintf(path, sizeof path, "/dev/video%d", idx[i]);
        int fd = open(path, O_RDONLY | O_NONBLOCK);
        if (fd < 0) continue;
        struct v4l2_capability cap;
        memset(&cap, 0, sizeof cap);
        int ok = ioctl(fd, VIDIOC_QUERYCAP, &cap) == 0;
        close(fd);
        if (!ok) continue;
        unsigned caps = (cap.capabilities & V4L2_CAP_DEVICE_CAPS) ? cap.device_caps : cap.capabilities;
        if (!(caps & V4L2_CAP_VIDEO_CAPTURE)) continue;   /* skip metadata / output-only nodes */
        snprintf(out[found].id, sizeof out[found].id, "%s", path);
        snprintf(out[found].name, sizeof out[found].name, "%s (%s)", (const char *)cap.card, path);
        found++;
    }
    return found;
}

#elif defined(_WIN32)
#define COBJMACROS
#include <windows.h>
#include <dshow.h>
/* DirectShow video-input enumeration. Returns each device's friendly name as the id (opened later as
 * "video=<name>" by the dshow input). */
int bsdr_webcam_list(bsdr_webcam_dev *out, int max) {
    if (!out || max <= 0) return 0;
    int found = 0;
    HRESULT hrco = CoInitializeEx(NULL, COINIT_MULTITHREADED);
    ICreateDevEnum *sysdev = NULL;
    if (FAILED(CoCreateInstance(&CLSID_SystemDeviceEnum, NULL, CLSCTX_INPROC_SERVER,
                                &IID_ICreateDevEnum, (void **)&sysdev)))
        goto done;
    IEnumMoniker *en = NULL;
    if (ICreateDevEnum_CreateClassEnumerator(sysdev, &CLSID_VideoInputDeviceCategory, &en, 0) != S_OK)
        goto done;
    IMoniker *mon = NULL;
    while (found < max && IEnumMoniker_Next(en, 1, &mon, NULL) == S_OK) {
        IPropertyBag *bag = NULL;
        if (SUCCEEDED(IMoniker_BindToStorage(mon, NULL, NULL, &IID_IPropertyBag, (void **)&bag))) {
            VARIANT v; VariantInit(&v);
            if (SUCCEEDED(IPropertyBag_Read(bag, L"FriendlyName", &v, NULL))) {
                char nm[128] = "";
                WideCharToMultiByte(CP_UTF8, 0, v.bstrVal, -1, nm, sizeof nm, NULL, NULL);
                snprintf(out[found].id, sizeof out[found].id, "%s", nm);
                snprintf(out[found].name, sizeof out[found].name, "%s", nm);
                found++;
                VariantClear(&v);
            }
            IPropertyBag_Release(bag);
        }
        IMoniker_Release(mon);
    }
    IEnumMoniker_Release(en);
done:
    if (sysdev) ICreateDevEnum_Release(sysdev);
    if (SUCCEEDED(hrco)) CoUninitialize();
    return found;
}

#elif defined(__ANDROID__)
/* Android enumerates cameras in Kotlin (CameraManager) and pushes the list to native; return it. */
extern int bsdr_android_cameras(bsdr_webcam_dev *out, int max);
int bsdr_webcam_list(bsdr_webcam_dev *out, int max) {
    return bsdr_android_cameras(out, max);
}

#else  /* macOS + anything else: avfoundation addresses cameras by numeric index and enumerating
        * their names needs AVFoundation (Obj-C), which we don't pull in here. Report none; the web UI
        * exposes a manual "camera index" field on this platform so the operator can still pick one. */
int bsdr_webcam_list(bsdr_webcam_dev *out, int max) {
    (void)out; (void)max;
    return 0;
}
#endif
