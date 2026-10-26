/*
 * The shield: it watches the IP Claude leaves by and the network that IP
 * goes through, and ends Claude when the exit is one the rules do not allow.
 *
 * Claude Desktop and Claude Code reach Anthropic through whatever proxy or
 * tunnel the system routes their traffic to. The shield reads that exit the
 * same way the user can see it: it asks Cloudflare's trace, behind which
 * Claude's own servers sit, what IP it sees (`claude.ai/cdn-cgi/trace`), and
 * then asks public IP-intelligence services what that IP is — the country,
 * whether it is a datacenter, a VPN, a proxy or Tor, its trust score. It
 * also looks at this PC's own network: whether a tunnel adapter is up,
 * whether a proxy app runs, and whether UDP leaves by another IP than
 * Claude's exit (a STUN server's answer).
 *
 * When the exit is in a region Anthropic does not serve, or breaks a rule
 * the user set (no datacenter IP, a lowest trust score, no DNS or UDP leak),
 * the shield ends Claude at once (`Claude_EndAll`) and the manager opens no
 * profile and routes no link until the exit is safe again. Nothing is asked:
 * the point is to stop an account being used from an exit that would get it
 * risk-controlled before the user notices.
 *
 * It runs on a thread of its own while the manager's window is open, waking
 * on network changes (`NotifyUnicastIpAddressChange`, the proxy settings) and
 * otherwise on a slow fallback timer. The latest result is kept for the
 * shield dialog (`Shield_Snapshot`); every check posts `WM_APP_SHIELD`.
 *
 * This file uses only the shared helpers (core.c, util.c, claude.c); its
 * own rules and facts live here, not in the manager or the taskbar code.
 */
#include "app.h"
#include "resource.h"
#include <winsock2.h>
#include <ws2tcpip.h>
#include <iphlpapi.h>
#include <winhttp.h>
#include <tlhelp32.h>
#include <commctrl.h>

#define SETTING_SHIELD_ON          L"ShieldOn"
#define SETTING_SHIELD_ALLOW_DC    L"ShieldAllowDatacenter"
#define SETTING_SHIELD_ALLOW_DNS   L"ShieldAllowDnsLeak"
#define SETTING_SHIELD_ALLOW_UDP   L"ShieldAllowUdpLeak"
#define SETTING_SHIELD_STOP_UNKNOWN L"ShieldStopWhenUnknown"
#define SETTING_SHIELD_STOP_TUN    L"ShieldStopWhenTunDown"
#define SETTING_SHIELD_STOP_PROXY  L"ShieldStopWhenProxyStops"
#define SETTING_SHIELD_MIN_SCORE   L"ShieldMinScore"
#define SETTING_SHIELD_IPV6        L"ShieldAllowIpv6Leak"
#define SETTING_SHIELD_WARN_ONLY   L"ShieldWarnOnly"
#define SHIELD_REGIONS_SECTION     L"ShieldRegions"   /* one item per profile: "<folder>\t<codes>" */

#define FALLBACK_POLL_MS   (5u * 60u * 1000u)   /* no event in this long: a check anyway */
#define SETTLE_MS          1500u                 /* after a network change, wait this long for it to settle */
#define RISK_MAX_BYTES     65536u
#define HTTP_DEADLINE_MS   9000u
#define STUN_WAIT_MS       3000

/* Claude's exit is read through these, each behind Cloudflare, in order
 * until one answers: a site may be blocked where another is not. */
static const WCHAR *const kTraceHosts[] = { L"claude.ai", L"api.anthropic.com", L"claude.com", L"www.anthropic.com" };

/* The IP services asked about the exit, in order; the first that answers for
 * all its facts wins, the rest fill what it left unknown. */
typedef struct RiskSource { int source; const WCHAR *host; const WCHAR *path; BOOL keyedByIp; } RiskSource;
static const RiskSource kRiskSources[] = {
    { SHIELD_SOURCE_NETCOFFEE,  L"ip.net.coffee", L"/api/iprisk/",  FALSE },
    { SHIELD_SOURCE_IPAPIIS,    L"api.ipapi.is",  L"/?q=",          TRUE  },
    { SHIELD_SOURCE_PROXYCHECK, L"proxycheck.io", L"/v2/",          TRUE  },
};

/* Proxy and tunnel apps whose running process means the exit is routed. */
static const WCHAR *const kProxyApps[] = {
    L"clash-verge.exe", L"verge-mihomo.exe", L"mihomo.exe", L"clash.exe", L"clash-win64.exe", L"nikoneko.exe", L"flclash.exe",
    L"sing-box.exe", L"v2ray.exe", L"v2rayn.exe", L"xray.exe", L"nekoray.exe", L"nekobox.exe", L"wireguard.exe", L"tun2socks.exe",
};

static struct {
    CRITICAL_SECTION  lock;             /* guards snapshot and rules */
    HWND              manager;
    ClaudePackage     pkg;
    HANDLE            thread, stop, wake;
    HANDLE            ipNotify;          /* NotifyUnicastIpAddressChange's */
    volatile LONG     running;
    volatile LONG     checkNow;          /* a manual "check now" asked for: run a read-only check even while off */
    volatile LONG     checking;          /* a check is running */
    HWND              dialog;            /* the open shield dialog, told when a check starts and ends */
    BOOL              enabled;
    CoreShieldRules   rules;
    ShieldSnapshot    snapshot;
    ShieldEvent       events[SHIELD_EVENTS_MAX];   /* newest first */
    int               eventCount;
} g;

static BOOL isIPv6(const WCHAR *ip)
{
    return ip && wcschr(ip, L':') != NULL;
}

/* ------------------------------------------------------------- settings */

static void LoadRules(void)
{
    g.enabled = Util_GetSetting(SETTING_SHIELD_ON, 0) != 0;
    g.rules.allowDatacenter = Util_GetSetting(SETTING_SHIELD_ALLOW_DC, 1) != 0;
    g.rules.allowDnsLeak = Util_GetSetting(SETTING_SHIELD_ALLOW_DNS, 1) != 0;
    g.rules.allowUdpLeak = Util_GetSetting(SETTING_SHIELD_ALLOW_UDP, 1) != 0;
    g.rules.allowIpv6Leak = Util_GetSetting(SETTING_SHIELD_IPV6, 1) != 0;
    g.rules.stopWhenUnknown = Util_GetSetting(SETTING_SHIELD_STOP_UNKNOWN, 0) != 0;
    g.rules.stopWhenTunDown = Util_GetSetting(SETTING_SHIELD_STOP_TUN, 0) != 0;
    g.rules.stopWhenProxyAppStops = Util_GetSetting(SETTING_SHIELD_STOP_PROXY, 0) != 0;
    g.rules.warnOnly = Util_GetSetting(SETTING_SHIELD_WARN_ONLY, 0) != 0;
    g.rules.minScore = (int)min(Util_GetSetting(SETTING_SHIELD_MIN_SCORE, 0), 100);
}

static void SaveRules(void)
{
    Util_SetSetting(SETTING_SHIELD_ON, g.enabled);
    Util_SetSetting(SETTING_SHIELD_ALLOW_DC, g.rules.allowDatacenter);
    Util_SetSetting(SETTING_SHIELD_ALLOW_DNS, g.rules.allowDnsLeak);
    Util_SetSetting(SETTING_SHIELD_ALLOW_UDP, g.rules.allowUdpLeak);
    Util_SetSetting(SETTING_SHIELD_IPV6, g.rules.allowIpv6Leak);
    Util_SetSetting(SETTING_SHIELD_STOP_UNKNOWN, g.rules.stopWhenUnknown);
    Util_SetSetting(SETTING_SHIELD_STOP_TUN, g.rules.stopWhenTunDown);
    Util_SetSetting(SETTING_SHIELD_STOP_PROXY, g.rules.stopWhenProxyAppStops);
    Util_SetSetting(SETTING_SHIELD_WARN_ONLY, g.rules.warnOnly);
    Util_SetSetting(SETTING_SHIELD_MIN_SCORE, (DWORD)g.rules.minScore);
}

/* Profile `folder`'s allowed region codes (`out`), from the list kept in
 * settings.ini; FALSE (and "") when the profile has no list (any region). */
static BOOL ShieldRegionsOf(const WCHAR *folder, WCHAR *out, size_t cch)
{
    WCHAR (*items)[MAX_PATH] = (WCHAR (*)[MAX_PATH])HeapAlloc(GetProcessHeap(), 0, (size_t)MAX_PROFILES * sizeof *items);
    int count, i;
    BOOL found = FALSE;
    out[0] = 0;
    if (!items) return FALSE;
    count = Util_GetSettingList(SHIELD_REGIONS_SECTION, items, MAX_PROFILES);
    for (i = 0; i < count && !found; i++) {
        WCHAR *tab = wcschr(items[i], L'\t');
        if (tab) {
            *tab = 0;
            if (Core_EqualsI(items[i], folder) && tab[1]) {
                StringCchCopyW(out, cch, tab + 1);
                found = TRUE;
            }
        }
    }
    HeapFree(GetProcessHeap(), 0, items);
    return found && out[0];
}

