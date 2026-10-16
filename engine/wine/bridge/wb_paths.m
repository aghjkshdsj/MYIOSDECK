// SPDX-License-Identifier: MIT
// Copyright (c) 2026 the MYIOSDECK author
//
// The app's directories as C strings, computed once. The wineserver and launch threads set
// HOME to the Wine prefix, so the values are taken early (a constructor below) and cached.

#import <Foundation/Foundation.h>

#include "wb_internal.h"

#include <stdlib.h>
#include <string.h>

static char *g_documents;
static char *g_bundle;
static char *g_support;

static char *copy_path(NSString *path)
{
    if (path.length == 0) return NULL;
    return strdup(path.fileSystemRepresentation);
}

static char *home_child(const char *child)
{
    const char *home = getenv("HOME");
    char buf[1024];

    if (!home || !*home) return NULL;
    if (wb_join(buf, sizeof(buf), "%s/%s", home, child) != 0) return NULL;
    return strdup(buf);
}

static void init_paths(void)
{
    @autoreleasepool {
        NSFileManager *fm = NSFileManager.defaultManager;
        NSURL *docs = [fm URLsForDirectory:NSDocumentDirectory inDomains:NSUserDomainMask].firstObject;
        NSURL *support = [fm URLsForDirectory:NSApplicationSupportDirectory
                                    inDomains:NSUserDomainMask].firstObject;

        g_documents = copy_path(docs.path);
        if (!g_documents) {
            const char *env = getenv("MADEIRA_DOCS_DIR");
            if (env && *env) g_documents = strdup(env);
        }
        if (!g_documents) g_documents = home_child("Documents");
        if (!g_documents) g_documents = strdup(".");

        g_support = copy_path(support.path);
        if (!g_support) g_support = home_child("Library/Application Support");

        g_bundle = copy_path(NSBundle.mainBundle.bundlePath);
        if (!g_bundle) g_bundle = strdup(".");
    }
}

static dispatch_once_t g_once;

const char *wb_documents_dir(void)
{
    dispatch_once(&g_once, ^{ init_paths(); });
    return g_documents;
}

const char *wb_bundle_dir(void)
{
    dispatch_once(&g_once, ^{ init_paths(); });
    return g_bundle;
}

const char *wb_app_support_dir(void)
{
    dispatch_once(&g_once, ^{ init_paths(); });
    return g_support;
}

__attribute__((constructor)) static void wb_paths_prime(void)
{
    (void)wb_documents_dir();
}
