# How Claude Desktop Profiles Manager works

How Claude Desktop Profiles Manager runs one Claude Desktop per profile, tells which ones are running, gives each its own taskbar button, pin and notification-area icon, and routes `claude://` links.

The observations below were made on Claude Desktop 2.16120.0 (MSIX) and Windows 11 (build 26200). All of it relies on undocumented behavior of Claude or Windows; the build checks Claude's side (see the last section).

## Profiles

Claude Desktop is an Electron (Chromium) app, and Chromium allows one instance per user data folder. Started with another `--user-data-dir`, it runs a second, independent instance with its own sign-in.

| Profile | Data folder seen by Claude | Started with |
|---|---|---|
| Main | `%APPDATA%\Claude` | no argument, like the Start menu |
| Others | `%APPDATA%\Claude-<name>` | `--user-data-dir="%APPDATA%\Claude-<name>"` |

- Folders stay directly under `%APPDATA%`: Cowork's VM service looks for its disk image in `%APPDATA%\<folder>\vm_bundles`.
- A folder is never renamed, since Claude stores absolute paths in it. Renaming a profile only changes its display name, kept in `HKCU\Software\Claude Desktop Profiles Manager\Profiles\<folder>`.
- Claude itself uses `%APPDATA%\Claude-3p` and `%LOCALAPPDATA%\<folder>-Data`, so the names `3p`, `Data`, `...-3p` and `...-Data` are refused.
- A profile is `%APPDATA%\Claude`, or a `%APPDATA%\Claude-*` folder with an allowed name that holds a Chromium `Local State` file or was created by the manager. A folder linked elsewhere (a junction or directory symbolic link) counts once its `Local State` is reachable.

The manager's **Role** column:

| Role | Meaning |
|---|---|
| Claude icon, default | Main, which the regular Claude icon opens, and also the default profile |
| Claude icon | Main; another profile is the default one |
| Default | The profile picked with **Set as default**: it is selected for `claude://` links while Claude is closed |
| *(empty)* | Any other profile |

## Starting Claude

Claude Desktop is an MSIX package, and the way it is started matters:

| Started by | Package identity | Updates |
|---|---|---|
| `CreateProcess` on `WindowsApps\...\app\Claude.exe` | no | fail: "Can not find Squirrel" |
| Start menu, or `IApplicationActivationManager::ActivateApplication` | yes | work (MSIX updater) |

The manager uses `ActivateApplication(<family>!Claude, arguments)`: the process gets its identity and the arguments arrive untouched. `CreateProcess` is only a fallback when activation fails. Claude logs which case it is in at start-up (the `[MSIX]` lines of `main.log`).

With its identity, Claude writes to AppData through the package's private copy, `%LOCALAPPDATA%\Packages\<family>\LocalCache`:

- Main's data is in `%APPDATA%\Claude` when that folder exists, else in `LocalCache\Roaming\Claude` (Claude then logs `Filesystem virtualization active`), as on an installation whose Main profile was first opened through the package;
- a `--user-data-dir` that does not exist yet is created under `LocalCache\Roaming`, so the manager creates a profile's folder before its first start (never Main's);
- a profile's logs (`%LOCALAPPDATA%\<folder>\logs`) can land in `LocalCache\Local\<folder>\logs`: both places are read, and both are removed with the profile.

The manager therefore keeps two paths per profile: the one Claude sees, `%APPDATA%\<folder>`, which identifies a running instance, goes into launch arguments and matches the folders recorded in sessions; and the one files are read and written through, which is Main's `LocalCache\Roaming\Claude` when `%APPDATA%\Claude` does not exist. Finding it never creates `%APPDATA%\Claude`.

The package is `Claude_pzs8sxrjxfjjc` (from claude.ai) or `AnthropicPBC.Claude_fnn82j28hfe8t` (Microsoft Store), else any other package named Claude.

A process started from Claude Desktop (a Claude Code terminal, for example) runs inside that package: it sees the private copy, not the real registry and AppData. Test from a process started by Explorer or WMI.

## Running profiles

A running Chromium instance owns a message-only window of class `Chrome_MessageWindow`, titled with its data folder; a second launch uses it to hand over its arguments. The manager lists those windows, without opening or locking anything. The window's process is the profile's main process, and its most recently used Claude window is the first one in Z-order.

## Taskbar and notification area

