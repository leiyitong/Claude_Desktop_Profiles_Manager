# Changelog

[简体中文](CHANGELOG.zh-CN.md)

> Run several Claude Desktop accounts side by side on Windows, each with its own sign-in, history and settings.

Every version of Claude Desktop Profiles Manager, newest first. A day's updates make one version, named after that day, which is the version the program shows; **About**, the icon right of Settings, shows this list too: a version chosen on the left, or **Changelog** for all of it. Each build of `main` is also published as an unsigned pre-release (`build-<number>`), which an installed copy does not update to.

## v2026.10.22

Main syncs again after Claude moved its data out of its package, and the manager sees it running again; a sync no longer sends a profile the same unchanged layout every time.

### Bug fixes

1. Main, the regular Claude icon's profile, syncs its sessions, settings and layout again: since Claude moved its data to `%LOCALAPPDATA%\Claude-Data`, the manager found no sessions for it and left it out of its sync group
2. Main shows as Running again while it runs, so changes for it wait until it closes, and opening it from the manager no longer reports that it did not start
3. Sessions without a folder that Main made before the move stay its own: a sync neither rewrites them nor sends them as another profile's
4. A sync no longer sends a profile the same layout every time when its own session presets, or a page zoom it has none of yet, keep what it holds

## v2026.10.20

Profiles gain notes and search, session imports show a preview, and backup history, per-profile results and private browser sign-in make common tasks easier to follow. Session sync, restore and cleanup keep their data safe when a write is interrupted, a file cannot be read or another operation changes the session list. Local-only inspection, migration and reading tools add the next set of capabilities without requesting Claude APIs.

### New features

1. Add a note to a profile and search profiles by name, folder or note
2. Preview new sessions, duplicates, updates and conflicts for each destination before importing
3. View the last 64 profile ZIP locations saved by this version, their size and whether the file still exists; open the containing folder or refresh the list
4. Open the log folder from About, or preview and copy diagnostics containing versions and numeric status without profile names, paths, links or log bodies
5. Open a separate Edge InPrivate or Chrome Incognito session for manual registration or sign-in, with a new browser directory each time; a complete Desktop authorization link can be pasted, and browser folders can be opened for manual cleanup after closing the browser
6. Search local sessions across all profiles from Search options, with shared sessions listed once and a choice of profile to open
7. Filter sessions by their last recorded result and read the associated summary; outdated summaries stay unknown
8. Review pending session transfers and local edits in their application order without applying them
9. Preview profile backup restores and vault recoveries, including session changes, deletion handling and sync-group effects
10. Validate backup and session ZIP files offline, including nested archives, item checksums and declared session counts, with cancellation
11. Search previous session titles and show the first matching message with nearby context, including matches outside the ordinary preview
12. Filter saved model and effort values, inspect execution settings and rewind/fork metadata, and review an artifact destination before opening its cloud link
13. Export reading copies to Markdown or plain text, and browse historical accounts/organizations and remote session metadata without changing account selection
14. Inspect local usage samples, current session/index health, configuration differences, logical storage sizes and known dated Claude Code copies on demand
15. Export and import manager names, notes, badges, actual custom images, default selection and sync relationships through an explicit existing-profile mapping with a pre-change backup
16. Save changed supported window geometry and developer flags on manager events, retain sixteen snapshots per profile, and restore selected supported values into closed profiles while preserving unrelated fields
17. Keep the latest sixty-four operation results locally, including per-profile counts, waiting status, bounded failure details and backup references
18. Recover only checked historical vault entries and archived marks, leaving other sidebar settings in place; historical transcript text is not rolled back
19. Back up and synchronize account-scoped session presets, mapping the selected account and preserving unrelated account data
20. Validate all archive members before restoring, including extra entries and nested session archives, before target data is replaced
21. Collect local scheduled-task definitions with coverage reports; tasks restored from definition-aware backups stay disabled and saved grants are not automatically applied, while older backup formats keep their existing restore behavior
22. Open valid unlisted local conversations through Claude Resume in a chosen initialized profile; Claude creates its own session entry

### Improvements

1. Profile search stays hidden until requested from the right-hand button or Ctrl+F; Esc clears and hides it. Opening, closing and restarting complete quietly, with a compact warning on failure. Session transfers show each profile's results
2. Conversation search reuses matching results for unchanged files and invalidates them on file notifications and refresh
3. The profile search label and form captions are centered on their fields; the sync choice and action keep the same height after a dialog opens or its theme changes, in both languages
4. A profile's badge stays editable while a picture is used; the saved badge appears when the picture is removed
5. Session display options gain environment grouping, ascending and descending sorting, a Show submenu for PR metadata and empty groups, and Reset to defaults; existing choices are kept

