# Claude Desktop Profiles Manager

Run several Claude Desktop accounts side by side on Windows, each with **its own sign-in, history and settings**.

Unofficial tool, not affiliated with Anthropic.

## 关于这个版本 / About this version

这是 [leiyitong](https://github.com/leiyitong) 公开分享的版本。程序最初由 [Freenitial](https://github.com/Freenitial)（Léo Gillet）编写，之后由 Cheese（[JustLikeCheese](https://github.com/JustLikeCheese)）在一个不公开的仓库里继续开发。这个仓库发布 Cheese 的最新版本，方便公开分享，并且只从这个仓库检查更新。

leiyitong 提交、已经被 Cheese 采纳的改进：几个账号保持同样的对话（不用链接文件夹）、会话保险库、恢复删除的对话、每周自动备份 `.claude`、保险库自动清理、帮助，以及修复"对话太长换新记录文件后被同步删除"的问题。

This is the version [leiyitong](https://github.com/leiyitong) shares publicly: Cheese's latest version of the program written by [Freenitial](https://github.com/Freenitial) (Léo Gillet), which Cheese ([JustLikeCheese](https://github.com/JustLikeCheese)) develops in a private repository. It checks for updates in this repository only.

### 怎么安装 / How to install

1. 先安装 [Claude Desktop](https://claude.ai/download)（Windows 10 1809 或更新的 64 位系统）
2. 打开本仓库的 **[Releases](../../releases/latest)**，下载 **`ClaudeDesktopProfilesManager.exe`**（不要下载 "Source code"，那是源代码）
3. 双击运行。它不需要安装包，也不需要管理员权限：会自己装到 `%LOCALAPPDATA%\Programs\Claude Desktop Profiles Manager`，在开始菜单加上快捷方式，然后打开。下载的那个文件之后可以删掉
4. Windows 问"用哪个应用打开 claude:// 链接"时，选 **Claude Desktop Profiles Manager**，再选**始终**

这个版本没有数字签名，所以第一次运行时 Windows 可能显示"Windows 已保护你的电脑"：点**更多信息** → **仍要运行**。有新版本时，管理器会提示你，并打开下载页面，按上面的步骤再装一次即可。

Download `ClaudeDesktopProfilesManager.exe` from [Releases](../../releases/latest) and double-click it: it installs itself for the current user. It is not signed, so Windows SmartScreen may ask first: **More info** → **Run anyway**.

![Claude Desktop Profiles Manager window in dark theme, listing the Personal and Work profiles](docs/screenshot.png)

## Features

- **Profile manager** - Lists existing profiles; creates, renames, recolors and deletes them (to the Recycle Bin)
- **Link chooser** - With several profiles, Claude Desktop Profiles Manager asks which one opens a `claude://` link; for a browser sign-in, the window that started it is already selected
- **Notifications** - A Claude notification clicked in Windows' notification center opens the profile that showed it, not the default one: with several profiles open, you pick which
- **Profile badges** - Each profile's shortcut and taskbar button show the Claude icon with its own colored badge, and its notification-area icon takes its color (or its picture, below). Twenty colors, the ones no profile uses listed first and the others with the profiles that use them; a badge shows the name's initial or one or two characters of your own; or a picture of your own (PNG, JPEG, GIF, BMP, TIFF, ICO, and WebP or HEIF with their Windows extension) replaces the icon
- **Toolbar and column** - A toolbar with colored icons: **Open**, **Quit**, **Restart**, **New…**, **Edit…**, **Delete…**, **Set as default**, and at its right end two icons that open a menu the program draws itself: **Shortcuts** (desktop shortcuts, shortcuts saved elsewhere, taskbar pins and Start menu entries for every profile selected) and **Language**. Beside the list: **Sessions view**, **Sync settings…**, **Backup & Restore…** and **Repair**. Everything that changes data asks first, in a sentence or two
- **Open at sign-in** - Any profile can open when you sign in to Windows, not only the one the regular Claude icon opens
- **Claude Code sessions** - The manager shows each profile's Code sessions by folder and where each one is listed; open a session in any profile, share it with another one or copy it there, preview its conversation (Space), edit its title and star (**Edit…**, F2), star it or remove it per profile; archived sessions, with **Show archived**, show apart under **Archived** at the bottom, by folder. Ctrl+click and Shift+click select several sessions (a folder's or Starred's row takes all its sessions), and a folder's or Starred's menu acts on all it shows: share, copy, export, star or remove them together. Ctrl+C and Ctrl+V copy sessions to the profile picked; folders start folded. Changes to a profile that is open are made when it closes
- **Session sync** - In the list's right-click menu: **Merge all sessions…** gives every profile the sessions the others list; **Copy all sessions to…** lists every session of the profiles selected in others too, **Move all sessions to…** then takes them out of the profiles selected; sessions can be exported to a .zip archive and imported into other profiles. What a change replaces or removes is kept in a backup
- **Open, Quit, Restart** - **Open** makes the sessions of the profiles selected the same first, **Quit** closes their Claude (as its own Quit in the notification area does) and then makes their sessions the same, **Restart** does both. The progress shows at the foot of the window's right column, above the version
- **Status** - The list shows which profiles are running, and follows them as they start and close
- **Right-click menu** - Right-click a profile for the toolbar's actions, **Sync now**, **Sync settings…**, merging, copying or moving every session, and backups, without selecting it first. Several profiles selected: every action that can takes them all
- **Profiles synced** - A profile syncs with others (**Sync settings…**, or **Sync** in its **Edit…** or **New…** dialog), which also chooses what they sync: its sessions (new ones, titles, stars, archived and deleted ones); the sidebar (pins, groups and their sections, project order, filters, **Edit sidebar** choices); each session's model, effort, side pane, unread mark and cost; appearance (fonts, the editor's settings, zoom, spelling); the interface language; the default model; Claude's settings (auto-archive, Cowork, Remote Control, Cowork's recent folders); and, only when chosen, folders' permission modes and Cowork's trusted folders. Each time one of them closes or opens through the manager, or on **Sync now**, what changed in one goes to the others; what they changed each their own way is merged item by item (a group, a session's group, a pin, a setting), and only an item two of them changed differently waits, each keeping its own, until you choose which version to keep of it (**Sync conflicts**, the latest chosen first). One sync runs at a time. A session two of them went on with apart is kept as two. A session without a folder works in each profile's own "no folder" area, so it shows there as Claude's own. Several such groups can coexist; the column on the right names the profiles each one syncs with. An open profile gets the others' changes once it closes: **Restart** gives them now
- **Repair** - After moving from an earlier version: registers the program again (for claude:// links too, letting Windows ask which app opens them when it is not this one), puts pins, shortcuts and icons right, gives a profile whose session folder is still a link one of its own, drops changes waiting for profiles that are gone, then keeps every profile's sessions
- **Session vault** - Every list of Code sessions is kept, all its versions, out of Claude's reach: **Recover sessions…** puts a version back into a profile, for example after reinstalling Claude, which takes Main's list away. A profile kept the same as others gets its list back by itself. Sessions deleted in Claude stay deleted
- **Clean up sessions** - Deleting a session in Claude leaves its conversation on disk. **Clean up sessions…** (in **Backup & Restore…**) lists the conversations no profile lists any more, by profile: **Restore** puts the ones you check back where they were, **Delete** moves them to the Recycle Bin, after a copy of `.claude` if you like. **Back up once a week** copies `.claude` beside itself each week, the two latest copies kept
- **Settings** - The gear at the top right: how many days the session vault keeps each list's versions (0 keeps them all), and a few answers to what people ask first
- **Backup & Restore** - One place for a profile's backup and its restore, exporting, importing, recovering and cleaning up sessions, and **Back up .claude…**, which copies Claude Code's folder (`%USERPROFILE%\.claude`, where every profile's conversations are) beside it, as `.claude_<yyyymmdd>`
- **Backups** - Right-click a profile, **Back up…** saves the parts you check (Code sessions, Cowork sessions, settings, sign-in) to one .zip, one per profile when several are selected; **Restore from backup…** puts them into the same or another profile
- **English and Simplified Chinese** - The manager follows your Windows language, or the one you pick with the **Language** icon
- **Settings copy** - A new profile can start with the MCP servers, notification-area icon, hardware acceleration setting, language and theme of another one
- **Updates** - Tells you when a new version is out and opens its download page (this version's releases are not signed, so it does not install them by itself)
- **Claude unchanged** - Starts the installed app like the Start menu does, so Claude keeps updating itself
- **Lightweight** - One native exe, no runtime, no admin rights

## Requirements

- Windows 10 1809+ (64-bit)
- [Claude Desktop](https://claude.ai/download), from claude.ai or the Microsoft Store

## Installation

**[Download ClaudeDesktopProfilesManager.exe](../../releases/latest/download/ClaudeDesktopProfilesManager.exe)**, then double-click it.

It installs itself for the current user in `%LOCALAPPDATA%\Programs\Claude Desktop Profiles Manager`, adds a Claude Desktop Profiles Manager folder to the Start menu and opens; the downloaded file can then be deleted. Claude Desktop Profiles Manager then asks Windows once which app opens claude:// links: choose **Claude Desktop Profiles Manager**, then **Always**. To update, download and run the new version the same way.

To uninstall, use Settings → Apps: it asks which profiles to keep and sends the others to the Recycle Bin. Claude Desktop and its own data are never touched.

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

- [Freenitial](https://github.com/Freenitial) (Léo Gillet) - the original Claude Desktop Profiles Manager
- Cheese ([JustLikeCheese](https://github.com/JustLikeCheese)) - its further development
- [ai-multi-instance](https://github.com/Zoltak-Dev/ai-multi-instance) - the `--user-data-dir` technique for Claude Desktop
- [claude-desktop-clone](https://github.com/vodongha/claude-desktop-clone) and [claude-windows-multiprofile](https://github.com/fredless/claude-windows-multiprofile) - earlier PowerShell tools for the same job

---

## License

Partially open source - enterprise or commercial usage of the Taskbar Module requires a paid license. See [LICENSE](LICENSE) for details.

The commercial license includes:

- Reverse engineering documentation (Markdown, 3k lines) covering how the taskbar-related DLL files work (blob format, COM vtables, notification chains, WFC gating, PIDL structures, etc.)
- Support within reasonable limits

Contact: [freenitial@gmail.com](mailto:freenitial@gmail.com)