A profile opened from the manager gets its own taskbar button with its badge, can be pinned from the manager, and shows its notification-area icon in its color, or its own picture when it has one (while Claude's **System tray** setting is on). A small watcher, `ClaudeDesktopProfilesManager.exe --watch "<folder>"`, does this while that profile's Claude runs: it only reacts to events and exits with Claude, unless Claude closed for an update (below).

The watcher knows that the profile's Claude has started when that Claude creates its `Chrome_MessageWindow`: it watches the window creations of the Claude process started for the profile, whose exit ends the wait. When that process is not known (Windows starts Claude itself after an update), it watches the profile's data folder instead, where a starting Claude creates `lockfile` right before that window. The wait lasts a minute and a half at most, a minute after an update. Renaming or recoloring a profile updates its button, pin and icon. A new display scale redraws the icon at the notification area's new size: the watcher's hidden window gets no `WM_DPICHANGED` then, but `WM_DISPLAYCHANGE`, and the size comes from the taskbar's own scale.

A profile started without the manager (the regular Claude icon) keeps Claude's button until the manager opens it or routes a link to it: its watcher then starts. A profile already watched gets no second watcher: its watcher takes the new start over, even when it was about to end.

A profile is pinned while Windows' pin list holds an active pin whose shortcut opens that profile (or Claude's own Apps folder item, for Main). A shortcut left in the pins folder, or a pin Windows retired, does not count. The manager follows changes to the pin list, without polling.

The manager writes its pins in the form Windows itself writes them, so Windows keeps them, and leaves other applications' pins as they are. It never writes over a list in a form it does not know, nor over a change Windows made meanwhile: it starts again from the new list, three times at most. A new pin takes the place of the profile's pin, active or retired, else goes last; the profile's other active pins are dropped, since Windows would unpin them and delete the shortcut they share. When the manager opens, after it changes one of its pins' shortcuts, and when a profile is pinned again, damaged pins of its own are repaired; a pin that is fine is left exactly as it is, and an unpinned profile stays unpinned. At uninstall, the active pins of the manager and its profiles leave the list, then their shortcuts are deleted.

A profile's icon is the installed Claude icon with a badge: a disc in one of twenty colors (the first eight are the colors of release 1.1, at the same index), with the name's initial or a text of one or two characters of the user's own. A picture of the user's own can replace it: Windows' codecs (WIC) read it, so PNG, JPEG, GIF, BMP, TIFF and ICO always work and WebP or HEIF once their extension is installed; a GIF shows its first frame, since Windows' icons do not move. The picture is cut to its middle square, scaled to 256 pixels and kept as `pictures\<folder>.png` in the manager's folder, so the file chosen can go; the profile records its stamp (a hash of its pixels), and a picture whose pixels do not match is not shown. The icon file's name encodes the name, color, badge text and picture stamp, so a change makes a new file (a profile without a badge text or a picture keeps the name release 1.1 gave it). The color menu lists the colors no other profile uses first, then, under a line, the others with the profiles that use them; a new profile starts with the first free one. The notification-area icon is Claude's own tray glyph in the profile's color; a profile with a picture shows the picture there.

The taskbar part is the Taskbar Module, under its own license (see `LICENSE`); the notification-area icon (`tray.c`) is not.

### Claude updates

A Claude update goes like this:

1. Claude's updater, in one of the open windows, logs `beforeQuitForUpdate handler fired, going down for update`.
2. Windows closes every Claude window to replace the package; each logs `Windows session ending (close-app) - quitting the app`.
3. Windows installs the new version and opens Claude again once, without arguments. That starts Main, within about half a minute, even when only another profile was open. The other profiles stay closed.

When its Claude exits, the watcher reads the last quit line of the profile's `main.log`. If it is one of the two above, from the last five minutes, the watcher waits, at most 2 minutes, for a Claude package other than the one that closed; changes to the user's package list and to the Apps folder wake it. Once the new package is there, Main is first given a minute to come back from Windows' own restart, and any other profile opens again at once; the watcher then watches the new window, so button, badge and notification-area color come back.

A quit from the window or the tray (`Quitting app...`, `beforeQuit:`, `willQuit:`), a Windows shutdown or a crash leaves the profile closed, as does a close for an update after which no new package comes. Opening the profile during the wait ends it.

The manager's own **Quit** asks Claude to end the way Windows does before an update (Restart Manager), so Claude logs the same `Windows session ending (close-app)` line: the manager first tells the profile's watcher (`Taskbar_QuitComing`), which then takes the close for a quit, not for an update.

## Start menu

Install puts a **Claude Desktop Profiles Manager** folder in the user's Start menu with the manager's shortcut, made again when the manager starts if it was deleted. **Add to Start menu** puts a profile shortcut (`Claude (<name>).lnk`) in that folder and **Remove from Start menu** deletes it. Like every profile shortcut the manager makes, it is recorded, so it follows renames and colors and goes with the profile. The button looks again when the selection changes, when the window comes back to the front, and when that folder, Programs, the desktop or the pins folder changes. At uninstall the manager's shortcut goes last, once its windows are closed: the taskbar takes their icon from it.

## The manager window

The manager follows changes made outside it without polling: a thread waits for registry notifications on `HKCU\Software\Claude Desktop Profiles Manager` (names, colors, default), on `...\Shell\Associations\UrlAssociations` (the app picked for `claude://`), on the user's package list (Claude installed or updated) and on the taskbar's pin list, and for folder notifications on `%APPDATA%` (profile folders made or removed); a burst of them refreshes the window once. Coming back to the window refreshes it too.

