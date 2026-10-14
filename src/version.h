#ifndef CLAUDE_DESKTOP_PROFILES_MANAGER_VERSION_H
#define CLAUDE_DESKTOP_PROFILES_MANAGER_VERSION_H

#define APP_VERSION_MAJOR 1
#define APP_VERSION_MINOR 1
#define APP_VERSION_PATCH 8
#define APP_VERSION_STR   "1.1.8"
#define APP_VERSION_WSTR  L"" APP_VERSION_STR

/* This version, the day of its changes, what the manager shows: a day's
 * commits make one version, "## v<this day>" on top of CHANGELOG.md and
 * CHANGELOG.zh-CN.md with what they change (tools/changelog.py --stamp sets
 * it, --check checks it). */
#define APP_VERSION_DATE   "2026.10.14"
#define APP_VERSION_DATE_W L"" APP_VERSION_DATE

#endif