void Shield_GetProfileRegions(const WCHAR *folder, WCHAR *out, size_t cch)
{
    ShieldRegionsOf(folder, out, cch);
}

void Shield_SetProfileRegions(const WCHAR *folder, const WCHAR *codes)
{
    WCHAR (*items)[MAX_PATH] = (WCHAR (*)[MAX_PATH])HeapAlloc(GetProcessHeap(), 0, (size_t)(MAX_PROFILES + 1) * sizeof *items);
    int count, i, kept = 0;
    if (!items) return;
    count = Util_GetSettingList(SHIELD_REGIONS_SECTION, items, MAX_PROFILES + 1);
    for (i = 0; i < count; i++) {
        WCHAR *tab = wcschr(items[i], L'\t');
        if (tab) {
            WCHAR save = *tab;
            *tab = 0;
            if (Core_EqualsI(items[i], folder)) { *tab = save; continue; }   /* drop the old entry for this profile */
            *tab = save;
        }
        if (kept != i) StringCchCopyW(items[kept], MAX_PATH, items[i]);
        kept++;
    }
    if (codes && codes[0] && kept <= MAX_PROFILES) StringCchPrintfW(items[kept++], MAX_PATH, L"%s\t%s", folder, codes);
    Util_SetSettingList(SHIELD_REGIONS_SECTION, (const WCHAR (*)[MAX_PATH])items, kept);
    HeapFree(GetProcessHeap(), 0, items);
    Shield_Recheck();
}

/* Running profiles whose allowed-region list excludes the exit country `code`
 * are ended (Claude_Quit), unless warn-only. Returns TRUE when any profile
 * was, or would be, stopped for its region (the badge turns red then). */
static BOOL EnforceProfileRegions(const WCHAR *code, BOOL warnOnly)
{
    ProfileList list;
    WCHAR allowed[128];
    BOOL any = FALSE;
    int i;
    Profiles_Load(&list, &g.pkg);
    Claude_UpdateRunning(&list);
    for (i = 0; i < list.count; i++) {
        if (!ShieldRegionsOf(list.items[i].folder, allowed, ARRAYSIZE(allowed)) || Core_RegionAllowed(code, allowed)) continue;
        any = TRUE;
        if (!warnOnly && list.items[i].running) {
            DWORD error = 0;
            Claude_Quit(&list.items[i], &error);
            Util_Log(L"shield: %s quit, its exit %s is not an allowed region", list.items[i].folder, code);
        }
    }
    return any;
}

/* ------------------------------------------------------------- the facts */

#define HTTP_RESOLVE_TIMEOUT_MS 8000
#define HTTP_CONNECT_TIMEOUT_MS 8000
#define HTTP_SEND_TIMEOUT_MS    8000
#define HTTP_RECEIVE_TIMEOUT_MS 9000

/* GET https://host/path through the system's proxy (the route Claude's own
 * requests take), given up after `deadlineMs`: the body (NUL-terminated,
 * HeapFree it) and `*size`, or NULL. Its own request, kept apart from
 * update.c's (which the tests drive through mocked WinHttp). */
static char *HttpGet(const WCHAR *host, const WCHAR *path, const WCHAR *headers, DWORD maxBytes, DWORD deadlineMs, DWORD *size)
{
    HINTERNET session, connection = NULL, request = NULL;
    DWORD status = 0, statusBytes = sizeof status, used = 0, capacity = 0, available, received, error = ERROR_SUCCESS;
    ULONGLONG deadline = GetTickCount64() + deadlineMs;
    char *body = NULL, *grown;
    BOOL complete = FALSE;
    *size = 0;
    session = WinHttpOpen(APP_NAME L"/" APP_VERSION_WSTR, WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME,
                          WINHTTP_NO_PROXY_BYPASS, 0);
    if (!session ||
        !WinHttpSetTimeouts(session, HTTP_RESOLVE_TIMEOUT_MS, HTTP_CONNECT_TIMEOUT_MS, HTTP_SEND_TIMEOUT_MS, HTTP_RECEIVE_TIMEOUT_MS) ||
        (connection = WinHttpConnect(session, host, INTERNET_DEFAULT_HTTPS_PORT, 0)) == NULL ||
        (request = WinHttpOpenRequest(connection, L"GET", path, NULL, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES,
                                      WINHTTP_FLAG_SECURE)) == NULL ||
        !WinHttpSendRequest(request, headers ? headers : WINHTTP_NO_ADDITIONAL_HEADERS, headers ? (DWORD)-1L : 0,
                            WINHTTP_NO_REQUEST_DATA, 0, 0, 0) ||
        !WinHttpReceiveResponse(request, NULL) ||
        !WinHttpQueryHeaders(request, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX,
                             &status, &statusBytes, WINHTTP_NO_HEADER_INDEX)) {
        error = GetLastError();
    } else if (status == HTTP_STATUS_OK) {
        for (;;) {
            if (GetTickCount64() >= deadline) break;
            available = 0;
            if (!WinHttpQueryDataAvailable(request, &available)) break;
            if (available == 0) { complete = body != NULL; break; }
            if (available > maxBytes - used) break;
            if (used + available + 1 > capacity) {
                capacity = used + available + 1 > capacity * 2 ? used + available + 1 : capacity * 2;
                if (capacity > maxBytes + 1) capacity = maxBytes + 1;
                grown = body ? (char *)HeapReAlloc(GetProcessHeap(), 0, body, capacity) : (char *)HeapAlloc(GetProcessHeap(), 0, capacity);
                if (!grown) break;
                body = grown;
            }
            if (!WinHttpReadData(request, body + used, available, &received)) break;
            used += received;
        }
    }
    (void)error;
    if (request) WinHttpCloseHandle(request);
    if (connection) WinHttpCloseHandle(connection);
    if (session) WinHttpCloseHandle(session);
    if (!complete) {
        if (body) HeapFree(GetProcessHeap(), 0, body);
        return NULL;
    }
    body[used] = 0;
    *size = used;
    return body;
}

/* Claude's exit IP and the country Cloudflare gives it, from the first
 * trace host that answers. FALSE when none does. */
static BOOL ReadExit(WCHAR *ip, size_t ipCch, WCHAR *loc, size_t locCch)
{
    size_t i;
    for (i = 0; i < ARRAYSIZE(kTraceHosts); i++) {
        DWORD size = 0;
        char *body = HttpGet(kTraceHosts[i], L"/cdn-cgi/trace", L"Cache-Control: no-cache\r\n", 8192, HTTP_DEADLINE_MS, &size);
        if (!body) continue;
        if (Core_ParseTrace(body, size, ip, ipCch, loc, locCch)) {
            HeapFree(GetProcessHeap(), 0, body);
            return TRUE;
        }
        HeapFree(GetProcessHeap(), 0, body);
    }
    return FALSE;
}

/* What the IP services say of `ip`: the first source that names its country
 * sets `out`, later sources fill only facts still unknown. FALSE when none
 * answers. */
static BOOL ReadRisk(const WCHAR *ip, CoreIpRisk *out)
{
    BOOL any = FALSE;
    size_t i;
    ZeroMemory(out, sizeof *out);
    out->trustScore = -1;
    for (i = 0; i < ARRAYSIZE(kRiskSources); i++) {
        WCHAR path[128];
        DWORD size = 0;
        char *body;
        CoreIpRisk one;
        if (kRiskSources[i].source == SHIELD_SOURCE_PROXYCHECK)
            StringCchPrintfW(path, ARRAYSIZE(path), L"%s%s?vpn=1&asn=1", kRiskSources[i].path, ip);
        else
            StringCchPrintfW(path, ARRAYSIZE(path), L"%s%s", kRiskSources[i].path, ip);
        body = HttpGet(kRiskSources[i].host, path, NULL, RISK_MAX_BYTES, HTTP_DEADLINE_MS, &size);
        if (!body) continue;
        if (Core_ParseIpRisk(kRiskSources[i].source, body, size, ip, &one)) {
            if (!any) {
                *out = one;
                any = TRUE;
            } else {
                /* Fill only what the first source left unknown. */
                if (out->trustScore < 0) out->trustScore = one.trustScore;
                if (out->datacenter < 0) out->datacenter = one.datacenter;
                if (out->residential < 0) out->residential = one.residential;
                if (out->mobile < 0) out->mobile = one.mobile;
                if (out->vpn < 0) out->vpn = one.vpn;
                if (out->proxy < 0) out->proxy = one.proxy;
                if (out->tor < 0) out->tor = one.tor;
                if (out->abuser < 0) out->abuser = one.abuser;
                if (!out->region[0]) StringCchCopyW(out->region, ARRAYSIZE(out->region), one.region);
            }
        }
        HeapFree(GetProcessHeap(), 0, body);
    }
    return any;
}

/* A tunnel adapter is up: one whose interface type is a tunnel, or whose
 * name says so (Wintun, TAP, WireGuard, a Clash/Mihomo adapter). */