### Bug fixes

1. A session entry still being written is treated as unreadable, so it cannot make another profile lose its session
2. Replacing queued changes keeps the previous content until the new plan has been saved successfully
3. Cleaning up conversations checks the profiles and vault again before deleting, keeping sessions restored while the dialog was open
4. Restoring sign-in establishes the account before restoring sessions and settings; an account without an initialized session folder reports that it needs to be opened once
5. Settings appended after a damaged LevelDB log block can be read again after reopening the profile
6. Backups and folder copies report directory read failures instead of treating unreadable folders as empty
7. Importing stops before writing entries when a conversation cannot be extracted, and backup restore reports failed entry writes
8. Vault cleanup leaves its objects alone when it cannot collect every version's references
9. Deletion and restoration of sessions serialize updates to the vault's deleted list, preserving concurrent changes

## v2026.10.14

Backups choose from the same detailed list as what to sync and restore only what you check, a profile's window shows its own title and icon, and Esc no longer closes the manager window.

### New features

1. A profile's window shows the profile in the taskbar's thumbnail and in Alt+Tab: its icon, and the title "Claude (<name>)", or a title of your own set under **Title** in **Edit…**; a running profile takes a new title at once

### Improvements

1. A backup and a restore choose from the same list as what to sync, item by item: the sidebar's stars, groups, order, filters and width; each session's model, side pane, unread mark and cost; fonts, editor, zoom and spelling; the language; the default model; each setting; the permissions. Code sessions and Cowork, MCP servers and the sign-in are there too
2. A restore puts back only the items you check and leaves the rest of the profile as it is: restoring the MCP servers alone, say, no longer replaces every other setting. Backups made before still restore as they did

### Bug fixes

1. Esc in the main window no longer closes the manager: it only clears the sessions search box when that holds text. The close box, Alt+F4 and the notification area still close it, and Esc still closes a dialog

## v2026.10.13

Menus the program draws take a letter typed alone, the sessions view shows where each session runs, backups choose their parts as what to sync does and can keep a sign-in for any PC, several backups restore into one profile at once, a Remote Control session turns into a local one, and Settings gets the weekly copy of .claude.

### New features

1. Settings has **Back up once a week** for .claude too, as Backup & Restore has it
2. A backup can keep the sign-in for any PC: its key goes into the backup behind a password you choose, and a restore on another PC or Windows user asks for it, so the profile is signed in again at once
3. Sessions view: each session shows where it runs by its icon: a blue message on this PC, a teal cloud in the cloud, a purple phone for Remote Control, a green command prompt over SSH, a gold chat bubble for Slack
4. Restore several backups into one profile at once, such as the backups of several accounts: choose them together in **Restore from backup…**; sessions, scheduled tasks and Cowork come from all of them, and when more than one holds the settings or the sign-in, a list of restore conflicts asks which backup each comes from
5. Sessions view: a Remote Control session's menu has **Turn into a local session**: its conversation is already on this PC, so only Remote Control is turned off, and it opens as a plain local session; in a profile that is open it waits until that profile closes, and Ctrl+Z turns Remote Control back on

### Improvements

