// SPDX-License-Identifier: MIT
// Copyright (c) 2026 the MYIOSDECK author
//
// Spec 02 Part C launch step 5: the shared audio session (Apple's AVAudioSession docs).

#import <AVFoundation/AVFoundation.h>

#include "wb_internal.h"
#include "wb_log.h"

static const char *describe(NSError *error)
{
    const char *s = error.localizedDescription.UTF8String;
    return s ? s : "unknown error";
}

void wb_audio_activate(void)
{
    @autoreleasepool {
        AVAudioSession *session = AVAudioSession.sharedInstance;
        NSError *error = nil;

        /* Playback: sound plays even with the silent switch on. */
        if (![session setCategory:AVAudioSessionCategoryPlayback error:&error]) {
            wb_log("wb-launch", "audio: setting the Playback category failed: %s", describe(error));
            error = nil;
        }
        if (![session setActive:YES error:&error]) {
            wb_log("wb-launch", "audio: activating the session failed: %s", describe(error));
            return;
        }
        wb_log("wb-launch", "audio: session active, %.0f Hz, output latency %.1f ms",
               session.sampleRate, session.outputLatency * 1000.0);
    }
}