Interface text is English or Simplified Chinese: Windows' preferred UI language when it is one of them (every Chinese locale takes the simplified catalog), else English, or the one picked in **Program** → **Language** (stored in the manager's `Language` registry value; a language an earlier version saved and no longer offered follows Windows). Profile names, session titles, paths and log messages are never translated. Text is measured with a font covering the selected script. Where the text names Claude's own interface (its Code tab, its No folder group, its Quit command, its stars), it uses Claude's words. After a change of language, what Windows shows of the program follows it, in the background: the `claude://` handler's name and description, and the tooltips of the manager's shortcut and of the profiles' shortcuts and pins.

The main window has a menu bar: **Sessions** (sync, merge, copy or move every session, keep sessions the same, export, import, recover, clean up), **Program** (language, repair, uninstall, exit) and **Shortcuts** (desktop shortcut, shortcut elsewhere, taskbar pin, Start menu; each for every profile selected, the ones elsewhere into one folder then), each filled as it opens for the profiles selected; in dark mode the window draws the bar itself, Windows having no dark one. Under it a toolbar, its buttons with icons from Windows' own icon font (Segoe Fluent Icons, else Segoe MDL2 Assets): **Open**, **Quit**, **Restart**; **New…**, **Edit…**, **Delete…**; **Set as default**. In the sessions view it acts on the profile shown. Beside the list, a column: **Sessions view** (**Back** in the other view, each with its icon) on top, then **Sync sessions**, **Repair**, **Back up .claude…** and the note on the default profile (in the sessions view, the session's details), and at its foot Claude Desktop's version over the state of `claude://` links, then this copy's version (the time it was built, `yyyy.mm.dd hh:mm`) and **Update** when there is one. The list's menu holds the toolbar's actions, **Sync sessions**, keeping sessions the same and the backups. Open, Quit, Restart and Sync run on a thread of their own: the profiles' Claude quit, their groups made the same (the progress takes the status's place, also for the sync a profile's watcher makes when it closes, which posts it to the window), then they open. Several profiles can be selected: every action that can takes them all; **Edit…** of several sets only how their sessions are kept.

