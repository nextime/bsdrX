/*
 * bsdrX — Bigscreen Remote Desktop agent.
 * Copyright (C) 2026 Stefy Lanza <stefy@nexlab.net>
 *
 * This program is free software: you can redistribute it and/or modify it under
 * the terms of the GNU General Public License, version 3 or (at your option) any
 * later version. See <https://www.gnu.org/licenses/>.
 */
/* webcam_macos.m — AVFoundation camera enumeration for the macOS source picker.
 *
 * webcam.c is pure C, but AVFoundation is Obj-C only, so the macOS camera list lives here (built
 * only into the osxcross target) and webcam.c's macOS branch forwards to bsdr_macos_camera_list().
 *
 * We return each camera's localizedName as BOTH the id and the label. ffmpeg's avfoundation input
 * accepts a device *name* as readily as a numeric index (it name-matches against this same device
 * list), and a name is stable across reboots/hotplug where the index is not — so the id is the name,
 * matching how the Windows/DirectShow path already keys on the friendly name. Enumeration itself
 * needs no Camera TCC grant (only opening the stream does), so the dropdown always populates. */
#import <AVFoundation/AVFoundation.h>
#include "bsdr/webcam.h"

int bsdr_macos_camera_list(bsdr_webcam_dev *out, int max) {
    if (!out || max <= 0) return 0;
    @autoreleasepool {
        /* devicesWithMediaType: is deprecated since 10.15 but returns every video device in exactly
         * the order ffmpeg's avfoundation enumerates, so name-matching stays consistent. The typed
         * DiscoverySession would silently omit device classes the running SDK predates. */
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
        NSArray *devs = [AVCaptureDevice devicesWithMediaType:AVMediaTypeVideo];
#pragma clang diagnostic pop
        int found = 0;
        for (AVCaptureDevice *d in devs) {
            if (found >= max) break;
            const char *nm = [[d localizedName] UTF8String];
            if (!nm || !nm[0]) nm = [[d uniqueID] UTF8String];
            if (!nm) continue;
            snprintf(out[found].id,   sizeof out[found].id,   "%s", nm);
            snprintf(out[found].name, sizeof out[found].name, "%s", nm);
            found++;
        }
        return found;
    }
}