static int TunnelUp(void)
{
    static const WCHAR *const kMarks[] = { L"wintun", L"tap-windows", L"wireguard", L"tunnel", L"mihomo", L"clash", L"sing-box", L"utun" };
    IP_ADAPTER_ADDRESSES *addresses, *a;
    ULONG size = 16384, result;
    int up = 0;
    size_t tries;
    for (tries = 0; tries < 3; tries++) {
        addresses = (IP_ADAPTER_ADDRESSES *)HeapAlloc(GetProcessHeap(), 0, size);
        if (!addresses) return -1;
        result = GetAdaptersAddresses(AF_UNSPEC, GAA_FLAG_SKIP_DNS_SERVER | GAA_FLAG_SKIP_MULTICAST | GAA_FLAG_SKIP_ANYCAST,
                                      NULL, addresses, &size);
        if (result == ERROR_SUCCESS) break;
        HeapFree(GetProcessHeap(), 0, addresses);
        if (result != ERROR_BUFFER_OVERFLOW) return -1;
    }
    if (tries == 3) return -1;
    for (a = addresses; a && !up; a = a->Next) {
        size_t m;
        if (a->OperStatus != IfOperStatusUp) continue;
        if (a->IfType == IF_TYPE_TUNNEL) up = 1;
        for (m = 0; a->Description && m < ARRAYSIZE(kMarks) && !up; m++)
            if (Core_ContainsI(a->Description, kMarks[m]) || (a->FriendlyName && Core_ContainsI(a->FriendlyName, kMarks[m]))) up = 1;
    }
    HeapFree(GetProcessHeap(), 0, addresses);
    return up;
}

/* A public (global unicast) IPv6 address on an up, non-tunnel adapter, into
 * `out`, and that adapter's IPv6 interface index (`*index`). "" when none. A
 * fc00::/7 unique-local or an fe80::/10 link-local address is not one. */
static void PublicIpv6(WCHAR *out, size_t cch, DWORD *index)
{
    IP_ADAPTER_ADDRESSES *addresses, *a;
    ULONG size = 16384, result;
    size_t tries;
    out[0] = 0;
    *index = 0;
    for (tries = 0; tries < 3; tries++) {
        addresses = (IP_ADAPTER_ADDRESSES *)HeapAlloc(GetProcessHeap(), 0, size);
        if (!addresses) return;
        result = GetAdaptersAddresses(AF_INET6, GAA_FLAG_SKIP_DNS_SERVER | GAA_FLAG_SKIP_MULTICAST | GAA_FLAG_SKIP_ANYCAST, NULL, addresses, &size);
        if (result == ERROR_SUCCESS) break;
        HeapFree(GetProcessHeap(), 0, addresses);
        if (result != ERROR_BUFFER_OVERFLOW) return;
    }
    if (tries == 3) return;
    for (a = addresses; a && !out[0]; a = a->Next) {
        IP_ADAPTER_UNICAST_ADDRESS *u;
        if (a->OperStatus != IfOperStatusUp || a->IfType == IF_TYPE_TUNNEL || a->IfType == IF_TYPE_SOFTWARE_LOOPBACK) continue;
        for (u = a->FirstUnicastAddress; u && !out[0]; u = u->Next) {
            const struct sockaddr_in6 *in6 = (const struct sockaddr_in6 *)u->Address.lpSockaddr;
            const BYTE *b;
            if (!in6 || u->Address.lpSockaddr->sa_family != AF_INET6) continue;
            b = in6->sin6_addr.s6_addr;
            if (b[0] == 0xFE && (b[1] & 0xC0) == 0x80) continue;   /* link-local */
            if ((b[0] & 0xFE) == 0xFC) continue;                   /* unique-local */
            StringCchPrintfW(out, cch, L"%x:%x:%x:%x:%x:%x:%x:%x", b[0] << 8 | b[1], b[2] << 8 | b[3], b[4] << 8 | b[5], b[6] << 8 | b[7],
                             b[8] << 8 | b[9], b[10] << 8 | b[11], b[12] << 8 | b[13], b[14] << 8 | b[15]);
            *index = a->Ipv6IfIndex;
        }
    }
    HeapFree(GetProcessHeap(), 0, addresses);
}

/* Whether IPv6 traffic would leave by the adapter that holds a public IPv6
 * (`index`), not the tunnel: Windows' best route to a public IPv6 host
 * (Cloudflare's DNS) goes out that interface. 1 a leak, 0 none (no IPv6
 * route, or it goes through the tunnel), -1 when no such adapter. */
static int Ipv6Leaks(DWORD index)
{
    struct sockaddr_in6 to;
    DWORD best = 0;
    static const BYTE kCloudflare[16] = { 0x26, 0x06, 0x47, 0x00, 0x47, 0x00, 0, 0, 0, 0, 0, 0, 0, 0, 0x11, 0x11 };
    if (!index) return -1;
    ZeroMemory(&to, sizeof to);
    to.sin6_family = AF_INET6;
    memcpy(to.sin6_addr.s6_addr, kCloudflare, sizeof kCloudflare);
    if (GetBestInterfaceEx((struct sockaddr *)&to, &best) != NO_ERROR) return 0;   /* no IPv6 route at all */
    return best == index ? 1 : 0;
}

/* This PC's current offset from UTC, in minutes (DST included). */
static int LocalOffsetMinutes(void)
{
    SYSTEMTIME local, utc;
    FILETIME lf, uf;
    ULARGE_INTEGER l, u;
    GetLocalTime(&local);
    GetSystemTime(&utc);
    if (!SystemTimeToFileTime(&local, &lf) || !SystemTimeToFileTime(&utc, &uf)) return 0;
    l.LowPart = lf.dwLowDateTime; l.HighPart = lf.dwHighDateTime;
    u.LowPart = uf.dwLowDateTime; u.HighPart = uf.dwHighDateTime;
    return (int)(((LONGLONG)l.QuadPart - (LONGLONG)u.QuadPart) / (10000000LL * 60));
}

/* A proxy or tunnel app runs. */
static int ProxyAppRuns(void)
{
    int i;
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    PROCESSENTRY32W entry;
    BOOL found = FALSE;
    if (snapshot == INVALID_HANDLE_VALUE) return -1;
    entry.dwSize = sizeof entry;
    if (Process32FirstW(snapshot, &entry)) {
        do {
            for (i = 0; i < (int)ARRAYSIZE(kProxyApps); i++)
                if (Core_EqualsI(entry.szExeFile, kProxyApps[i])) { found = TRUE; break; }
        } while (!found && Process32NextW(snapshot, &entry));
    }
    CloseHandle(snapshot);
    return found ? 1 : 0;
}

/* The UDP exit a STUN server sees, "" when it could not be reached. Leaks
 * when it differs from Claude's exit IP (udpLeak set by the caller). */
static void ReadUdpExit(WCHAR *ip, size_t cch)
{
    SOCKET sock = INVALID_SOCKET;
    struct addrinfo hints, *info = NULL;
    BYTE transaction[12], request[32], response[512];
    DWORD i;
    int n;
    ip[0] = 0;
    for (i = 0; i < sizeof transaction; i++) transaction[i] = (BYTE)(GetTickCount() >> (i % 4 * 8)) ^ (BYTE)i;
    ZeroMemory(&hints, sizeof hints);
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_DGRAM;
    if (getaddrinfo("stun.l.google.com", "19302", &hints, &info) != 0 || !info) return;
    sock = socket(info->ai_family, SOCK_DGRAM, IPPROTO_UDP);
    if (sock != INVALID_SOCKET) {
        DWORD wait = STUN_WAIT_MS;
        size_t length = Core_StunRequest(request, sizeof request, transaction);
        setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, (const char *)&wait, sizeof wait);
        if (length && sendto(sock, (const char *)request, (int)length, 0, info->ai_addr, (int)info->ai_addrlen) == (int)length) {
            n = recv(sock, (char *)response, sizeof response, 0);
            if (n > 0) Core_StunMappedAddress(response, (size_t)n, transaction, ip, cch);
        }
        closesocket(sock);
    }
    freeaddrinfo(info);
}

/* Whether DNS leaks to a region Claude does not serve, the way the IP page
 * checks it: a random subdomain of ip.net.coffee's authoritative DNS is
 * resolved (the HTTPS connect triggers it, whatever the page returns), then
 * its API says which resolver IPs that DNS server saw; a resolver in a
 * region Claude does not serve means the real location leaks through DNS.
 * 1 a leak, 0 none seen, -1 could not tell. */