Every action that changes data asks first, in words and by folder: syncing, keeping sessions the same (from the Sessions menu or a profile's dialog), merging, copying, moving, importing and recovering sessions name each profile's folder and what in it changes (`claude-code-sessions`, `scratch-workspaces`, and `claude_desktop_config.json` when the sidebar follows), Claude Code's `projects` folder when sessions get a copy there, and the `backups` folder where what is replaced goes first; cleaning up names the folders the conversations' files leave for the Recycle Bin, and where Claude Code's folder is copied first when that was chosen; deleting a profile, unlinking a session folder, restoring a backup and repairing name what they remove, replace or rewrite (Repair also its registry keys and the folders of its shortcuts and pins). Paths show `%LOCALAPPDATA%`, `%APPDATA%` and `%USERPROFILE%` for their start. Opening, quitting and restarting, which make a profile's group the same along the way, do not ask.

The main window can be resized and maximized. Its minimum size fits both interface languages and both views, so changing the language, the view or the status line never resizes it. A wider window keeps the content at a reading width, centered, and gives the lists the extra height. The Profile and Role columns start at the same width in every language and the last column takes the rest; a column the user sized keeps its width.

Every control's look comes from `theme.c`; [UI-COMPONENTS.md](UI-COMPONENTS.md) describes how to add one. Text cut short in a list, tree or label shows whole in a tooltip at once. The color selector and the session **Actions** buttons open Windows' own menus.

## Claude Code sessions

A Code-tab session is stored in two places:

- its conversation, the transcript: `%USERPROFILE%\.claude\projects\<project>\<id>.jsonl` (or under `CLAUDE_CONFIG_DIR` when it is set), in the Claude Code folder every profile shares;
- one entry per profile that lists it, in that profile's `claude-code-sessions\<account>\<organization>\local_<...>.json`: title, star (`isStarred`), folder, archived, last activity, and the session's earlier transcripts (`priorCliSessionIds`, `preClearCliSessionId`, `unarchivedCliSessionId`), which its size counts unless another session goes on with one. A profile can list a session twice; every entry of it is changed together.

Claude shows the entries of the account signed in (`lastKnownAccountUuid` in the profile's `config.json`; without one, the account with the latest entry) and of its organization with the latest entry; the entries of an account signed in before stay on disk but are not shown. A session without a project folder works in `<profile data>\scratch-workspaces\<account>\<organization>\scratch-...`, and Claude shows it under "no folder" only in the profile that owns that area. A session run over SSH (`sshConfig`), in WSL (`wslConfig`) or in the cloud (`cloudSessionId`, or `movedToCloud` set to anything but `null` or `false`) has its conversation elsewhere: the manager only counts it.

A session's `cwd` stays the path Claude recorded; opening, copying or deleting its scratch folder goes through the profile's file path (above), which differs for a virtualized Main. Paths can be longer than MAX_PATH: transcripts and entries are reached through their `\\?\` form. The Recycle Bin takes no path of MAX_PATH characters or more, so a removal that would need it for one is refused with that path, and nothing moves. A junction or directory link among the paths is removed as a link, never followed.

**Sessions >** turns the manager window to its sessions view, **< Back** turns it back. On the left the profiles, with how many sessions each lists; in the middle the chosen profile's sessions as a tree, the starred ones first, then by folder, filtered by the search box; on the right the selected session: its folder, last use and size, then each profile with what the session is there (not listed, starred, its title there, open or in use, archived) and an **Actions** menu, and **Delete session everywhere** at the bottom. The tree's context menu (Shift+F10 too) holds the shown profile's actions (Open, Rename, Star, Remove, or **Keep** while a removal waits), **Open in**, **Share with** and **Copy to** for the other profiles, **Show folder** and **Delete session everywhere**; Enter opens the session, F2 renames it, Delete removes it. Ctrl+click and Shift+click select several sessions; their menu, and the menu of a folder or of Starred for all the sessions it shows, shares, copies, exports, stars, unstars or removes them together, or deletes them everywhere, and Delete removes them. A right-click below the rows exports or imports sessions. An empty tree says why (not signed in, no session yet, all archived, nothing matches the search, only remote sessions, entries that could not be read).

Sessions are read on a background thread from a snapshot of the profiles, which publishes a complete result to the window. The view follows the disk through folder notifications on each profile's entries folder and on the transcripts folder; a burst of changes reloads it once, at most twice a second, and a change of profiles cancels a read in progress.

### Session actions

- **Open** starts the profile, or reaches its window, with `claude://resume?session=<id>`. Claude opens the session, and first adds its entry when the profile does not list it yet (`local_<id>`).
- **Share** sends that link to a profile that does not list the session: both profiles then go on with the same conversation. A session without a folder shows there in a folder named `scratch-...`, not under "no folder", which is the other profile's area.
- **Copy** writes a new conversation with a new id: the transcript with every `sessionId` replaced, up to its last whole line; a session without a folder gets a copy of its scratch folder in the target's own area, with every `cwd` pointed at it (Windows shows the progress of a long folder copy, which can be cancelled). The target then opens it with the link, and the copy goes on separately. A copy that cannot be opened is deleted again.
- **Rename**, **Star** and **Remove** change that profile's entries of the session: `title` (with `titleSource` `user`), `isStarred`, or the entry itself, which goes to the Recycle Bin with Claude's own `deleted_<id>` marks (the time in ms) beside it, one for each id of the session that no other session there claims, as Claude's own delete does. A shared or copied session gets the title it has in the profile it came from.
- **Delete session everywhere** moves to the Recycle Bin, in one operation, what Claude's own delete removes: the session's entries in each profile (with the transcript Claude staged when it took the session in); its scratch folder, unless another session works in it; and for each of its transcripts that no other session goes on with, `<id>.jsonl`, the `<id>` folder, `.jsonl.pre-import`, `.ccr-tip.json`, `.precompact.json` and `.desktop-released.json` in every project folder, Claude Code's temporary folder of the session (`<temp>\claude\<project>\<id>`), and the session's `file-history`, `session-env`, `uploads`, `tasks`, `image-cache`, `debug`, `usage-data` and `startup-perf` items in the Claude Code folder. It then writes the `deleted_<id>` marks and drops the changes waiting for the session. It is refused while a profile that lists the session is open (that Claude would write it back), or while any Claude Code runs it.

Archiving stays in Claude: its own archive also cleans up the session's worktree and records it in `archived-sessions.idx`.

### Sessions between profiles

**Merge all sessions…**, **Copy all sessions to…** and **Move all sessions to…** in the Sessions menu, the menus of several sessions, and **Export sessions…** and **Import sessions…** (in the Sessions menu and the menu of the sessions tree) send sessions to several profiles at once. Ctrl+click and Shift+click select several profiles in the profile list too. A dialog lists the profiles checked and says what will happen; a profile not signed in to Claude yet has no entries folder and is left out.

- **Merge** gives each profile checked the sessions the others list, with the entry of the profile that used the session last: an older entry is replaced (the profile keeps its own entry id), a newer one stays.
- **Copy all sessions to** lists every session of the profiles selected in the profiles checked too (shared: the same conversations). **Move all sessions to** then takes them out of the profiles selected, their entries to the backup and Claude's `deleted_<id>` marks left beside, so that Claude does not take them in again; it is refused for a profile whose sessions are kept the same as others', whose sessions would go from those too.
- **Share** and **Copy** of several sessions write each entry instead of opening the session with a link, so no profile opens.
- **Export** writes a ZIP archive, stored rather than compressed: `manifest.json`, each session's entry, and its conversation's files in the Claude Code folder (the items **Delete session everywhere** would remove there). **Import** adds the conversation files that are missing, never replacing one and only where a conversation keeps its files, then sends the entries to the profiles checked.

An entry sent to a profile is written as Claude writes its own: `local_<id>.json` in the folder of the account signed in and its organization, through a temporary file put in place. A running profile's entries are not written: what is sent to it waits in `pending-sync-<folder>.txt`, with the entries in `pending-sync-<folder>\`, and is made when it closes, at the moments the waiting changes below are made. Each change is checked again then: a session used there since keeps its entry, and one Claude marked deleted there stays deleted unless it was overwritten, shared or imported on purpose. What a change replaces or removes is first copied to `backups\<time>\<folder>` in the manager's folder, which keeps the latest 20. A summary then says what was added, updated, removed, left as it was, and what waits for a profile to close.

A running Claude keeps its sessions in memory and writes them back: an entry changed under it is overwritten. A change to a running profile therefore waits in `%LOCALAPPDATA%\Claude Desktop Profiles Manager\pending-sessions-<folder>.txt` and is made when that profile closes: by its watcher, before the manager starts it again, or when the sessions view finds it closed. The session then reads "Changes made when <profile> closes", and **Keep in <profile>** cancels a removal still waiting. A newer change replaces what waited of its kind, a removal replaces everything, and any other change cancels a waiting removal; at most 256 changes wait. A pending file is written to a temporary copy, flushed, then put in place, and one that cannot be read is never written over. Entry changes and pending-file rewrites share a per-profile mutex across processes; a waiting change is made only once Claude is checked closed, and a removal overtaken by Claude's start waits again. When a profile starts while Windows asks about a removal, its Claude keeps the session, and the entry goes when that Claude closes.

Claude Code records each session it runs in `<Claude Code folder>\sessions\<pid>.json` (`sessionId`, `procStart`). A session whose process still runs, with that start time, under a profile's Claude is "in use" there; one in use in two profiles at once gets a warning, since each goes on from what it read.

### Sessions kept the same, and the session vault

Claude no longer writes a profile's entries through a link: before it saves one, it refuses an entries folder that is a reparse point, or whose path leads elsewhere (`main.log` then says `Failed to save session ...: Refusing non-directory at private dir path (symlink/file plant)`). A profile whose entries folder is a junction to another profile's, as earlier versions of the manager made them, reads the shared sessions but loses every change it makes (titles, stars, the last activity that orders them, new sessions) once it closes. Such links are only read and taken away now: when the manager opens it offers, for each, to give the profile a folder of its own again (with the entries the shared one holds) and to keep its sessions the same as the other profile's instead; **Unlink sessions folder…** stays in the profile list's menu.

Profiles keep the same sessions in groups: `SyncSessions` under each one's key holds its group's number (1 for the group an earlier version made). **Sessions** in the profile dialog (also for several profiles edited together, and for a new profile, which gets the group's sessions once it is signed in) and **Keep sessions the same as** in the Sessions menu put the profiles in the group of the one chosen, or in a new group with it; **Stop keeping sessions the same** and **Its own, kept apart** take them out. A group left with one profile is none. Grouping is transitive: a profile kept the same as one of a group joins the whole group, and the Sessions column names every member. Each group's sessions are made the same each time one of its profiles closes (its watcher, after the waiting changes), before one opens through the manager, on **Sync sessions**, and when the manager opens. A profile of the group opened alone (no link, not from the manager) while another one runs asks to quit that one first: an open profile gets the others' changes only once it closes, which **Restart** does at once.

Each session, by its transcript id, is resolved against the last version the vault kept of the group's list (`Core_MirrorResolve`); an entry's hash leaves out its own id and, for a session without a folder, where that folder's area is: in each profile its entry, without its own id, hashed, with the time its file was written; or Claude's `deleted_<id>` mark and its time; or nothing. A profile that had that version's list and no longer lists a session took it away; one that never had the list, or whose entries folder is not the one the version knew (another path, or made since: Claude reinstalled, another account), or one of whose entries could not be read, only lacks it. An entry changed since the version wins, the one written last first, unless a deletion came later; a deletion, or a session taken away, goes from every profile; a profile that only lacks a session gets it. A profile that was open when a change was sent to it shows the old entry until it closes: as long as it has not used that session since, the change waiting for it (`pending-sync-<folder>.txt`) stands for its entry, so what it still shows is no change. `archived-sessions.idx` follows the same way.

Claude's sidebar keeps its pins (`starred-local-code-sessions`, `dframe-local-slice`), its project order (`code-projects-order.<account>`) and its filter (`code-sessions-status-filter`, `code-sessions-selected-environments-v2`, `code-sessions-show-empty-projects` and `code-sessions-show-pr-status`, each `.<account>`, and `ccd-sessions-filter`, the projects chosen) in `claude_desktop_config.json`, under `preferences.epitaxyPrefs`, naming sessions by their entry's own id (`local_<id>`, `code:local_<id>`). The vault reads them as one layout, each session named by its transcript, and keeps each of its parts the same as the entries: the latest changed since the last version goes to the others, each getting its own entry ids and its own account's keys, the rest of its settings as they were. A running Claude rewrites its settings all the time (a newer one adds fields to the sections): taken part by part, that rewrite changes none of the other parts, and a profile with a layout waiting for it takes no part until it closes, nor does one new to the group (or that lost its list), which takes the group's. Kept for the first time, with no version to go by, the fullest of each part is the one kept.

The sidebar's manual groups and their sections (`customGroupsByScope`, `codeSidebarByScope`, each by `<account>/<organization>`), its **Edit sidebar** choices (`navPinnedIds`), the groups folded (`collapsedGroups`), how sessions are grouped and sorted (`groupByByMode`, `sortByByMode`), the filter of Recents, where Routines goes, the sidebar's width and the interface font, Claude's web UI keeps in its web storage, under `state` of the value `dframe-store`; the editor's font, code theme and text size in the value `epitaxy-editor-prefs`. At each start it writes its groups and sections from there over its two copies, `dframe-group-scopes` and `dframe-code-sections` in `claude_desktop_config.json` and `LSS-persisted.<the same>` in the web storage: groups written into that file alone while Claude is closed are gone once it has started (measured on 2.31226, the groups of one profile written into another's file three times). So `webstore.c` reads these parts from the web storage as part of the layout, and the vault writes them there, with both copies alike (a copy's `timestamp` made now), while that Claude is closed; what else the web UI keeps there stays as it was.

The web storage is Chromium's Local Storage of `https://claude.ai`: the LevelDB database `<profile>\Local Storage\leveldb`. A key is `_https://claude.ai`, a zero byte, then the name, after a byte telling its encoding (1: Latin-1, 0: UTF-16LE); a value starts with the same byte. `CURRENT` names the manifest, whose edits name the live tables (`.ldb`), the log and the last sequence number; the log holds write batches in 32 KiB blocks of records, each with a masked CRC-32C. Reading takes every table and every log LevelDB would replay, each key's value of the highest sequence. Writing adds one write batch to the log, after its last whole record (in the next block when a write was cut short), with the next sequence number: LevelDB takes it in when it next opens, as it takes in its own last writes after a crash (measured on 2.31226: `Recovering log #3`, `Reusing old log`, the value read back by Claude). The folder is copied to the backup first; nothing is written while that Claude runs, and LevelDB's `LOCK` is never opened.

A session without a folder works in `<profile data>\scratch-workspaces\<account>\<organization>\scratch-...`, and Claude shows it under "no folder" only in the profile whose area that is. Sent to another profile of the group, its entry's `cwd` and `originCwd` name the same folder in that profile's own area, which is made with the files of the one it came from that it lacks or holds older (nothing is deleted, no link is made); its transcript stays where it is, and Claude Code finds it by its id. An entry an earlier version sent as it was moves to its own area at the next sync.

A session two profiles went on with apart since the last version (both entries changed, both with activity after it) whose transcript holds a branch for each (`Core_TranscriptBranches`: the last message each used, following `parentUuid`) is kept as two: the branch of the profile that went on first becomes a session of its own, a copy of the transcript without the lines of the other branch alone, under a new id, with that profile's entry titled "<title> (<profile>)", sent to every profile of the group; the original goes on with the latest branch. One whose transcript holds one line of conversation (one went on from the other) stays one.

The entries go through the sends above (backed up first, at once where a profile is closed, else once it closes), and the version is written only once every change was made or waits.

The vault is `vault` in the manager's folder, out of Claude's package, so a reinstalled Claude finds it: `lists\<list>\<UTC time>.txt` names each session a version lists, its entry (kept once, `objects\<hash>.json`) and the profiles that had it; `deleted.txt` names the sessions deleted (their entries' own ids and transcripts), which nothing here brings back. A version is written only when the list changed, and every version stays. The group's list is `group`; a profile alone keeps one named after its folder, kept when it closes, opens or the manager opens, which only follows it: what it lost stays listed, to be recovered.

**Recover sessions…** puts a version of a profile's list back into it, closed: each session the version lists (as it was then, but not a deleted one), its `archived-sessions.idx`, and Claude's marks of the deleted sessions it does not list, so that Claude's own import of Claude Code's sessions leaves them out too. A profile of the group recovered to an older version passes it on to the others.

**Clean up deleted sessions…** lists the transcripts in Claude Code's folder that no profile lists and no kept list names, nor any transcript a listed session goes on from: the deleted ones (a profile's `deleted_<id>` mark, or the vault's), checked; the ones no list ever had (made in a terminal, say), unchecked, and only when written more than 24 hours ago; none that a Claude Code runs now. The ones checked go, as Claude's own delete removes what a transcript has in Claude Code's folders (never a working folder), to the Recycle Bin, and their ids join the vault's deleted ones; a copy of Claude Code's folder can be made first. **Back up .claude…** copies Claude Code's folder beside it as `<name>_<yyyymmdd>` (then `_2`, `_3`… that day); Windows shows the copy's progress and asks about a file in use.

## Opening at sign-in

**Open at Windows sign-in** puts a profile shortcut (`Claude (<name>).lnk`) in the user's Startup folder, which Windows opens at sign-in: the profile starts with its own button and icon. Claude's own start-up setting cannot do this for a profile other than Main, since it starts Claude without arguments. The shortcut is recorded like the others.

## Settings copy

A new profile can start with some settings of an existing one. From `claude_desktop_config.json`: `mcpServers`, `isHardwareAccelerationDisabled` and `preferences.menuBarEnabled`; from `config.json`: `locale` and `userThemeMode`. Nothing else is copied: the sign-in, account ids and everything tied to an account stay behind. The files are written before the profile's first start and never over existing ones.

## Updates

When the manager opens, at most every 4 hours, it reads the tag of the latest release (`api.github.com/repos/<owner>/<repo>/releases/latest`) on a background thread. A newer one shows next to the version with an **Update** button, which downloads that release's `ClaudeDesktopProfilesManager.exe` to `%TEMP%` and checks it while keeping it open, so it cannot change before it runs: Windows must find it signed by the author (a valid signature, a trusted and unrevoked certificate whose common name is exactly the one the releases are signed with), and its file version must be the release's. Otherwise the file is deleted and nothing runs. The release check gives up after about a minute, the download after about ten. The new copy then installs itself over the current one (written next to it, then put in place in one step, the old copy kept on failure), restarts the watchers, reopens the manager and deletes the download. Nothing else is sent.

## claude:// links

A browser sign-in returns to the app through a `claude://` link, and Windows gives every such link to one program. Claude accepts a sign-in only in the window that opened the browser, so with several windows open the link must reach the right one. With several profiles, the manager asks which one opens each link, the likely one selected.

### Which app opens them

Windows opens `claude://` links with the default app picked for `claude://`, kept in `HKCU\Software\Microsoft\Windows\Shell\Associations\UrlAssociations\claude\UserChoiceLatest` (current Windows 11 builds; other builds use `UserChoice`). The choice is signed (`Hash`): only Windows' chooser and Settings can write it.

The manager registers as an app for `claude://`: the ProgId `ClaudeDesktopProfilesManager.Url`, which runs `"<ClaudeDesktopProfilesManager.exe>" --url "%1"`, its capabilities, a `RegisteredApplications` value and an `ApplicationAssociationToasts` value of 0, which keeps Windows from announcing the new app. **Repair** writes that registration again; then, and after a first install, while Claude Desktop is installed and links do not open in the manager, the manager says what to pick, and on **Continue** opens an empty `claude:` link so that Windows shows its chooser (the router ignores that link). Windows shows the chooser only when no app is picked, so it first forgets another app's choice.

`HKCU\Software\Classes\claude` belongs to Claude, which rewrites it at every start: the manager leaves it alone.

### Routing a link

1. Spaces around the link are trimmed; it must start with `claude:` followed by something. Quotes, backslashes, spaces and control characters inside it are percent-encoded so it stays one argument; a link holding another kind of space (no-break, ideographic, any other Unicode space) is refused.
2. **One profile**: the link goes to it.
3. **Several profiles**: **Open a Claude link** lists them all, each open one said so, with the link shown without its query or fragment (they can hold a sign-in code). **Open** (or Enter, or a double click) gives the link to the profile chosen, which starts with it when it is closed; **Cancel** drops the link.
4. **Selected at first**: for a **sign-in link** (`login`, `auth`, `magic-link`, `sso` or `callback` in the path, not in the query or fragment), the window that opened the browser. That window logs `[Auth] Using system browser for: /login/...` in its `main.log`; the window with the latest such line from the last 15 minutes is selected, and the dialog names it. A window that did not start the sign-in ignores its link. For any other link, or a sign-in no window claims: the Claude window used last, else the default profile's window, else the first open one; with no Claude window open, the default profile.

A link reaches a running window through a second activation with the same `--user-data-dir`: Chromium's single-instance lock hands the link over and the new process exits.

## Files and registry

| What | Where |
|---|---|
| Program | `%LOCALAPPDATA%\Programs\Claude Desktop Profiles Manager\ClaudeDesktopProfilesManager.exe` |
| Start menu folder | `%APPDATA%\Microsoft\Windows\Start Menu\Programs\Claude Desktop Profiles Manager` (the manager, and the profiles added to the Start menu) |
| Settings | `HKCU\Software\Claude Desktop Profiles Manager`: `Profiles\<folder>` (name, color, badge text, picture stamp, `SyncSessions`: the group whose sessions it keeps the same), `DefaultProfile`, `Language`, `InstallPath`, `Shortcuts` (the profile shortcuts made, which follow renames), `Update` (the last release check), `Capabilities` (the `claude://` link handler as Windows lists it) |
| Profile icons and pictures, log, session changes and sessions waiting for a profile to close, backups of what sending sessions replaced, the session vault (`vault`) | `%LOCALAPPDATA%\Claude Desktop Profiles Manager` |
| A downloaded update, until it has installed itself | `%TEMP%\update-ClaudeDesktopProfilesManager.exe` |
| Taskbar pins | Windows' own pin list |
| Apps & features entry | `HKCU\Software\Microsoft\Windows\CurrentVersion\Uninstall\ClaudeDesktopProfilesManager` |
| Link handler | `HKCU\Software\Classes\ClaudeDesktopProfilesManager.Url`, value `Claude Desktop Profiles Manager` in `HKCU\Software\RegisteredApplications`, value `ClaudeDesktopProfilesManager.Url_claude` in `HKCU\Software\Microsoft\Windows\CurrentVersion\ApplicationAssociationToasts` |
| Default app for `claude://` | picked by the user in Windows: `...\UrlAssociations\claude\UserChoiceLatest` |

Uninstalling removes all of it but the session vault (Main, which it never touches, and the profiles kept go on with it), asks first when a kept profile is open with session changes still waiting for it, stops (and says so) when the watchers cannot be stopped, forgets the default-app choice when it is the manager (links then go back to Claude), gives open windows their Claude button back, and never touches `%APPDATA%\Claude`.

## Command line

| Option | Effect |
|---|---|
| none | opens the manager; a copy outside the install folder installs itself first |
| `--launch "<folder>"` | opens a profile (what shortcuts run) |
| `--url "<link>"` | routes a link |
| `--watch "<folder>" [<pid>]` | runs a profile's watcher, which also makes the session changes waiting for the profile when its Claude closes, keeps its list of sessions (made the same in its group) and opens the profile again after a Claude update (`<pid>`: the Claude process just started for it) |
| `--set-up-links` | opens the manager and offers to set up links |
| `--install [--quiet]` | installs this copy, then opens the manager (not with `--quiet`) |
| `--uninstall` | opens the uninstall dialog |

Any other option, or `--launch` and `--watch` without their folder, does nothing; a first argument that is not an option opens the manager.

## Limitations

- **The regular Claude icon opens Main**, with Claude's own button until the manager opens it or sends it a link: the Start menu and taskbar start Claude without arguments. The default profile only decides where `claude://` links go while Claude is closed.
- **After a Claude update** Windows opens Main again, even when only another profile was open. That Main keeps Claude's button unless it was open with its own before the update.
- **Sessions kept the same** reach a profile that is open only once it closes, and a session run over SSH, in WSL or in the cloud is not kept the same. The files of a session without a folder are copied to the other profiles' areas when it is sent; what changes in them later is copied again only for files missing or older there.
- **Shared Claude Code data**: `%USERPROFILE%\.claude` (or `CLAUDE_CONFIG_DIR`: Claude Code settings and memory) is the same for every profile. A `CLAUDE_CONFIG_DIR` set in a profile's own Claude settings is not followed.
- **Cowork** runs in one profile at a time: its VM is shared by the whole PC.
- **One sign-in at a time**: the newest `[Auth]` line is the one selected. A second sign-in started in another window before the first one finishes gets the first one's link selected for it: pick the first window in the dialog, or start that sign-in again.
- **Running profiles** cannot be deleted: quit them from the notification area first.
- **Deleted profiles**: a shortcut or pin to one offers to open the manager instead of starting Claude.
- **Linked folders**: deleting a profile whose folder is linked elsewhere removes the link and Claude's local files for it (`%LOCALAPPDATA%\<folder>`, `<folder>-Data`), not the folder it leads to.
- **Limits**: 32 profiles; `3p`, `Data`, `...-3p` and `...-Data` are refused as new profiles' names (they name the folder); a display name can be any text of 48 characters or fewer, without control characters.
- **Uninstall** keeps the profiles you choose in `%APPDATA%`, and a new install finds them again with default names and colors. A kept profile that was never opened and holds only the settings copied into it is removed.
- **Claude not installed**: only the MSIX packages are detected; the manager then shows **Get Claude**.

## Checking Claude's behavior

`build.cmd` runs `tests/test_claude.c`, which looks up in the installed Claude Desktop (`app.asar`, `Claude.exe`, its notification-area icons) and in the Claude Code it installed each fact this document relies on: the `claude://resume` link and the entry it makes, the quit, sign-in and virtualization log lines, where entries live and their fields, what Claude's own delete removes and the marks it leaves, the "no folder" area and its folder names, the window classes, how Claude Code finds transcripts and names project folders, and its running-session records. While a profile runs, it also checks the `lockfile` the watcher relies on. A Claude update that changes one of these facts makes the build fail with that fact's name; the checks read the installed files, they do not reproduce a clean installation.