1. In the menus the program draws (a profile's right-click menu, Shortcuts), a letter typed alone chooses its item, as in Windows' menus: right-click, then E, edits
2. A backup and a restore choose their parts in a list like what to sync: Sessions, Settings and Account, each a group that folds, with its boxes
3. A restore adds the backup's scheduled tasks to the profile's instead of replacing them, and keeps the profile's list with what else it replaced

## v2026.10.12

What profiles sync can be chosen item by item, and every list whose groups fold opens or folds all at once; a changelog and an About window arrive, and lists scroll with a precision touchpad again.

### New features

1. Choose what profiles sync item by item: 26 items under 8 groups, such as new sessions, titles, archived states and deletions on their own; until you choose, profiles sync everything, permissions included
2. **Expand all** and **Collapse all** in the sessions tree's menus, and on a right-click in what to sync and the sync conflicts
3. **About**, the icon right of Settings: this version, help and the changelog together
4. A changelog in English and Simplified Chinese, one version per day, browsed by month or read whole

### Improvements

1. What to sync is a list like the sync conflicts', its groups folded at first, and the ones you open stay open the next time
2. In what to sync and the sync conflicts, a group folds and opens with its arrow, a double-click, or Left and Right
3. A right-click on a profile in the sessions view shows it and gives its menu
4. The version the manager shows is the day of its updates, the same as its version in the changelog
5. Settings keeps the language and the session vault; help moves into About
6. Help and the changelog in About are rendered from Markdown: headings bold and larger, numbered lines indented past their number, code and links styled, all in the interface's font

### Bug fixes

1. Fixed lists such as what to sync and the sessions not scrolling with a precision touchpad (two-finger pans, small steps and line scrolls)
2. Fixed a row's highlight staying in the indent of a list whose groups fold
3. Fixed About's text showing in a serif face in the Chinese interface

## v2026.10.11

The sessions view is reworked: no right column, a filter menu as Claude's sidebar has it, search in the conversations, and cut, undo and redo.

### New features

1. A filter menu for sessions as Claude's sidebar has it: status, environment, group by, sort by, empty groups and pull request status, kept between runs
2. Search sessions in their titles and folders, their conversations, or both, as typed or as a regular expression, its case matched or not
3. Ctrl+X and Ctrl+V move sessions to the profile shown
4. Undo and redo: Ctrl+Z undoes what the manager did to sessions (the last 50 actions), Ctrl+Y does it again
5. **Delete session everywhere…** is back (Shift+Del)

### Improvements

1. The sessions view has no right column: a session's folder, last use and size show in its tip
2. Archived sessions show under Archived at the bottom, with an amber mark
3. **Rename…** (F2) changes the title only; Star stays in the menu
4. The folders you open stay open the next time, but not one opened only for the selected session
5. Settings gains **Apply**, a tighter layout and the language, and its answers wrap as their language breaks lines
6. The Shortcuts and Settings icons follow Set as default at the toolbar's group gap
7. A picture taken from an .ico uses its largest image

### Bug fixes

1. Fixed taskbar buttons turning blank: the icon of the shortcut Windows makes for a profile is kept current
2. Fixed Settings' help showing the first answer only

## v2026.10.10

Notifications open the profile that showed them, sync conflicts merge item by item, and the Status column, session preview and Settings arrive.

### New features

1. Notifications clicked in Windows' notification center open the profile that showed them; with several running, you pick
2. A **Status** column shows which profiles run, followed without polling
3. **Preview** (Space) shows a session's conversation, read only
4. **Settings**, a gear at the toolbar's end: how many days the session vault keeps each list's versions, and help
5. **Back up once a week** copies .claude after a Claude closes, the two latest copies kept
6. Ctrl+C and Ctrl+V copy sessions to the profile shown

### Improvements

1. Changes made in several profiles merge item by item, and only an item two of them changed differently is a conflict; **Sync conflicts** lists each under its kind, a column per profile, the latest chosen at first
2. **Clean up sessions** lists each conversation with the profiles that had it, to restore it or move it to the Recycle Bin
3. Sessions view folders start folded, and archived sessions show apart under **Archived** at the bottom
4. Sharing or copying one session keeps its title, star and archived state
5. Rename becomes **Edit…** (F2): the title and the star in one dialog
6. A right-click on a profile's part of the details opens its menu, in place of the Actions boxes
7. Delete session everywhere is removed for now

### Bug fixes

1. Fixed a conversation that went on with a new transcript (after running out of room, or being cleared) being removed from both profiles by a sync

## v2026.10.09

The window is laid out again: a toolbar with colored icons in place of the menu bar; profiles choose what they sync, and the sidebar's layout stays the same across them.

### New features

1. A toolbar with colored icons: Open, Quit, Restart, New, Edit, Delete and Set as default, with Shortcuts and Language as icons at its end
2. **What to sync**: sessions, the sidebar, each session's model, effort and more, appearance, language, the default model, Claude's settings, and permissions if chosen
3. The sidebar's pins, project order, filters, groups and sections stay the same across a group's profiles
4. Parts changed differently in several profiles are each kept, and **Sync conflicts** lets you pick, showing each version's size
5. **Backup & Restore…**: backups and restores, exporting, importing, recovering and cleaning up sessions, and backing up .claude
6. **Create shortcut** works on several profiles at once, into one folder

### Improvements

1. No menu bar: the column beside the list holds **Sync settings…**, **Backup & Restore…** and **Repair**, with the versions at its foot; uninstalling goes through Windows' Settings > Apps
2. Every change to data asks first, in a sentence or two
3. **Repair** registers the program for claude:// links again and lets Windows ask
4. A profile's own picture shows in the notification area too
5. The profile list's menu gains Sync now, and merging, copying and moving sessions
6. One sync runs at a time

### Bug fixes

1. Fixed a profile whose session list was wiped deleting sessions in the others
2. Fixed Exit not closing the window while the search box held text
3. Fixed icons showing stray characters

## v2026.10.08

Profiles keep the same sessions by copying entries, session lists can be recovered and cleaned up, and the interface is in English and Simplified Chinese.

### New features

1. Session groups: a group's profiles keep the same sessions by copying entries, the latest change winning and a deletion going everywhere
2. **Recover sessions…** puts a kept version of a profile's list back, and a profile that lost its list gets it back by itself
3. **Clean up deleted sessions…** and **Back up .claude…**
4. Open, Quit, Restart and Sync show a progress bar

### Improvements

1. A session without a folder works in each profile's own "no folder" area, and one two profiles went on with apart is kept as two
2. Overwrite becomes Copy all sessions and Move all sessions
3. The version shown is the time the program was built

### Bug fixes

1. Fixed list check boxes drawn off their place at 175 % scaling

### Breaking changes

1. The interface is in English and Simplified Chinese only; the ten other languages are removed, and a language no longer offered follows Windows
2. Session folders are no longer shared through a link (Claude no longer writes sessions through one); a folder still linked is given its own again

## v2026.10.07

Quit and profile backups arrive, and the profile list's right-click menu holds every action.

### New features

1. **Quit** closes the Claude of the profiles selected as Windows does before an update
2. Profile backups: **Back up…** saves chosen parts (Code sessions, Cowork sessions, settings, sign-in) to .zip files, **Restore from backup…** puts them into a closed profile
3. Session folders linked to another profile's folder or one of yours, for several profiles at once
4. Every build of main is published on GitHub as a pre-release

### Improvements

1. The profile list's right-click menu holds every action, and opens with a touchpad's right click and below the rows
2. Ctrl+click and Shift+click on a folder's row in the sessions view select all its sessions

### Bug fixes

1. Fixed deleting, linking, restoring or uninstalling a profile failing with "access denied" once the sessions view had read it
2. Fixed the main window's last column overflowing into a horizontal scroll bar at its minimum width

## v2026.10.06

Sessions are merged, exported and imported in numbers, a claude:// link asks which profile opens it, and profile icons can be your own.

### New features

1. **Merge all sessions…** and **Overwrite sessions…**
2. Sessions exported to a .zip archive and imported into other profiles, what a change replaces backed up first
3. With several profiles, you pick which one opens a claude:// link; for a sign-in, the window that started it is selected
4. Twenty colors, a badge of one or two characters of your own, and a picture of your own as the icon
5. Ctrl and Shift select several profiles and sessions, and a folder's menu acts on every session it shows

### Bug fixes

1. Fixed the link dialog losing its choice after a click beside the rows or Ctrl+Space
2. Fixed uninstalling hanging without a window

## v2026.10.03

Release 1.1.0: reliable taskbar pins, twelve interface languages and a faster sessions view.

### New features

1. Twelve interface languages, following Windows or picked in the Language menu
2. The main window can be resized and maximized

### Improvements

1. Taskbar pins are written as Windows writes them, so Windows keeps them
2. The sessions view reads on a background thread, so it is faster
3. Delete session everywhere removes what Claude's own delete removes
4. A profile closed by a Claude update opens again after it

## v2026.09.30

First release, 1.0.0: several Claude Desktop accounts side by side on Windows; 1.0.1 then fixed reading Main's sessions.

### New features

1. Profiles, each with its own sign-in, history and settings, created, renamed, recolored and deleted
2. Profile badges on shortcuts, taskbar buttons and notification-area icons, each in its color
3. Taskbar pins, Start menu entries and opening at Windows sign-in
4. A browser sign-in returns to the window that started it
5. Claude Code sessions: open, share, copy, rename, favorite, remove or delete everywhere
6. Settings copied from another profile, and updates in one click

### Bug fixes

1. Fixed Main's sessions not found when Claude keeps Main's data in its package's private folder (1.0.1)
