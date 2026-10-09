# Claude Desktop Profiles Manager

Run several Claude Desktop accounts side by side on Windows, each with **its own sign-in, history and settings**.

Unofficial tool, not affiliated with Anthropic.

![Claude Desktop Profiles Manager window in dark theme, listing the Personal and Work profiles](docs/screenshot.png)

## Features

- **Profile manager** - Lists existing profiles; creates, renames, recolors and deletes them (to the Recycle Bin)
- **Link chooser** - With several profiles, Claude Desktop Profiles Manager asks which one opens a `claude://` link; for a browser sign-in, the window that started it is already selected
- **Profile badges** - Each profile's shortcut and taskbar button show the Claude icon with its own colored badge, and its notification-area icon takes its color (or its picture, below). Twenty colors, the ones no profile uses listed first and the others with the profiles that use them; a badge shows the name's initial or one or two characters of your own; or a picture of your own (PNG, JPEG, GIF, BMP, TIFF, ICO, and WebP or HEIF with their Windows extension) replaces the icon
- **Menu bar and toolbar** - The window's **Sessions**, **Program** and **Shortcuts** menus (desktop shortcuts, shortcuts saved elsewhere, taskbar pins and Start menu entries for every profile selected); under them a toolbar with colored icons: **Open**, **Quit**, **Restart**, **New…**, **Edit…**, **Delete…**, **Set as default**. Beside the list: **Sessions view**, **Sync sessions**, **Repair** and **Back up .claude…**. Everything that changes data asks first, naming the folders it changes and where it backs up what it replaces
- **Open at sign-in** - Any profile can open when you sign in to Windows, not only the one the regular Claude icon opens
- **Claude Code sessions** - The manager shows each profile's Code sessions by folder and where each one is listed; open a session in any profile, share it with another one or copy it there, rename it, star it or remove it per profile, or delete it everywhere. Ctrl+click and Shift+click select several sessions (a folder's or Starred's row takes all its sessions), and a folder's or Starred's menu acts on all it shows: share, copy, export, star or remove them together. Changes to a profile that is open are made when it closes
- **Session sync** - In the **Sessions** menu: **Merge all sessions…** gives every profile the sessions the others list; **Copy all sessions to…** lists every session of the profiles selected in others too, **Move all sessions to…** then takes them out of the profiles selected; sessions can be exported to a .zip archive and imported into other profiles. What a change replaces or removes is kept in a backup
- **Open, Quit, Restart** - **Open** makes the sessions of the profiles selected the same first, **Quit** closes their Claude (as its own Quit in the notification area does) and then makes their sessions the same, **Restart** does both. The progress shows at the foot of the window's right column, above the version
- **Right-click menu** - Right-click a profile for the toolbar's actions, **Sync sessions**, keeping its sessions the same as another one's, and backups, without selecting it first. Several profiles selected: every action that can takes them all
- **Sessions kept the same** - Profiles form groups (**Sessions** in a profile's **Edit…** or **New…** dialog, or **Keep sessions the same as** in the Sessions menu): each time one of them closes or opens through the manager, or on **Sync sessions**, what changed in one goes to the others: new sessions, titles, stars, archived and deleted ones, and Claude's sidebar: its pins, groups and their sections, project order, filters, **Edit sidebar** choices, and the editor's settings. A session two of them went on with apart is kept as two. A session without a folder works in each profile's own "no folder" area, so it shows there as Claude's own. Several groups can coexist; the column on the right names each profile's group. An open profile gets the others' changes once it closes: **Restart** gives them now
- **Repair** - After moving from an earlier version: registers the program again (for claude:// links too, letting Windows ask which app opens them when it is not this one), puts pins, shortcuts and icons right, gives a profile whose session folder is still a link one of its own, drops changes waiting for profiles that are gone, then keeps every profile's sessions
- **Session vault** - Every list of Code sessions is kept, all its versions, out of Claude's reach: **Recover sessions…** puts a version back into a profile, for example after reinstalling Claude, which takes Main's list away. A profile kept the same as others gets its list back by itself. Sessions deleted in Claude stay deleted
- **Clean up deleted sessions** - Deleting a session in Claude leaves its conversation on disk, where a restore or Claude's own import could bring it back. **Clean up deleted sessions…** lists the conversations no profile lists any more and moves the ones you check to the Recycle Bin, after a copy of `.claude` if you like
- **Back up .claude** - **Back up .claude…** copies Claude Code's folder (`%USERPROFILE%\.claude`, where every profile's conversations are) beside it, as `.claude_<yyyymmdd>`
- **Backups** - Right-click a profile, **Back up…** saves the parts you check (Code sessions, Cowork sessions, settings, sign-in) to one .zip, one per profile when several are selected; **Restore from backup…** puts them into the same or another profile
- **English and Simplified Chinese** - The manager follows your Windows language, or the one you pick in **Program** → **Language**
- **Settings copy** - A new profile can start with the MCP servers, notification-area icon, hardware acceleration setting, language and theme of another one
- **Updates** - Tells you when a new version is out and installs it in one click, once Windows has checked its signature
- **Claude unchanged** - Starts the installed app like the Start menu does, so Claude keeps updating itself
- **Lightweight** - One native exe, no runtime, no admin rights

## Requirements

- Windows 10 1809+ (64-bit)
- [Claude Desktop](https://claude.ai/download), from claude.ai or the Microsoft Store

## Installation

**[Download ClaudeDesktopProfilesManager.exe](../../releases/latest/download/ClaudeDesktopProfilesManager.exe)**, then double-click it.

It installs itself for the current user in `%LOCALAPPDATA%\Programs\Claude Desktop Profiles Manager`, adds a Claude Desktop Profiles Manager folder to the Start menu and opens; the downloaded file can then be deleted. Claude Desktop Profiles Manager then asks Windows once which app opens claude:// links: choose **Claude Desktop Profiles Manager**, then **Always**. To update, download and run the new version the same way.

To uninstall, choose **Program** → **Uninstall…** or use Settings → Apps: it asks which profiles to keep and sends the others to the Recycle Bin. Claude Desktop and its own data are never touched.

## Usage

1. Click **New…**, enter a name, pick a color (or a badge text, or a picture), choose whose sessions it keeps the same, and click **OK**: the profile opens in its own window
2. Sign in to Claude in that window
3. In the **Shortcuts** menu, choose **Create shortcut on desktop** or **Pin to taskbar**, then open the profile from there
4. *(Optional)* Select a profile and click **Set as default**: it is selected for `claude://` links while Claude is closed

How it works: see [docs/HOW-IT-WORKS.md](docs/HOW-IT-WORKS.md).

## Notes and limitations

- The regular Claude icon always opens Main, with Claude's plain taskbar button until Claude Desktop Profiles Manager opens it or sends it a link. Main is Claude's own `%APPDATA%\Claude`; the others live in `%APPDATA%\Claude-<name>` and open from their shortcuts
- Cowork runs in one profile at a time: its virtual machine is shared by the whole PC
- Claude Desktop settings (theme, language, notification-area icon, MCP servers, extensions) belong to each profile; Claude Code settings and memory (`%USERPROFILE%\.claude`) are shared by every profile
- When its window opens, Claude Desktop Profiles Manager asks GitHub for the latest release, at most every 4 hours
- A profile shows its icon in the notification area while Claude's own **System tray** setting is on for that profile
- When Claude updates itself, it closes every open profile: they open again once the update is installed, with their badge and colors (Windows also reopens Main, even if it was closed)
- Tested with Claude Desktop 2.16120.0. Sign-in routing relies on undocumented behavior that a Claude update can change

## Troubleshooting

| What you see | What to do |
|---|---|
| The browser sign-in finishes, but the window stays signed out | Sign in again from that window, and open the link in the profile the dialog names. If it still fails, click **Repair** in Claude Desktop Profiles Manager |
| Something is off after an update of Claude Desktop Profiles Manager | Click **Repair** |
| Windows asks which app opens a Claude link | Choose **Claude Desktop Profiles Manager**, then **Always** |
| A shortcut says its profile no longer exists | The profile was deleted: delete or unpin the shortcut |
| Something else | Read the log: `%LOCALAPPDATA%\Claude Desktop Profiles Manager\claude-desktop-profiles-manager.log` |

## Build from source

See [BUILD.md](BUILD.md).

## Credits

- [ai-multi-instance](https://github.com/Zoltak-Dev/ai-multi-instance) - the `--user-data-dir` technique for Claude Desktop
- [claude-desktop-clone](https://github.com/vodongha/claude-desktop-clone) and [claude-windows-multiprofile](https://github.com/fredless/claude-windows-multiprofile) - earlier PowerShell tools for the same job

---

## License

Partially open source - enterprise or commercial usage of the Taskbar Module requires a paid license. See [LICENSE](LICENSE) for details.

The commercial license includes:

- Reverse engineering documentation (Markdown, 3k lines) covering how the taskbar-related DLL files work (blob format, COM vtables, notification chains, WFC gating, PIDL structures, etc.)
- Support within reasonable limits

Contact: [freenitial@gmail.com](mailto:freenitial@gmail.com)