static int ReadDnsLeak(void)
{
    WCHAR token[20], host[64], path[96];
    char *body;
    WCHAR *servers, *at;
    DWORD size = 0;
    int i, leak = -1;
    ULARGE_INTEGER now;
    GetSystemTimeAsFileTime((FILETIME *)&now);
    StringCchPrintfW(token, ARRAYSIZE(token), L"%08lx%08lx", now.LowPart, (DWORD)(now.HighPart ^ GetCurrentThreadId()));
    /* Two rounds, as the page does: resolve the token's subdomain so the
     * authoritative server logs the resolver. The fetch itself may 404. */
    for (i = 1; i <= 2; i++) {
        DWORD ignore = 0;
        char *drop;
        StringCchPrintfW(host, ARRAYSIZE(host), L"%s-%d.d.ip.net.coffee", token, i);
        drop = HttpGet(host, L"/pixel.gif", NULL, 1024, 4000, &ignore);
        if (drop) HeapFree(GetProcessHeap(), 0, drop);
    }
    Sleep(1500);
    StringCchPrintfW(path, ARRAYSIZE(path), L"/api/dns/result/%s", token);
    body = HttpGet(L"ip.net.coffee", path, NULL, 16384, HTTP_DEADLINE_MS, &size);
    if (!body) return -1;
    {
        const char *value;
        size_t valueLength;
        if (Core_JsonMember(body, size, "dns_servers", &value, &valueLength) && (servers = Core_JsonStringList(value, valueLength)) != NULL) {
            leak = 0;   /* a result came back: no leak until a resolver says otherwise */
            for (at = servers; *at && leak == 0; at += wcslen(at) + 1) {
                WCHAR geoPath[96], code[8];
                DWORD geoSize = 0;
                char *geo;
                StringCchPrintfW(geoPath, ARRAYSIZE(geoPath), L"/api/geoip/%s", at);
                geo = HttpGet(L"ip.net.coffee", geoPath, NULL, 8192, HTTP_DEADLINE_MS, &geoSize);
                if (geo) {
                    if (Core_JsonMemberString(geo, geoSize, "country_code", code, ARRAYSIZE(code)) && code[0] && !Core_ClaudeRegionSupported(code, NULL))
                        leak = 1;
                    HeapFree(GetProcessHeap(), 0, geo);
                }
            }
            HeapFree(GetProcessHeap(), 0, servers);
        }
    }
    HeapFree(GetProcessHeap(), 0, body);
    return leak;
}

/* The default (Cloudflare) exit, read as the IP page does (1.1.1.1's trace):
 * it may differ from Claude's when the proxy sends Cloudflare traffic apart. */
static void ReadCloudflareExit(WCHAR *ip, size_t ipCch, WCHAR *loc, size_t locCch)
{
    DWORD size = 0;
    char *body = HttpGet(L"1.1.1.1", L"/cdn-cgi/trace", L"Cache-Control: no-cache\r\n", 8192, HTTP_DEADLINE_MS, &size);
    ip[0] = 0;
    if (loc) loc[0] = 0;
    if (body) {
        Core_ParseTrace(body, size, ip, ipCch, loc, locCch);
        HeapFree(GetProcessHeap(), 0, body);
    }
}

/* How long `host` takes to answer its trace, in ms; -1 when it does not. */
static int Latency(const WCHAR *host)
{
    ULONGLONG start = GetTickCount64();
    DWORD size = 0;
    char *body = HttpGet(host, L"/cdn-cgi/trace", L"Cache-Control: no-cache\r\n", 8192, 6000, &size);
    if (!body) return -1;
    HeapFree(GetProcessHeap(), 0, body);
    return (int)(GetTickCount64() - start);
}

/* Anthropic's overall service status, as the IP page reads it. */
static void ReadStatus(WCHAR *out, size_t cch)
{
    DWORD size = 0;
    char *body = HttpGet(L"ip.net.coffee", L"/claude/status.json", NULL, 8192, HTTP_DEADLINE_MS, &size);
    WCHAR indicator[32];
    out[0] = 0;
    if (!body) return;
    if (Core_JsonMemberString(body, size, "overall_indicator", indicator, ARRAYSIZE(indicator))) {
        if (Core_EqualsI(indicator, L"none")) StringCchCopyW(out, cch, TR(L"all services normal"));
        else if (Core_EqualsI(indicator, L"minor")) StringCchCopyW(out, cch, TR(L"minor outage"));
        else if (Core_EqualsI(indicator, L"major")) StringCchCopyW(out, cch, TR(L"major outage"));
        else if (Core_EqualsI(indicator, L"critical")) StringCchCopyW(out, cch, TR(L"critical outage"));
        else if (Core_EqualsI(indicator, L"maintenance")) StringCchCopyW(out, cch, TR(L"under maintenance"));
        else StringCchCopyW(out, cch, TR(L"service issue"));
    }
    HeapFree(GetProcessHeap(), 0, body);
}

/* ------------------------------------------------------------- the check */

#define WM_SHIELD_CHECKED (WM_APP + 0x5D)   /* to the open dialog: a check started or ended */

static void Post(ShieldState state)
{
    HWND dialog;
    if (g.manager) PostMessageW(g.manager, WM_APP_SHIELD, (WPARAM)state, 0);
    EnterCriticalSection(&g.lock);
    dialog = g.dialog;
    LeaveCriticalSection(&g.lock);
    if (dialog) PostMessageW(dialog, WM_SHIELD_CHECKED, 0, 0);
}

/* The time-zone, language and split-exit mismatches only show in the dialog:
 * a Chinese system on a US exit is the usual case, and a badge that is
 * always amber would say nothing. */
#define SHIELD_INFO_ONLY (SHIELD_CONCERN_TZ | SHIELD_CONCERN_LANG | SHIELD_CONCERN_SPLIT)

/* Stopped for every profile only by a global reason: one a profile's own regions give ends that profile alone. */
static ShieldState StateFor(const ShieldSnapshot *s)
{
    if (!g.enabled) return SHIELD_OFF;
    if (s->stop & ~SHIELD_STOP_PROFILE) return SHIELD_STOPPED;
    if (s->stop || (s->concerns & ~SHIELD_INFO_ONLY)) return SHIELD_CONCERN;
    return SHIELD_SAFE;
}

/* One check: read the exit, what it is, and the local network; decide; end
 * Claude when a rule says to; keep the result and tell the window. */
