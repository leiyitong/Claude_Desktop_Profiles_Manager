# Claude Desktop Profiles Manager

Run several Claude Desktop accounts side by side on Windows, each with **its own sign-in, history and settings**.

Unofficial tool, not affiliated with Anthropic.

## 关于这个版本 / About this version

这是 [leiyitong](https://github.com/leiyitong) 公开分享的版本。程序最初由 [Freenitial](https://github.com/Freenitial)（Léo Gillet）编写，之后由 Cheese（[JustLikeCheese](https://github.com/JustLikeCheese)）在一个不公开的仓库里继续开发。这个仓库跟随 Cheese 的最新版本，并在它的基础上做了下面这些改进：

- **帮助**：顶部菜单栏多了"帮助"，用大白话回答新手常问的问题：对话存在哪里、几个账号怎么保持同样的对话、什么在保护对话、重装 Claude 前要做什么。没有快捷键，不会和其他软件冲突
- **最近删除**：在 Claude 里删掉的对话，文件其实还在电脑上。"会话"菜单 → **最近删除…** 列出它们，可以**恢复**到原来的账号，也可以**删除**（放进回收站）
- **每周自动备份 `.claude`**：一周内没有备份过时，在 Claude 关闭后自动复制一份 `%USERPROFILE%\.claude`，名叫 `.claude_auto_<日期>`，只保留最新 2 份，更旧的自动删除
- **保险库自动清理**：保险库（`%LOCALAPPDATA%\Claude Desktop Profiles Manager\vault`）只保留最近 30 天的版本（至少最新 20 个），不再无限增长
- **再次询问"保持一致"**：如果因为某个 Claude 正开着而没能把账号设成"保持一致"，管理器会提议帮你退出它；等它关闭后会再问一次
- **更新来源**：管理器从这个仓库检查新版本，所以原版的发布不会覆盖这个版本

更早的改进已经被 Cheese 合并进他的版本：几个账号不再共用一个链接的会话文件夹（Claude 会拒绝写入链接文件夹，导致新对话丢失），而是各自保存，再由保险库把变化同步过去。

This is the version [leiyitong](https://github.com/leiyitong) shares publicly. It was written by [Freenitial](https://github.com/Freenitial) (Léo Gillet) and developed further by Cheese ([JustLikeCheese](https://github.com/JustLikeCheese)) in a private repository. This repository follows Cheese's latest version and adds: a **Help** menu, **Recently deleted** with **Restore**, a weekly copy of `.claude` (two kept), a vault limited to 30 days (at least 20 versions), the "Keep the same" question asked again once the Claude in the way closes, and updates from this repository.

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
- **Profile badges** - Each profile's shortcut and taskbar button show the Claude icon with its own colored badge, and its notification-area icon takes its color (or its picture, below). Twenty colors, the ones no profile uses listed first and the others with the profiles that use them; a badge shows the name's initial or one or two characters of your own; or a picture of your own (PNG, JPEG, GIF, BMP, TIFF, ICO, and WebP or HEIF with their Windows extension) replaces the icon
- **Menu bar and toolbar** - The window's **Sessions**, **Program** and **Shortcuts** menus (desktop shortcuts, shortcuts saved elsewhere, taskbar pins and Start menu entries for every profile selected); under them a toolbar with colored icons: **Open**, **Quit**, **Restart**, **New…**, **Edit…**, **Delete…**, **Set as default**. Beside the list: **Sessions view**, **Sync sessions**, **Repair** and **Fix claude:// links**
- **Open at sign-in** - Any profile can open when you sign in to Windows, not only the one the regular Claude icon opens
- **Claude Code sessions** - The manager shows each profile's Code sessions by folder and where each one is listed; open a session in any profile, share it with another one or copy it there, rename it, star it or remove it per profile, or delete it everywhere. Ctrl+click and Shift+click select several sessions (a folder's or Starred's row takes all its sessions), and a folder's or Starred's menu acts on all it shows: share, copy, export, star or remove them together. Changes to a profile that is open are made when it closes
- **Session sync** - In the **Sessions** menu: **Merge all sessions…** gives every profile the sessions the others list; **Copy all sessions to…** lists every session of the profiles selected in others too, **Move all sessions to…** then takes them out of the profiles selected; sessions can be exported to a .zip archive and imported into other profiles. What a change replaces or removes is kept in a backup
- **Open, Quit, Restart** - **Open** makes the sessions of the profiles selected the same first, **Quit** closes their Claude (as its own Quit in the notification area does) and then makes their sessions the same, **Restart** does both. The progress shows at the foot of the window's right column, above the version
- **Right-click menu** - Right-click a profile for the toolbar's actions, **Sync sessions**, keeping its sessions the same as another one's, and backups, without selecting it first. Several profiles selected: every action that can takes them all
- **Sessions kept the same** - Profiles form groups (**Sessions** in a profile's **Edit…** or **New…** dialog, or **Keep sessions the same as** in the Sessions menu): each time one of them closes or opens through the manager, or on **Sync sessions**, what changed in one goes to the others: new sessions, titles, stars, the pins and groups of Claude's sidebar, archived and deleted ones. A session two of them went on with apart is kept as two. A session without a folder works in each profile's own "no folder" area, so it shows there as Claude's own. Several groups can coexist; the column on the right names each profile's group. An open profile gets the others' changes once it closes: **Restart** gives them now
- **Repair** - After moving from an earlier version: registers the program again, puts pins, shortcuts and icons right, gives a profile whose session folder is still a link one of its own, drops changes waiting for profiles that are gone, then keeps every profile's sessions
- **Session vault** - Every list of Code sessions is kept out of Claude's reach (`%LOCALAPPDATA%\Claude Desktop Profiles Manager\vault`), with its versions of the last 30 days, at least the 20 latest: **Recover sessions…** puts a version back into a profile, for example after reinstalling Claude, which takes Main's list away. A profile kept the same as others gets its list back by itself. Sessions deleted in Claude stay deleted, unless you restore them
- **Recently deleted** - Deleting a session in Claude leaves its conversation on disk. **Recently deleted…** lists the conversations no profile lists any more: **Restore** puts the ones you check back in the profiles that had them, **Delete** moves them to the Recycle Bin, after a copy of `.claude` if you like
- **Back up .claude** - **Back up .claude…** copies Claude Code's folder (`%USERPROFILE%\.claude`, where every profile's conversations are) beside it, as `.claude_<yyyymmdd>`. Once a week, after a Claude closes and none runs, a copy is made by itself as `.claude_auto_<yyyymmdd>`; only the two latest stay
- **Help** - **Help**, in the menu bar, answers in plain words what someone new asks: where conversations are saved, how profiles keep the same ones, what protects them, what to do before reinstalling Claude
- **Backups** - Right-click a profile, **Back up…** saves the parts you check (Code sessions, Cowork sessions, settings, sign-in) to one .zip, one per profile when several are selected; **Restore from backup…** puts them into the same or another profile
- **English and Simplified Chinese** - The manager follows your Windows language, or the one you pick in **Program** → **Language**
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
| The browser sign-in finishes, but the window stays signed out | Sign in again from that window, and open the link in the profile the dialog names. If it still fails, click **Fix claude:// links** in Claude Desktop Profiles Manager |
| Something is off after an update of Claude Desktop Profiles Manager | Click **Repair** |
| Windows asks which app opens a Claude link | Choose **Claude Desktop Profiles Manager**, then **Always** |
| A shortcut says its profile no longer exists | The profile was deleted: delete or unpin the shortcut |
| Something else | Read the log: `%LOCALAPPDATA%\Claude Desktop Profiles Manager\claude-desktop-profiles-manager.log` |

## Build from source

See [BUILD.md](BUILD.md).

## Credits

- [Freenitial](https://github.com/Freenitial) (Léo Gillet) - the original Claude Desktop Profiles Manager
- Cheese ([JustLikeCheese](https://github.com/JustLikeCheese)) - its further development: session groups, the sidebar kept the same, the menus
- [ai-multi-instance](https://github.com/Zoltak-Dev/ai-multi-instance) - the `--user-data-dir` technique for Claude Desktop
- [claude-desktop-clone](https://github.com/vodongha/claude-desktop-clone) and [claude-windows-multiprofile](https://github.com/fredless/claude-windows-multiprofile) - earlier PowerShell tools for the same job

---

## License

Partially open source - enterprise or commercial usage of the Taskbar Module requires a paid license. See [LICENSE](LICENSE) for details.

The commercial license includes:

- Reverse engineering documentation (Markdown, 3k lines) covering how the taskbar-related DLL files work (blob format, COM vtables, notification chains, WFC gating, PIDL structures, etc.)
- Support within reasonable limits

Contact: [freenitial@gmail.com](mailto:freenitial@gmail.com)
