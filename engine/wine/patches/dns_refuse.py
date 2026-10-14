#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Host names that resolve to nothing at once, for a Madeira Dock session without JIT.

Without JIT Valve's Steam client cannot finish a TLS handshake in time: its HTTPS request to the
CM directory (api.steampowered.com) either times out after half a minute or never returns (build
181: no answer in 4 minutes), and the client waits for it before it connects to Steam. When the
request fails, the client falls back to the CM list cached in its config.vdf, which MYIOSDECK
fills before the session (App/Sources/Steam/DockCMCache.swift), and connects over UDP/TCP with
Valve's own channel encryption, no TLS (docs/STEAM_DOCK.md).

MYIOSDECK_DNS_REFUSE (comma-separated host names, compared without regard to case; the app sets
it only for a Dock session without JIT) makes Wine's getaddrinfo answer "host not found" for
those names at once, so that request fails immediately. Nothing else changes: every other name
resolves as before, and without the variable getaddrinfo is untouched.

Applies to Wine's dlls/ws2_32/unixlib.c (Madeira's fork, which Madeira's ntdll-unix build
compiles into the app). Usage: dns_refuse.py <wine checkout>
"""
import sys

REL = "dlls/ws2_32/unixlib.c"

HELPER_ANCHOR = "static NTSTATUS unix_getaddrinfo( void *args )\n{\n"
HELPER = '''/* MYIOSDECK (engine/wine/patches/dns_refuse.py): host names listed in MYIOSDECK_DNS_REFUSE
 * (comma-separated, without regard to case) resolve to nothing at once. */
#include <stdlib.h>
#include <string.h>
static int myiosdeck_dns_refused( const char *node )
{
    const char *list = getenv( "MYIOSDECK_DNS_REFUSE" ), *p;
    size_t len;
    if (!list || !node || !*node) return 0;
    len = strlen( node );
    for (p = list; *p; )
    {
        const char *end = p;
        size_t i, n;
        while (*end && *end != ',') end++;
        n = end - p;
        if (n == len)
        {
            for (i = 0; i < n; i++)
            {
                char a = p[i], b = node[i];
                if (a >= 'A' && a <= 'Z') a += 32;
                if (b >= 'A' && b <= 'Z') b += 32;
                if (a != b) break;
            }
            if (i == n) return 1;
        }
        p = *end ? end + 1 : end;
    }
    return 0;
}

'''

CALL_ANCHOR = "    /* servname tweak required by OSX and BSD kernels */\n    if (service && !service[0]) service = \"0\";\n"
CALL = CALL_ANCHOR + '''
    if (myiosdeck_dns_refused( params->node ))   /* MYIOSDECK: dns_refuse.py */
        return WS_EAI_NONAME;
'''


def main():
    path = "%s/%s" % (sys.argv[1], REL)
    s = open(path).read()
    if "myiosdeck_dns_refused" in s:
        print("dns_refuse: already patched")
        return
    for anchor in (HELPER_ANCHOR, CALL_ANCHOR):
        n = s.count(anchor)
        if n != 1:
            sys.exit("dns_refuse.py: %s: expected 1 of %r, found %d" % (REL, anchor[:40], n))
    s = s.replace(HELPER_ANCHOR, HELPER + HELPER_ANCHOR)
    s = s.replace(CALL_ANCHOR, CALL)
    open(path, "w").write(s)
    print("dns_refuse: patched %s" % REL)


if __name__ == "__main__":
    main()