static void RunCheck(void)
{
    ShieldSnapshot s;
    CoreShieldFacts facts;
    CoreShieldRules rules;
    WSADATA wsa;
    BOOL wsaReady;
    DWORD ipv6Index = 0;

    EnterCriticalSection(&g.lock);
    rules = g.rules;
    LeaveCriticalSection(&g.lock);

    InterlockedExchange(&g.checking, 1);
    Post(g.enabled ? SHIELD_CHECKING : SHIELD_OFF);   /* a check now while off leaves the badge off */
    ZeroMemory(&s, sizeof s);
    s.tunUp = TunnelUp();
    s.proxyApp = ProxyAppRuns();
    s.udpLeak = s.dnsLeak = s.ipv6Leak = s.tzMismatch = s.langMismatch = s.cfSplit = -1;
    s.latencyClaude = s.latencyAnthropic = -1;
    PublicIpv6(s.ipv6, ARRAYSIZE(s.ipv6), &ipv6Index);
    s.exitKnown = ReadExit(s.exitIp, ARRAYSIZE(s.exitIp), s.exitLoc, ARRAYSIZE(s.exitLoc));
    if (s.exitKnown) {
        const WCHAR *code;
        int exitOffset, localOffset;
        WCHAR primary[16];
        s.riskKnown = ReadRisk(s.exitIp, &s.risk);
        code = s.risk.countryCode[0] ? s.risk.countryCode : s.exitLoc;
        s.dnsLeak = ReadDnsLeak();
        ReadCloudflareExit(s.cfIp, ARRAYSIZE(s.cfIp), s.cfLoc, ARRAYSIZE(s.cfLoc));
        if (s.cfLoc[0] && code[0]) s.cfSplit = Core_EqualsI(s.cfLoc, code) ? 0 : 1;
        /* Claude leaving by IPv4 while IPv6 routes past the tunnel: an IPv6
         * connection could reach Anthropic without the proxy. */
        s.ipv6Leak = isIPv6(s.exitIp) ? 0 : Ipv6Leaks(ipv6Index);
        if (s.ipv6Leak < 0) s.ipv6Leak = 0;
        exitOffset = Core_TimezoneOffsetMinutes(s.risk.timezone[0] ? s.risk.timezone : code);
        localOffset = LocalOffsetMinutes();
        if (exitOffset != INT_MIN) s.tzMismatch = abs(localOffset - exitOffset) > 120 ? 1 : 0;
        if (GetUserDefaultLocaleName(primary, ARRAYSIZE(primary)) > 0)
            s.langMismatch = Core_RegionLanguageMatches(code, primary) ? 0 : 1;
        wsaReady = WSAStartup(MAKEWORD(2, 2), &wsa) == 0;
        if (wsaReady) {
            ReadUdpExit(s.udpIp, ARRAYSIZE(s.udpIp));
            WSACleanup();
            if (s.udpIp[0]) s.udpLeak = Core_EqualsI(s.udpIp, s.exitIp) ? 0 : 1;
        }
        s.latencyClaude = Latency(L"claude.ai");
        s.latencyAnthropic = Latency(L"www.anthropic.com");
        ReadStatus(s.statusText, ARRAYSIZE(s.statusText));
    }

    ZeroMemory(&facts, sizeof facts);
    facts.exitKnown = s.exitKnown;
    facts.tunUp = s.tunUp;
    facts.proxyApp = s.proxyApp;
    facts.udpLeak = s.udpLeak;
    facts.dnsLeak = s.dnsLeak;
    facts.ipv6Leak = s.ipv6Leak;
    facts.tzMismatch = s.tzMismatch;
    facts.langMismatch = s.langMismatch;
    facts.cfSplit = s.cfSplit;
    facts.profileAllowed = TRUE;   /* per-profile regions not set here yet */
    if (s.exitKnown) {
        const WCHAR *code = s.risk.countryCode[0] ? s.risk.countryCode : s.exitLoc;
        facts.supported = Core_ClaudeRegionSupported(code, s.risk.region[0] ? s.risk.region : NULL);
        facts.riskKnown = s.riskKnown;
        facts.trustScore = s.riskKnown ? s.risk.trustScore : -1;
        facts.datacenter = s.riskKnown ? s.risk.datacenter : -1;
    } else {
        facts.trustScore = -1;
        facts.datacenter = -1;
    }
    s.stop = Core_ShieldVerdict(&facts, &rules);   /* the global reasons; the per-profile one is added below */
    s.concerns = Core_ShieldConcerns(&facts);
    s.checkedMs = Util_NowMs();

    /* The global reasons end every Claude at once; a per-profile region ends
     * only the profiles it names (EnforceProfileRegions). */
    if (g.enabled && s.stop) {
        int failed = 0;
        s.endedDesktop = Claude_EndAll(&g.pkg, CLAUDE_END_DESKTOP, NULL, &failed);
        s.endedCode = Claude_EndAll(&g.pkg, CLAUDE_END_CODE, NULL, &failed);
    }
    if (s.exitKnown) {
        const WCHAR *code = s.risk.countryCode[0] ? s.risk.countryCode : s.exitLoc;
        if (EnforceProfileRegions(code, rules.warnOnly || !g.enabled)) {
            s.concerns |= SHIELD_STOP_PROFILE;
            if (!rules.warnOnly) s.stop |= SHIELD_STOP_PROFILE;
        }
    }
    if (g.enabled && s.stop)
        Util_Log(L"shield: exit %s (%s) stop 0x%04lX; ended %d desktop, %d code process(es)",
                 s.exitIp, s.exitLoc, s.stop, s.endedDesktop, s.endedCode);
    else if (g.enabled)
        Util_Log(L"shield: exit %s (%s) is allowed (concerns 0x%04lX)", s.exitIp[0] ? s.exitIp : L"unknown", s.exitLoc, s.concerns);

    s.state = StateFor(&s);
    EnterCriticalSection(&g.lock);
    {
        ShieldState was = g.snapshot.state;
        g.snapshot = s;
        /* The log keeps a line whenever the state changes or Claude is
         * ended, newest first. */
        if (s.state != was || s.endedDesktop || s.endedCode) {
            MoveMemory(&g.events[1], &g.events[0], (SHIELD_EVENTS_MAX - 1) * sizeof g.events[0]);
            g.events[0].whenMs = s.checkedMs;
            g.events[0].state = s.state;
            g.events[0].reasons = s.stop ? s.stop : s.concerns & ~SHIELD_INFO_ONLY;
            g.events[0].ended = s.endedDesktop + s.endedCode;
            if (g.eventCount < SHIELD_EVENTS_MAX) g.eventCount++;
        }
    }
    LeaveCriticalSection(&g.lock);
    InterlockedExchange(&g.checking, 0);
    Post(s.state);
}

/* ------------------------------------------------------------- the thread */

static void NETIOAPI_API_ OnIpChange(void *context, MIB_UNICASTIPADDRESS_ROW *row, MIB_NOTIFICATION_TYPE type)
{
    (void)context;
    (void)row;
    (void)type;
    if (g.wake) SetEvent(g.wake);
}

/* The proxy settings (Internet Settings, its connections included) watched once more: `event` is set at their next
 * change. FALSE when they cannot be. */
static BOOL WatchProxySettings(HKEY *key, HANDLE event)
{
    if (!*key && RegOpenKeyExW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Internet Settings", 0, KEY_NOTIFY, key) !=
                     ERROR_SUCCESS) {
        *key = NULL;
        return FALSE;
    }
    return RegNotifyChangeKeyValue(*key, TRUE, REG_NOTIFY_CHANGE_LAST_SET | REG_NOTIFY_THREAD_AGNOSTIC, event, TRUE) == ERROR_SUCCESS;
}

static DWORD WINAPI ShieldThread(void *param)
{
    HANDLE waits[3], proxyChanged = CreateEventW(NULL, FALSE, FALSE, NULL);
    HKEY proxyKey = NULL;
    DWORD count = 2;
    (void)param;
    waits[0] = g.stop;
    waits[1] = g.wake;
    NotifyUnicastIpAddressChange(AF_UNSPEC, OnIpChange, NULL, FALSE, &g.ipNotify);
    /* A proxy app turned on or off, or another proxy set: Claude's exit may change with no address changing. */
    if (proxyChanged && WatchProxySettings(&proxyKey, proxyChanged)) waits[count++] = proxyChanged;
    for (;;) {
        DWORD which;
        /* A manual "check now" runs even while the shield is off: it reads the exit and leaks for the dialog but
         * enforces nothing (RunCheck ends Claude only when g.enabled). Otherwise a check runs only when enabled. */
        BOOL forced = InterlockedExchange(&g.checkNow, 0) != 0;
        if (g.enabled || forced) RunCheck();
        else Post(SHIELD_OFF);
        which = WaitForMultipleObjects(count, waits, FALSE, g.enabled ? FALLBACK_POLL_MS : INFINITE);
        if (which == WAIT_OBJECT_0) break;   /* stop */
        if (which == WAIT_OBJECT_0 + 2) {
            if (!WatchProxySettings(&proxyKey, proxyChanged)) count = 2;   /* watched again for the next change */
            which = WAIT_OBJECT_0 + 1;   /* a network change */
        }
        if (which == WAIT_OBJECT_0 + 1) {
            /* A network change: let it settle, and fold a burst into one. */
            while (WaitForSingleObject(g.stop, SETTLE_MS) == WAIT_TIMEOUT) {
                if (WaitForSingleObject(g.wake, 0) != WAIT_OBJECT_0) break;
            }
            if (WaitForSingleObject(g.stop, 0) == WAIT_OBJECT_0) break;
        }
    }
    if (g.ipNotify) {
        CancelMibChangeNotify2(g.ipNotify);
        g.ipNotify = NULL;
    }
    if (proxyKey) RegCloseKey(proxyKey);
    if (proxyChanged) CloseHandle(proxyChanged);
    return 0;
}

/* ------------------------------------------------------------- the interface */

void Shield_Start(HWND manager, const ClaudePackage *pkg)
{
    if (InterlockedCompareExchange(&g.running, 1, 0) != 0) return;
    InitializeCriticalSection(&g.lock);
    g.manager = manager;
    if (pkg) g.pkg = *pkg;
    LoadRules();
    g.stop = CreateEventW(NULL, TRUE, FALSE, NULL);
    g.wake = CreateEventW(NULL, FALSE, FALSE, NULL);
    g.snapshot.state = g.enabled ? SHIELD_CHECKING : SHIELD_OFF;
    g.thread = (g.stop && g.wake) ? CreateThread(NULL, 0, ShieldThread, NULL, 0, NULL) : NULL;
    if (!g.thread) {
        Util_Log(L"shield: could not start its thread (error %lu)", GetLastError());
        InterlockedExchange(&g.running, 0);
    }
}

void Shield_Stop(void)
{
    if (InterlockedCompareExchange(&g.running, 0, 1) != 1) return;
    if (g.stop) SetEvent(g.stop);
    if (g.thread) {
        WaitForSingleObject(g.thread, 15000);
        CloseHandle(g.thread);
        g.thread = NULL;
    }
    if (g.stop) { CloseHandle(g.stop); g.stop = NULL; }
    if (g.wake) { CloseHandle(g.wake); g.wake = NULL; }
    DeleteCriticalSection(&g.lock);
    g.manager = NULL;
}

void Shield_Recheck(void)
{
    if (g.wake) SetEvent(g.wake);   /* checks only when enabled; a change while off just keeps the badge off */
}

void Shield_CheckNow(void)
{
    InterlockedExchange(&g.checkNow, 1);   /* the "check now" button: read the exit and leaks once even while off */
    if (g.wake) SetEvent(g.wake);
}

void Shield_SetEnabled(BOOL on)
{
    if (!g.running) return;
    EnterCriticalSection(&g.lock);
    g.enabled = on;
    SaveRules();
    LeaveCriticalSection(&g.lock);
    Shield_Recheck();
}

BOOL Shield_IsEnabled(void)
{
    return g.running && g.enabled;
}

void Shield_Snapshot(ShieldSnapshot *out)
{
    ZeroMemory(out, sizeof *out);
    if (!g.running) { out->state = SHIELD_OFF; return; }
    EnterCriticalSection(&g.lock);
    *out = g.snapshot;
    LeaveCriticalSection(&g.lock);
}

void Shield_GetRules(CoreShieldRules *rules)
{
    if (!g.running) { LoadRules(); *rules = g.rules; return; }
    EnterCriticalSection(&g.lock);
    *rules = g.rules;
    LeaveCriticalSection(&g.lock);
}

void Shield_SetRules(const CoreShieldRules *rules)
{
    if (!g.running) return;
    EnterCriticalSection(&g.lock);
    g.rules = *rules;
    SaveRules();
    LeaveCriticalSection(&g.lock);
    Shield_Recheck();
}

BOOL Shield_Blocking(void)
{
    BOOL blocking;
    if (!g.running || !g.enabled) return FALSE;
    EnterCriticalSection(&g.lock);
    blocking = g.snapshot.state == SHIELD_STOPPED;
    LeaveCriticalSection(&g.lock);
    return blocking;
}

DWORD Shield_ProfilesBlocked(const ProfileList *list, DWORD bits, WCHAR *exit, size_t cch)
{
    WCHAR code[8] = L"", allowed[128];
    DWORD blocked = 0;
    BOOL known = FALSE;
    int i;
    if (exit && cch) exit[0] = 0;
    if (!g.running || !g.enabled) return 0;
    EnterCriticalSection(&g.lock);
    if (g.snapshot.exitKnown && !g.rules.warnOnly) {
        StringCchCopyW(code, ARRAYSIZE(code), g.snapshot.risk.countryCode[0] ? g.snapshot.risk.countryCode : g.snapshot.exitLoc);
        known = code[0] != 0;
    }
    LeaveCriticalSection(&g.lock);
    if (!known) return 0;
    for (i = 0; i < list->count; i++)
        if ((bits & (1u << i)) && ShieldRegionsOf(list->items[i].folder, allowed, ARRAYSIZE(allowed)) && !Core_RegionAllowed(code, allowed))
            blocked |= 1u << i;
    if (blocked && exit) StringCchCopyW(exit, cch, code);
    return blocked;
}

int Shield_Events(ShieldEvent *out, int max)
{
    int count;
    if (!g.running) return 0;
    EnterCriticalSection(&g.lock);
    count = min(g.eventCount, max);
    if (count > 0) memcpy(out, g.events, (size_t)count * sizeof *out);
    LeaveCriticalSection(&g.lock);
    return count;
}

const WCHAR *Shield_ReasonText(DWORD reason)
{
    switch (reason) {
    case SHIELD_STOP_REGION:     return TR(L"the exit is in a region Claude does not serve");
    case SHIELD_STOP_DATACENTER: return TR(L"the exit is a datacenter IP");
    case SHIELD_STOP_SCORE:      return TR(L"the trust score is too low");
    case SHIELD_STOP_DNS:        return TR(L"DNS leaks to another region");
    case SHIELD_STOP_UDP:        return TR(L"UDP leaks by another IP");
    case SHIELD_STOP_IPV6:       return TR(L"a public IPv6 could leave the proxy");
    case SHIELD_STOP_UNKNOWN:    return TR(L"the exit could not be read");
    case SHIELD_STOP_TUN:        return TR(L"no tunnel adapter is up");
    case SHIELD_STOP_PROXY_APP:  return TR(L"no proxy app is running");
    case SHIELD_STOP_PROFILE:    return TR(L"the exit is not a region this profile allows");
    case SHIELD_CONCERN_TZ:      return TR(L"the time zone does not match the exit");
    case SHIELD_CONCERN_LANG:    return TR(L"the language does not match the exit");
    case SHIELD_CONCERN_SPLIT:   return TR(L"the default exit is in another country");
    default:                     return L"";
    }
}

/* A one-shot check for the router, which runs in a process of its own with
 * no shield thread: one trace (claude.ai's) and the country Cloudflare gives
 * it, so a link opens without waiting on the IP services. The shield in the
 * manager catches everything else (leaks, scores, Ukraine's occupied
 * regions) on its next check. TRUE: hold the link back. Warn-only never
 * holds one back. */
BOOL Shield_QuickBlock(const ClaudePackage *pkg, const Profile *profile)
{
    WCHAR ip[64], loc[8], allowed[128];
    DWORD size = 0;
    char *body;
    (void)pkg;
    if (!Util_GetSetting(SETTING_SHIELD_ON, 0) || Util_GetSetting(SETTING_SHIELD_WARN_ONLY, 0)) return FALSE;
    body = HttpGet(kTraceHosts[0], L"/cdn-cgi/trace", L"Cache-Control: no-cache\r\n", 8192, HTTP_DEADLINE_MS, &size);
    if (!body || !Core_ParseTrace(body, size, ip, ARRAYSIZE(ip), loc, ARRAYSIZE(loc)) || !loc[0]) {
        if (body) HeapFree(GetProcessHeap(), 0, body);
        return Util_GetSetting(SETTING_SHIELD_STOP_UNKNOWN, 0) != 0;
    }
    HeapFree(GetProcessHeap(), 0, body);
    if (!Core_ClaudeRegionSupported(loc, NULL)) return TRUE;
    if (profile && ShieldRegionsOf(profile->folder, allowed, ARRAYSIZE(allowed)) && !Core_RegionAllowed(loc, allowed)) return TRUE;
    return FALSE;
}

/* ------------------------------------------------------------- the badge */

/* The badge's shield glyph and color in each state: gray off, blue
 * checking, green safe, amber a concern, red stopped. */
static WCHAR StateIcon(ShieldState state, ThemeTint *tint)
{
    switch (state) {
    case SHIELD_SAFE:     *tint = THEME_TINT_GREEN; return 0xE72E;   /* Shield (locked) */
    case SHIELD_CONCERN:  *tint = THEME_TINT_AMBER; return 0xE7BA;   /* Warning over the shield's place */
    case SHIELD_STOPPED:  *tint = THEME_TINT_RED;   return 0xE730;   /* ShieldSolid: a filled, firm shield */
    case SHIELD_CHECKING: *tint = THEME_TINT_BLUE;  return 0xE72E;
    default:              *tint = THEME_TINT_NONE;  return 0xE72E;
    }
}

static const WCHAR *StateText(ShieldState state)
{
    switch (state) {
    case SHIELD_SAFE:     return TR(L"Shield on \x00B7 the exit looks safe");
    case SHIELD_CONCERN:  return TR(L"Shield on \x00B7 something is not ideal");
    case SHIELD_STOPPED:  return TR(L"Shield on \x00B7 Claude is stopped: the exit is not safe");
    case SHIELD_CHECKING: return TR(L"Shield on \x00B7 checking the exit\x2026");
    default:              return TR(L"Shield off");
    }
}

void Shield_SetupButton(HWND dialog, int id)
{
    HWND button = GetDlgItem(dialog, id);
    if (!button) return;
    Theme_SetIconButton(button, 0xE72E, THEME_TINT_NONE);
    SetWindowPos(button, HWND_TOP, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
}

void Shield_ShowState(HWND dialog, int id, ShieldState state)
{
    HWND button = GetDlgItem(dialog, id);
    ThemeTint tint;
    WCHAR glyph = StateIcon(state, &tint);
    if (!button) return;
    Theme_SetIconButton(button, glyph, tint);
    SetWindowTextW(button, StateText(state));   /* the tip under the mouse */
    InvalidateRect(button, NULL, FALSE);
}

/* ------------------------------------------------------------- the dialog */

#define SHIELD_FRESH_MS 60000u   /* a check that ended this recently is what the dialog opens on; an older one is read again */

/* A check is asked for or running. */
static BOOL Checking(void)
{
    return g.running && (g.checkNow || g.checking);
}

/* The dialog told when a check starts and ends; NULL once it closes. */
static void SetDialog(HWND dialog)
{
    if (!g.running) return;
    EnterCriticalSection(&g.lock);
    g.dialog = dialog;
    LeaveCriticalSection(&g.lock);
}

static void Yes(WCHAR *out, size_t cch, int fact, const WCHAR *yes, const WCHAR *no)
{
    StringCchCopyW(out, cch, fact < 0 ? TR(L"unknown") : fact ? yes : no);
}

static void FillLog(HWND dialog);

static void FillDialog(HWND dialog)
{
    ShieldSnapshot s;
    WCHAR line[1024], part[256], code[8];
    const WCHAR *country;
    BOOL checking = Checking();
    ShieldState state;
    Shield_Snapshot(&s);
    /* Off, it is off whatever the last check found; just turned on, its first check is on the way. */
    state = !Shield_IsEnabled() ? SHIELD_OFF : checking || s.state == SHIELD_OFF ? SHIELD_CHECKING : s.state;
    SetDlgItemTextW(dialog, IDC_H_STATE, state == SHIELD_OFF && checking ? TR(L"Shield off \x00B7 checking the exit\x2026") : StateText(state));
    if (!s.checkedMs) {   /* no check has ended yet: nothing is shown as read */
        SetDlgItemTextW(dialog, IDC_H_EXIT, checking ? TR(L"Reading Claude's exit\x2026") : L"");
        SetDlgItemTextW(dialog, IDC_H_FACTS, L"");
        SetDlgItemTextW(dialog, IDC_H_AVAIL, L"");
        FillLog(dialog);
        return;
    }

    if (!s.exitKnown) {
        StringCchCopyW(line, ARRAYSIZE(line), TR(L"Claude's exit IP could not be read (offline, or every check host is blocked)."));
    } else {
        country = s.risk.country[0] ? s.risk.country : L"";
        StringCchCopyW(code, ARRAYSIZE(code), s.risk.countryCode[0] ? s.risk.countryCode : s.exitLoc);
        StringCchPrintfW(line, ARRAYSIZE(line), TR(L"Claude's exit: %s"), s.exitIp);
        if (country[0] || code[0]) {
            StringCchPrintfW(part, ARRAYSIZE(part), L" \x00B7 %s%s%s%s", country, country[0] && code[0] ? L" (" : L"", code, country[0] && code[0] ? L")" : L"");
            StringCchCatW(line, ARRAYSIZE(line), part);
        }
    }
    SetDlgItemTextW(dialog, IDC_H_EXIT, line);

    line[0] = 0;
    if (s.exitKnown) {
        const WCHAR *code2 = s.risk.countryCode[0] ? s.risk.countryCode : s.exitLoc;
        BOOL supported = Core_ClaudeRegionSupported(code2, s.risk.region[0] ? s.risk.region : NULL);
        StringCchPrintfW(part, ARRAYSIZE(part), TR(L"Claude supported here: %s"), supported ? TR(L"yes") : TR(L"no \x2014 Claude will risk-control this exit"));
        StringCchCatW(line, ARRAYSIZE(line), part);
        Yes(part, ARRAYSIZE(part), s.risk.datacenter, TR(L"datacenter"), TR(L"home / residential"));
        StringCchCatW(line, ARRAYSIZE(line), TR(L"\nIP type: "));
        StringCchCatW(line, ARRAYSIZE(line), s.riskKnown ? part : TR(L"unknown"));
        if (s.riskKnown && s.risk.trustScore >= 0) {
            StringCchPrintfW(part, ARRAYSIZE(part), TR(L"\nTrust score: %d / 100"), s.risk.trustScore);
            StringCchCatW(line, ARRAYSIZE(line), part);
        }
        if (s.riskKnown && (s.risk.vpn == 1 || s.risk.proxy == 1 || s.risk.tor == 1 || s.risk.abuser == 1)) {
            StringCchCatW(line, ARRAYSIZE(line), TR(L"\nFlags: "));
            if (s.risk.vpn == 1) StringCchCatW(line, ARRAYSIZE(line), L"VPN ");
            if (s.risk.proxy == 1) StringCchCatW(line, ARRAYSIZE(line), TR(L"proxy "));
            if (s.risk.tor == 1) StringCchCatW(line, ARRAYSIZE(line), L"Tor ");
            if (s.risk.abuser == 1) StringCchCatW(line, ARRAYSIZE(line), TR(L"abuse-history "));
        }
    }
    StringCchCatW(line, ARRAYSIZE(line), TR(L"\nTunnel adapter up: "));
    Yes(part, ARRAYSIZE(part), s.tunUp, TR(L"yes"), TR(L"no"));
    StringCchCatW(line, ARRAYSIZE(line), part);
    StringCchCatW(line, ARRAYSIZE(line), TR(L"   Proxy app running: "));
    Yes(part, ARRAYSIZE(part), s.proxyApp, TR(L"yes"), TR(L"no"));
    StringCchCatW(line, ARRAYSIZE(line), part);
    if (s.dnsLeak >= 0) {
        StringCchPrintfW(part, ARRAYSIZE(part), TR(L"\nDNS leak: %s"), s.dnsLeak ? TR(L"yes") : TR(L"no"));
        StringCchCatW(line, ARRAYSIZE(line), part);
    }
    if (s.udpLeak >= 0) {
        StringCchPrintfW(part, ARRAYSIZE(part), TR(L"   WebRTC/UDP leak: %s"), s.udpLeak ? TR(L"yes") : TR(L"no"));
        StringCchCatW(line, ARRAYSIZE(line), part);
    }
    if (s.ipv6[0]) {
        StringCchPrintfW(part, ARRAYSIZE(part), TR(L"\nPublic IPv6: %s%s"), s.ipv6, s.ipv6Leak == 1 ? TR(L" (could leak)") : L"");
        StringCchCatW(line, ARRAYSIZE(line), part);
    }
    if (s.tzMismatch == 1 || s.langMismatch == 1 || s.cfSplit == 1) {
        StringCchCatW(line, ARRAYSIZE(line), TR(L"\nMismatch: "));
        if (s.tzMismatch == 1) StringCchCatW(line, ARRAYSIZE(line), TR(L"time zone "));
        if (s.langMismatch == 1) StringCchCatW(line, ARRAYSIZE(line), TR(L"language "));
        if (s.cfSplit == 1) {
            StringCchPrintfW(part, ARRAYSIZE(part), TR(L"default exit %s "), s.cfLoc);
            StringCchCatW(line, ARRAYSIZE(line), part);
        }
    }
    SetDlgItemTextW(dialog, IDC_H_FACTS, line);

    line[0] = 0;
    if (s.latencyClaude >= 0 || s.latencyAnthropic >= 0) {
        WCHAR claudeMs[16], anthropicMs[16];
        StringCchCopyW(claudeMs, ARRAYSIZE(claudeMs), L"\x2014");
        StringCchCopyW(anthropicMs, ARRAYSIZE(anthropicMs), L"\x2014");
        if (s.latencyClaude >= 0) StringCchPrintfW(claudeMs, ARRAYSIZE(claudeMs), L"%dms", s.latencyClaude);
        if (s.latencyAnthropic >= 0) StringCchPrintfW(anthropicMs, ARRAYSIZE(anthropicMs), L"%dms", s.latencyAnthropic);
        StringCchPrintfW(line, ARRAYSIZE(line), TR(L"Latency: claude.ai %s, anthropic.com %s"), claudeMs, anthropicMs);
    }
    if (s.statusText[0]) {
        StringCchCatW(line, ARRAYSIZE(line), line[0] ? L"  \x00B7  " : L"");
        StringCchCatW(line, ARRAYSIZE(line), TR(L"service: "));
        StringCchCatW(line, ARRAYSIZE(line), s.statusText);
    }
    SetDlgItemTextW(dialog, IDC_H_AVAIL, line);
    FillLog(dialog);
}

static void FillLog(HWND dialog)
{
    ShieldEvent events[SHIELD_EVENTS_MAX];
    int count = Shield_Events(events, SHIELD_EVENTS_MAX), i, b;
    WCHAR log[4096], when[32], reasonList[512], part[640];
    log[0] = 0;
    for (i = 0; i < count; i++) {
        WCHAR date[32], time[16];
        reasonList[0] = 0;
        for (b = 0; b < 32; b++) {
            const WCHAR *r;
            if (!(events[i].reasons & (1u << b))) continue;
            r = Shield_ReasonText(1u << b);
            if (r[0]) {
                if (reasonList[0]) StringCchCatW(reasonList, ARRAYSIZE(reasonList), L", ");
                StringCchCatW(reasonList, ARRAYSIZE(reasonList), r);
            }
        }
        if (Util_FormatWhen(events[i].whenMs, TIME_NOSECONDS, date, ARRAYSIZE(date), time, ARRAYSIZE(time)))
            StringCchPrintfW(when, ARRAYSIZE(when), L"%s %s", date, time);
        else when[0] = 0;
        StringCchPrintfW(part, ARRAYSIZE(part), L"%s  %s%s%s\r\n", when, StateText(events[i].state),
                         reasonList[0] ? L" \x2014 " : L"", reasonList);
        StringCchCatW(log, ARRAYSIZE(log), part);
    }
    SetDlgItemTextW(dialog, IDC_H_LOG, log[0] ? log : TR(L"No events yet."));
}

/* The manager's profiles for the per-profile region row. */
static void FillProfileCombo(HWND dialog)
{
    HWND combo = GetDlgItem(dialog, IDC_H_PROFILE);
    ProfileList list;
    WCHAR regions[128];
    int i;
    SendMessageW(combo, CB_RESETCONTENT, 0, 0);
    Profiles_Load(&list, NULL);
    for (i = 0; i < list.count; i++) SendMessageW(combo, CB_ADDSTRING, 0, (LPARAM)list.items[i].name);
    if (list.count) {
        SendMessageW(combo, CB_SETCURSEL, 0, 0);
        Shield_GetProfileRegions(list.items[0].folder, regions, ARRAYSIZE(regions));
        SetDlgItemTextW(dialog, IDC_H_REGIONS, regions);
        SendDlgItemMessageW(dialog, IDC_H_REGIONS, EM_SETMODIFY, FALSE, 0);
    }
}

static void ProfileFolderAt(int index, WCHAR *out, size_t cch)
{
    ProfileList list;
    out[0] = 0;
    Profiles_Load(&list, NULL);
    if (index >= 0 && index < list.count) StringCchCopyW(out, cch, list.items[index].folder);
}

static int g_regionsProfile;   /* the profile whose regions the edit box shows */

/* The edit box's regions kept for the profile it shows, when they changed: FALSE, and not kept, when they are not
 * two-letter codes; `tell` says so then, and goes back to the box. */
static BOOL SaveRegions(HWND dialog, BOOL tell)
{
    WCHAR folder[FOLDER_CCH], regions[128];
    if (!SendDlgItemMessageW(dialog, IDC_H_REGIONS, EM_GETMODIFY, 0, 0)) return TRUE;
    GetDlgItemTextW(dialog, IDC_H_REGIONS, regions, ARRAYSIZE(regions));
    if (!Core_RegionCodes(regions)) {
        if (tell) {
            Ui_Message(dialog, MB_ICONINFORMATION, TR(L"Type two-letter country or region codes, such as US, JP."));
            SendMessageW(dialog, WM_NEXTDLGCTL, (WPARAM)GetDlgItem(dialog, IDC_H_REGIONS), TRUE);
        }
        return FALSE;
    }
    SendDlgItemMessageW(dialog, IDC_H_REGIONS, EM_SETMODIFY, FALSE, 0);
    ProfileFolderAt(g_regionsProfile, folder, ARRAYSIZE(folder));
    if (folder[0]) Shield_SetProfileRegions(folder, regions);
    return TRUE;
}

static void ReadDialogRules(HWND dialog)
{
    CoreShieldRules rules;
    BOOL ok;
    UINT score = GetDlgItemInt(dialog, IDC_H_SCORE, &ok, FALSE);
    Shield_GetRules(&rules);
    rules.allowDatacenter = IsDlgButtonChecked(dialog, IDC_H_NO_DC) != BST_CHECKED;
    rules.allowDnsLeak = IsDlgButtonChecked(dialog, IDC_H_NO_DNS) != BST_CHECKED;
    rules.allowUdpLeak = IsDlgButtonChecked(dialog, IDC_H_NO_UDP) != BST_CHECKED;
    rules.allowIpv6Leak = IsDlgButtonChecked(dialog, IDC_H_NO_IPV6) != BST_CHECKED;
    rules.stopWhenTunDown = IsDlgButtonChecked(dialog, IDC_H_STOP_TUN) == BST_CHECKED;
    rules.stopWhenProxyAppStops = IsDlgButtonChecked(dialog, IDC_H_STOP_PROXY) == BST_CHECKED;
    rules.stopWhenUnknown = IsDlgButtonChecked(dialog, IDC_H_STOP_UNKNOWN) == BST_CHECKED;
    rules.warnOnly = IsDlgButtonChecked(dialog, IDC_H_WARN) == BST_CHECKED;
    rules.minScore = ok && score <= 100 ? (int)score : 0;
    Shield_SetRules(&rules);
}

static INT_PTR CALLBACK ShieldProc(HWND dialog, UINT message, WPARAM wp, LPARAM lp)
{
    switch (message) {
    case WM_INITDIALOG: {
        CoreShieldRules rules;
        ShieldSnapshot s;
        (void)lp;
        Shield_GetRules(&rules);
        Theme_SetStrong(GetDlgItem(dialog, IDC_H_RULES));
        CheckDlgButton(dialog, IDC_H_ENABLE, Shield_IsEnabled() ? BST_CHECKED : BST_UNCHECKED);
        CheckDlgButton(dialog, IDC_H_NO_DC, rules.allowDatacenter ? BST_UNCHECKED : BST_CHECKED);
        CheckDlgButton(dialog, IDC_H_NO_DNS, rules.allowDnsLeak ? BST_UNCHECKED : BST_CHECKED);
        CheckDlgButton(dialog, IDC_H_NO_UDP, rules.allowUdpLeak ? BST_UNCHECKED : BST_CHECKED);
        CheckDlgButton(dialog, IDC_H_NO_IPV6, rules.allowIpv6Leak ? BST_UNCHECKED : BST_CHECKED);
        CheckDlgButton(dialog, IDC_H_STOP_TUN, rules.stopWhenTunDown ? BST_CHECKED : BST_UNCHECKED);
        CheckDlgButton(dialog, IDC_H_STOP_PROXY, rules.stopWhenProxyAppStops ? BST_CHECKED : BST_UNCHECKED);
        CheckDlgButton(dialog, IDC_H_STOP_UNKNOWN, rules.stopWhenUnknown ? BST_CHECKED : BST_UNCHECKED);
        CheckDlgButton(dialog, IDC_H_WARN, rules.warnOnly ? BST_CHECKED : BST_UNCHECKED);
        SendDlgItemMessageW(dialog, IDC_H_SCORE, EM_LIMITTEXT, 3, 0);
        if (rules.minScore > 0) SetDlgItemInt(dialog, IDC_H_SCORE, (UINT)rules.minScore, FALSE);
        SendDlgItemMessageW(dialog, IDC_H_REGIONS, EM_LIMITTEXT, 100, 0);
        SendDlgItemMessageW(dialog, IDC_H_REGIONS, EM_SETCUEBANNER, TRUE, (LPARAM)TR(L"Codes such as US, JP; empty: any region"));
        g_regionsProfile = 0;
        FillProfileCombo(dialog);
        /* Opened, it shows the exit as it is now: read again (while off too, enforcing nothing) unless just read. */
        SetDialog(dialog);
        Shield_Snapshot(&s);
        if (!Checking() && (!s.checkedMs || Util_NowMs() - s.checkedMs >= SHIELD_FRESH_MS)) Shield_CheckNow();
        FillDialog(dialog);
        return TRUE;
    }
    case WM_SHIELD_CHECKED:
        FillDialog(dialog);
        return TRUE;
    case WM_DESTROY:
        SetDialog(NULL);
        break;
    case WM_COMMAND:
        switch (LOWORD(wp)) {
        case IDC_H_ENABLE:
            Shield_SetEnabled(IsDlgButtonChecked(dialog, IDC_H_ENABLE) == BST_CHECKED);
            FillDialog(dialog);
            return TRUE;
        case IDC_H_RECHECK:
            if (!Checking()) {   /* one running already answers it */
                Shield_CheckNow();
                FillDialog(dialog);
            }
            return TRUE;
        case IDC_H_NO_DC: case IDC_H_NO_DNS: case IDC_H_NO_UDP: case IDC_H_NO_IPV6:
        case IDC_H_STOP_TUN: case IDC_H_STOP_PROXY: case IDC_H_STOP_UNKNOWN: case IDC_H_WARN:
            ReadDialogRules(dialog);
            return TRUE;
        case IDC_H_SCORE:
            if (HIWORD(wp) == EN_CHANGE) ReadDialogRules(dialog);
            return TRUE;
        case IDC_H_PROFILE:
            if (HIWORD(wp) == CBN_SELCHANGE) {
                WCHAR folder[FOLDER_CCH], regions[128];
                if (!SaveRegions(dialog, TRUE)) {   /* the profile shown before keeps the box until its codes are right */
                    SendDlgItemMessageW(dialog, IDC_H_PROFILE, CB_SETCURSEL, (WPARAM)g_regionsProfile, 0);
                    return TRUE;
                }
                g_regionsProfile = (int)SendDlgItemMessageW(dialog, IDC_H_PROFILE, CB_GETCURSEL, 0, 0);
                ProfileFolderAt(g_regionsProfile, folder, ARRAYSIZE(folder));
                Shield_GetProfileRegions(folder, regions, ARRAYSIZE(regions));
                SetDlgItemTextW(dialog, IDC_H_REGIONS, regions);
                SendDlgItemMessageW(dialog, IDC_H_REGIONS, EM_SETMODIFY, FALSE, 0);
            }
            return TRUE;
        case IDC_H_REGIONS:
            /* Kept once typed out, not at each key: "U" on the way to "US"
             * must not end the profile. */
            if (HIWORD(wp) == EN_KILLFOCUS) SaveRegions(dialog, FALSE);
            return TRUE;
        case IDOK:
            if (!SaveRegions(dialog, TRUE)) return TRUE;
            EndDialog(dialog, IDOK);
            return TRUE;
        case IDCANCEL:
            SaveRegions(dialog, FALSE);   /* codes typed right are kept, as the box keeps them on leaving it */
            EndDialog(dialog, IDCANCEL);
            return TRUE;
        }
        break;
    }
    return FALSE;
}

void Shield_ShowDialog(HWND owner)
{
    Ui_Dialog(owner, IDD_SHIELD, ShieldProc, 0);
}
