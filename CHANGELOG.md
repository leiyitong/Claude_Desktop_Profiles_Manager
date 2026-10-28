# Changelog

[简体中文](CHANGELOG.zh-CN.md)

> Run several Claude Desktop accounts side by side on Windows, each with its own sign-in, history and settings.

Every version of Claude Desktop Profiles Manager, newest first. A day's updates make one version, named after that day, which is the version the program shows; **About**, the icon right of Settings, shows this list too: a version chosen on the left, or **Changelog** for all of it. Each build of `main` is also published as an unsigned pre-release (`build-<number>`), which an installed copy does not update to.

## v2026.10.28

Starred sessions stay the same in synced profiles, now that Claude keeps them in its own database and on its server; private sign-in can skip Claude's onboarding; after a start the manager lists sessions faster, and it syncs only what changed.

### New features

1. **Skip Claude's onboarding after signing in** — a checkbox in Private sign-in, off at first and remembered: once the sign-in is done, the manager tells claude.ai the account's onboarding is finished (the same call the form's last step makes) and reads it back, so a profile signed in for the first time opens the chats, not the usage/plans/name form. It also frees an account the onboarding form is holding (its flag says unfinished, so every web login lands there)

### Improvements

1. A profile that lacks a part of Claude's window (a side pane it never opened) is no longer sent the layout at every sync, which changed nothing there
2. A sync is no longer made when nothing changed since the last one: restarting two profiles synced four times over, now once
3. The first session list after the manager starts takes a fraction of a second, not up to four: the sessions read before are kept on disk, and only those that changed are read again, also when a sync reads the same sessions at the same moment (it used to make both read them all)
4. An update that leaves Windows MCP's source as it was no longer rewrites it in every profile, nor makes uv build it again
5. A shorter log: a profile's sessions folder is noted once, not again by every process of the manager; a sync is one line per profile with its counts, not one per session; a change a newer one replaced is no longer noted as removed

### Bug fixes

1. **Starred sessions** stay the same in synced profiles: Claude 3.6556 keeps which sessions are starred in its own database (IndexedDB) and on its server, per account, and put each profile's own back at every start; the sync now writes them there and has Claude send them to its server
2. **The editor's settings** and **the projects chosen** in the sessions' filter stay the same too: Claude keeps them on its server as well, and put the server's back at its start
3. **Claude Delta in the Start menu** is no longer deleted at every start; the shortcut left by its earlier name, Claude Desktop Profiles Manager, is removed instead
4. **Windows MCP** starts again in a profile that was open during an update: its Claude wrote the folder it had started with back into its settings, a folder the update had removed. A running profile's entry is now written once it closes, and no older folder goes while one runs
5. The program keeps its folders under their earlier name, as the rename to Claude Delta meant to: it is installed in `%LOCALAPPDATA%\Programs\Claude Desktop Profiles Manager` and keeps its session vault, settings, waiting changes, backups and log in `%LOCALAPPDATA%\Claude Desktop Profiles Manager`, so an installed copy finds them again instead of starting over in a folder named Claude Delta

## v2026.10.27

Pinned sessions and the interface's size stay the same again in profiles kept the same, with Claude 3.6556.

### Bug fixes

1. **Pinned sessions** stay the same in synced profiles again: Claude now keeps its pins in a second place too and put a profile's earlier pins back each time it started, which the next sync then took for a change and sent to the others
2. **The interface's size** (Ctrl + and Ctrl -) stays the same in synced profiles again: a profile whose Claude settings came from another folder keeps that folder's size beside its own, and the sync read and wrote the wrong one, so the profile stayed at 100% and its size went to the others

## v2026.10.26

Claude can search the web through your own SearXNG instance, and the manager no longer stalls while Claude works: sessions are read again only where they changed. Settings are sorted by kind and hold Claude Code's and Claude's own switches, each saying how far it reaches and how much it changes.

### New features

1. **SearXNG search…** in the list's right-click menu sets up web search through your own SearXNG instance in the profiles selected: type its address (`https://` may be left out), and choose whether it may reach private network addresses (needed when the instance is on your own network, or when a proxy's fake IPs give its address a private range). It adds the `mcp-searxng` server, run by Node.js, and fetches it in the background so that Claude's first start does not wait for the download. The same menu item changes the address or removes it. Open profiles are closed for it and opened again, asked first. It needs Node.js
2. In a profile with SearXNG search, Windows MCP tells Claude where the instance is, and how to query it by hand without garbling Chinese: send the query in UTF-8 (from Python, `urllib.parse.urlencode` or `requests`' `params=`), and in Git Bash use `C:\Windows\System32\curl.exe` rather than Git Bash's own curl, which sends a Chinese query as GBK
3. **SearXNG search** has a button of its own in the main window's column, below **Windows MCP**: one click sets it up in the profiles selected with the address already used (it asks for one only the first time), and once they all have it the button reads **Remove SearXNG search**. A profile with it shows a globe at the end of its name, purple as Windows MCP's computer, its address in the tip
4. **Export…** and **Import…** have **Select all**, **Select none** and **Invert** below the list
5. **New versions offered**: when the manager opens it looks up the latest release of main with what this PC signs in to GitHub with (the GitHub CLI `gh`, or git's GitHub credential) and, when it was built from another commit, offers it in a window with its notes. **Update now** downloads it and installs it; **Later** leaves **Version … is available** beside the version, with **Update**, and it is offered again once another is out. Click the version (the pointer turns into a hand over it) to look now. Without `gh`, git finds it and **Update** opens its release page. A copy on a PC that signs in to GitHub with neither shows its version in red, and is offered nothing. Builds of main are published as one release a day, named after the day (Claude Delta v2026.10.26) and marked latest; a later push of the day replaces it
6. **Remove Cowork** in the main window's column: in the profiles selected it turns Cowork off in Claude's preferences (`secureVmFeaturesEnabled`) and deletes Cowork's virtual machine, some 9 GB in each profile that used it, asked first; open profiles are closed for it and opened again. Once none of them has it, the button reads **Restore Cowork** (Claude downloads the virtual machine again when Cowork is used). A profile's tip shows the size of its virtual machine, or that Cowork is off
7. **Settings** says what a new profile is made with: Windows MCP and SearXNG search set up (both on at first; they need uv and Node.js), SearXNG search's address (left empty: searxng.cheese.ink), and Cowork off, so that no virtual machine comes (on at first). The SearXNG search button sets up that address in the profiles selected that have none
8. **Settings** are sorted by kind: General, New profiles, Claude Code and Claude Desktop on the left, each kind's settings beside them, with a search. Every switch says how far it reaches (this program, the sign-in window, new profiles, Claude Code, all profiles) and how much it changes (minimal, small, medium or large), and below the list the setting selected says what it does. The language and the size of the windows stay above
9. Claude Code's own settings in **Settings**, written to its settings.json, which every profile's Code and the terminal read: how many days it keeps conversations (it deletes older ones when it starts, 30 by default), telemetry, error reporting, nonessential traffic, feedback surveys and automatic updates turned off, auto memory and the 1M-token context turned off, claude.ai connectors turned off, agent teams, each command starting in the project folder, cost warnings and spinner tips hidden, Claude left out of commits and pull requests, the built-in git instructions turned off, the permission mode new sessions start in, and every project MCP server approved. Only what you change is written, and a settings.json that cannot be read is left as it is
10. Claude's own preferences in **Settings**, set in every profile at once: Cowork web search and browser tools, Claude in Chrome, all browser actions allowed, Cowork and Code scheduled tasks, bypass permissions mode allowed, prompt suggestions, archive when the PR closes, keeping the computer awake while Claude works (on battery too) and the notification sound. Open profiles are closed for it and opened again, asked first; a switch on in some profiles only says in how many
11. **Settings** are laid out as Claude lays out its own: the kinds on the left under a search box, and on the right a row for each setting, with its name, what it does, how far it reaches and how much, and its switch, drop-down, field or button at the right end. The language and the size of the windows are settings of General; a search shows the settings that match from every kind
12. **Settings** read closer to Claude's: each kind on the left is a tall row with its icon, a setting's row is shorter, how far it reaches and how much on its name's line after its impact's icon, and each section's name has its icon again
13. **Settings** are plainer, as Claude's own: the kinds on the left in two groups, Preferences and Claude, with one-color icons, the kind open on a gray row; each setting says what it does, and "Large impact" beside its name only where it changes much; the switches are drawn as Claude draws them; Save keeps the changes and closes the window, and Apply is gone

### Improvements

1. The manager no longer stalls while Claude and Claude Code work. The sessions are read again only where something changed (a tenth of a second on a PC with 1,700 session entries, where every entry was read and parsed again, over a second each time), a burst of changes on disk is read once it is over, and a change the manager made itself is not read a second time. When nothing shown has changed, the window is not drawn again
2. Searching the conversations no longer reads every transcript again each time something changes on disk: a transcript searched once is searched again only once it changed, and one Claude Code is adding to only in its new lines; a transcript is read a part at a time, never whole. On a PC with 1.5 GB of transcripts, the search that follows each change took 3.8 s and now takes about 20 ms, and every transcript is searched, however large
3. A backup dropped on the main window where no profile is now makes a new profile from it, instead of asking you to drop it on one: the new profile's form opens with the backup's name, color, badge, note and window title (a number added when the name is taken), then the backup is restored into it, and of the profile's own only its picture is offered again. Dropped on a profile, a backup still goes into that profile; dropped elsewhere, it no longer goes into the profile selected
4. The profile list no longer shows a tip when the pointer rests on a row: the plan's usage, Windows-MCP, SearXNG search and Cowork lines are gone (the marks on the rows and the buttons in the column stay as they were); a name too long for its column still shows in full
5. A restore from a backup starts once you press **Restore** in its window: the **Restore preview** listing every file, and the question "Restore ... from ...?" after it, are gone. Into a profile just made from a backup, the window no longer says that what it replaces is kept in a backup first, and the message after the restore names no backup folder: nothing in it was replaced
6. A session archive is imported once you press **Import** in its window: the **Import preview** listing what each session would become in each profile is gone; the report after the import still gives each profile's result
7. **Remove Cowork** asks briefly: one profile by its name, several by their count, and how much the virtual machines deleted take; sizes from a gigabyte are shown in GB (18.6 GB, not 19083.2 MB)
8. A session archive dropped on the main window where no profile is opens its import, the profiles selected checked (every one signed in when none of them is), instead of making a new profile, which could keep no sessions before it signs in; backups dropped with it still make one
9. Message boxes are no wider than they need: each button as wide as its own caption, at the bottom right, the box widened only when its buttons need it; a long caption, in any language, widened the whole box and every button with it
10. The export and import window is smaller, in the golden ratio, and checks only the account and the profile's own at first; the note on keeping the sign-in file safe is gone, and the preview pane's title stands on its heading's row, as in Edit profile and Sync settings
11. The new version's window shows its notes in the language of the interface, without how to install it, whose icons showed as boxes
12. The shield checks again as soon as the proxy settings change (a proxy app turned on or off, another proxy set), not only when an address does; the regions box says what it takes, two-letter codes such as US, JP, and keeps nothing else

### Bug fixes

1. The manager no longer freezes when a profile closes while sessions are being synced: the changes waiting for a closed profile were made in the window's own thread, which then waited for the sync under way (the session vault's, minutes at most). They are made in the background now
2. The shield's window no longer says Claude's exit IP could not be read, with 0 ms latency and every check "no", before any check was made: opened, it reads the exit at once (with the shield off too, ending nothing) and says it is reading until the result comes, and it changes when a check ends instead of every second. A check asked for while the shield is off no longer turns the badge to "Shield on · checking the exit…"
3. The shield's badge at the main window's bottom right shows whole: the version's line, painted over it, cut off its lower half; it now stands at the end of that line
4. A profile whose shield regions leave out Claude's exit no longer stops every profile: it alone is closed, and stays closed (Open and Restart say why), and the badge turns amber, not red

## v2026.10.25

Windows MCP can sleep and come back: ask Claude "in 10 minutes, tell me..." and it waits, then answers. Its own tools no longer wait for you to stop using the mouse and keyboard, and an update now reaches the Windows MCP your profiles run.

### New features

1. Windows MCP has **Sleep**: asked "in 10 minutes, tell me the gold price" or "sleep 30 minutes, then...", Claude sleeps and then does it in the same reply, rather than saying it cannot wait. **Idle**'s description now says the same, so Claude reaches for either
2. **Uninstall the Windows-MCP extension…** in the list's right-click menu, wherever Claude's own Windows-MCP extension (Settings > Extensions) is installed: it is a second server with the same tools as Windows MCP. Setting up or repairing Windows MCP offers to uninstall it too. Its files are deleted as Claude's own Uninstall does, and an uninstall that could not delete them all (a file still in use) says so and can be run again. An open profile is closed for it and opened again, asked first
3. Windows MCP spends far fewer tokens on the desktop: **Click**, **Type**, **Scroll**, **Move**, **Shortcut**, **MultiSelect** and **MultiEdit** answer just "ok" (verify=true adds what changed on screen), and **Snapshot** lists only the elements to act on, one short row each (`12|button|Save|412,300|`), at most 150 rows, a window's rows under its title; window=... narrows it, diff=true returns only what changed since the previous Snapshot, and mode="tree" still gives the full tree, as deep as you ask. The row's number is the label Click and the others take
4. You can work while Claude works on your desktop: Windows MCP no longer waits for you to stop using the mouse and keyboard before it acts, and no longer holds them while it acts. **Esc**, pressed while Claude is acting, stops it: its clicks and typing are refused and it asks you how to go on; once it has left the desktop alone for 15 seconds, your next request works. Esc still reaches the app you are in
5. **Sessions can be dragged** in the sessions view. Drag a session (or several chosen together) onto another profile in the side bar to copy it there, or hold **Shift** to move it; drag it out to a folder or the desktop to export it as a session archive; and drag a session archive from Explorer onto a profile to import it there

### Improvements

1. Claude now gives Windows MCP's **PowerShell**, **Python**, **Node** and **Wsl** the folder to run in (`cwd`) rather than starting the command with `cd`: the parameter is a plain text field now, and the tool says to use it. Without it, a command runs where the last one left off, as before
2. **Settings** has a switch for the private sign-in window's DevTools port (CDP), off at first, under **Private sign-in**: it was a line in settings.ini only, and while it was set every private sign-in opened slower. On, the window serves DevTools on 127.0.0.1:9222 (or the port settings.ini's SignInDevToolsPort gives) while it is open

### Bug fixes

1. Windows MCP's tools no longer fail with USER_CONTROL while you are using the computer: Windows-MCP holds back tools that act on the desktop until the mouse and keyboard have rested 10 seconds, and counted every tool it did not know as one of them. A wait like "tell me in 30 minutes" now works while you keep working; so do Snapshot, Screenshot and Windows-MCP's other tools that only read
2. A profile's Windows MCP ran the copy set up with it, and an update never reached it; several builds of one day even shared one copy, the first's. Each build now unpacks its own, and the manager points every profile's Windows MCP at it as it opens, so an update takes effect at that Claude's next start, nothing to click. A profile whose Windows MCP is an older build's now shows **Repair Windows MCP**, which moves it to this build's (it takes effect when its Claude next starts); the older copy stays until no profile runs it
3. The private sign-in window has its own button on the taskbar and its place in Alt+Tab (it had neither: it was the manager's). Each page it goes to is logged by its host and path, never its query (codes and tokens are there), and so is the claude:// link it ends with and the profile it went to, so a sign-in that never ends can be read in the log. **SignInDevToolsPort** in settings.ini (off by default) makes it serve DevTools on that port of 127.0.0.1 while it is open, for debugging a sign-in: any program of yours can then drive the page
4. **Private sign-in now signs the profile in.** The sign-in window opens claude.ai, and once you have signed in, it reads the sign-in cookies and writes them into the profile, which then opens already signed in — no need for the sign-in to come from Claude Desktop itself. An open profile is closed for it and opened again. Its Claude must have been started once before (so it has a key to seal the cookies with)
5. The sessions view now keeps each profile’s place: scroll a profile’s sessions, switch to another profile, come back, and it is where you left it, showing as much as you had shown — it no longer jumps to the top and folds the extra sessions away
6. The shield's **Check now** works while the shield is off: it reads the exit IP, latency and leaks once and shows them, without turning the shield on; before, pressing it while off left the panel blank (0ms, “exit IP could not be read”)
7. A profile's Windows MCP no longer stops starting when uv moves: an entry whose uvx.exe is no longer where it was written (Python reinstalled elsewhere, uv moved) reads as outdated, and the manager points it at the uvx it finds as it opens

### Breaking changes

1. Windows MCP is now named **windows** in Claude (its tools are `mcp__windows__Click` and the like, no longer `mcp__windows-mcp__...`), so that it is not taken for Claude's own Windows-MCP extension. A server of your own named windows-mcp is left as it is

### Upgrade guide

1. Select the profiles that have Windows MCP and click **Repair Windows MCP** (the button shows it while one still runs the earlier copy or name): their Claude is closed and opened again with the new one

## v2026.10.24

Windows MCP sets up windows-mcp-delta, an extended Windows-MCP, and Claude can open and continue a session or a chat in any account through the program.

### New features

1. Windows MCP now sets up windows-mcp-delta (Windows-MCP with more of its own tools: files, the web, WSL, and the Claude Code sessions and chats of every account on the PC), its source carried in the program and run by uv. The button, the computer mark and Repair work as before.
2. Settings has a Windows MCP switch, “Let Claude read and search this account’s chats on claude.ai”, off by default. When it is on, Claude can read and search the signed-in account’s chats through that account’s own claude.ai session.
3. A **shield** at the window's bottom-right corner watches the IP Claude leaves by (Cloudflare's trace behind claude.ai) and what it is (country, datacenter or home, VPN/proxy/Tor, trust score, from ip.net.coffee, ipapi.is and proxycheck.io), the tunnel adapter and proxy app, and whether UDP leaks by another IP. When the exit is in a region Anthropic does not serve, or breaks a rule you set (no datacenter IP, a lowest trust score, no UDP leak, the tunnel or proxy gone, the exit unreadable), it ends Claude Desktop and Claude Code at once and the manager opens no profile until the exit is safe. Click the badge for the details and the rules; it is off until you turn it on.

### Improvements

1. The shield checks more: a public IPv6 that could bypass the proxy (a rule you can turn on), whether this PC's time zone and language match the exit's region, whether the default (Cloudflare) exit is in another country than Claude's, and the latency to claude.ai and anthropic.com with Anthropic's service status. The mismatches only show; they never end Claude
2. When the shield ends Claude, a Windows notification says why. **Only warn** keeps the badge red without ending Claude. **Recent events** in the shield's dialog lists when it stopped or cleared and why
3. A profile can be limited to some regions: in the shield's dialog, pick the profile and type its country codes (US, JP…); when the exit is elsewhere, only that profile is ended. A claude:// link is held back too while the exit is in a region Claude does not serve or one its profile does not allow
4. The shield now checks DNS leaks: it resolves a one-off subdomain of ip.net.coffee's DNS and reads back which resolver the query reached, flagging a leak when that resolver is in a region Anthropic does not serve. **End Claude when the exit leaks DNS to another region** is a rule you can turn on
5. A profile's command line can name a claude:// link: `ClaudeDelta.exe --launch <folder> <link>` opens it in that profile, asking nothing (how Claude itself opens a session or a chat in another account).

### Bug fixes

1. In **Sync settings…**, the profiles' check boxes no longer lie under the empty field whose row they take

## v2026.10.23

Resting the pointer on a profile shows its usage. A profile is exported to a .zip with **Export…** or by dragging it out of the list, and imported with **Import…** or by dropping the file on it, its sign-in, name, note and icon included, without a password. The local profile tools are gone; cleaning up sessions and copying `.claude` are in Settings. **Edit…**, exports, imports and sync conflicts share one list, each setting shown with its value, while **New…** keeps its short form. The sessions view looks and works like Claude's sidebar.

### New features

1. Resting the pointer on a profile shows its usage of the last five hours and seven days, as Claude last recorded it on this PC: nothing is asked of Claude, and a profile with none shows none
2. **Export…** and **Import…**, side by side beside the list in place of **Backup & Restore…**, and in the list's right-click menu
3. Drag profiles out of the list onto the desktop or a folder to export each to a .zip, and drop backups or sessions archives on a profile to import them into it. Once dropped, you choose what they hold, and the program writes them there itself, not Explorer: its window shows how far, and pauses, leaves for later or cancels; one cut short by an error or a close goes on from where it stopped the next time the manager opens
4. An export keeps the profile's own name, note, color, badge, picture and window title, and an import gives them to the profile; the list of what to export or import shows each of them, to change or leave out first: a text in an edit box, the color in a drop-down, the picture as a switch
5. **Clean up sessions…** and **Back up .claude…** are in **Settings**
6. Exports, imports and session archives list their sessions one by one, each with when it was last used, to leave some out: an import takes only those checked, and only their conversations' files (from an archive of an earlier version, all of them)
7. The settings an export or import lists show their values, to change before they go: one on or off as a switch, one of several in a drop-down, a text or number in an edit box
8. **Edit…** shows the profile as the same list: its name, color, badge, picture, window title and notes changed in place, its switches, how it syncs, and its settings one by one, which change there while its Claude is closed
9. A setting that holds a list or a set of values shows each of them below it, in exports, imports and **Edit…**: a session by its title, a folder by its path; each can be left out of an export or an import, and a text or a switch among them changed
10. **Zoom** in **Settings** makes every window of the program 90% to 200% of its size; the manager's window opens again at the new size
11. **Export…**, **Import…** and **Edit…** have a **Preview pane** on the right. The list on the left is an overview: a group opens to its items, each saying how many values it holds (or showing its only one), and the profile's own name, color, badge and the rest stay in it to change in place, as before. The pane shows what the row selected holds: a group's items, which open to their values, an item's values, a setting's parts, or the row itself, each changed there with check boxes, switches, drop-downs and edit boxes; a change there shows in the list, and one in the list shows there
12. The **Preview pane** has a search box: it keeps the rows whose names or values match and those holding them; its button chooses where it looks (names, values) and how (regular expression, match case), and **Ctrl+F** shows or hides it
13. The sign-in's row shows what it holds in the **Preview pane**: the account and its organization, the plan, each access token with its scopes, until when, and whether it renews itself, where the sign-in opens and how much it takes; never a token itself
14. Windows MCP, in the column: adds Windows-MCP (Claude works Windows itself: apps, windows, files, mouse and keyboard) to the profiles selected in one click, uv installed first when it is missing; an open profile is closed and opened again, asked first. Remove Windows MCP takes it away. It runs over stdio, named so that a ~/.windows-mcp/config.toml asking for HTTP cannot turn it into a server Claude cannot talk to; a profile set up by an earlier test build reads Repair Windows MCP, and servers left running after their Claude closed are offered to be ended. A profile with it shows a computer at the end of its name (amber while it needs repairing), and when some of those selected are open, the question also offers to change only the closed ones.

### Improvements

1. Exporting the sign-in no longer asks for a password: it opens on this PC or another, so keep the file safe
2. Export and Import start with everything checked, the sign-in included
3. An import checks the whole archive before it changes anything, so there is no separate check to run first
4. **Sync conflicts**, and the choice of backup when several hold the same part, are that list, read-only: the versions side by side, the one kept checked
5. The export's dialog, its questions and its results say Export, as the button does, not Back up
6. Every row of these lists has an icon that says what it is, a setting its own item's, and no line runs between a row's name and its value
7. Exporting a profile whose Claude is open, with its sign-in, closes it first (asked) and opens it again once the export is written: the sign-in's files are in use while it runs. To export without the sign-in, uncheck it in the list
8. **Ctrl+F** shows the search box, goes into it, and hides it again, in the profile list, the sessions view and the **Preview pane**
9. These lists are quicker: what each box shows is worked out once for all its rows, and rows changed together are drawn once
10. A name with no value beside it runs on over the empty value column instead of being cut, and the **Preview pane**'s columns fit what it shows
11. The sessions view looks and works like Claude's sidebar: **Pinned** on top (what was Starred), sessions grouped by date at first (**Today**, **Yesterday**, then each date), headings open until you fold them, and before each session an icon of where it runs: Claude's code icon on this PC, a cloud, a phone for Remote Control, a command prompt for SSH or a chat bubble for Slack
12. A session's right-click menu starts as Claude's does, with **Open in**, **Pin**, **Rename…** and **Delete…**, then the manager's own commands; the filter menu starts with **Group by**, **Sort by** and **Show**, then **Status**, **Last recorded result** and **Environment**
13. Each heading of the sessions view shows its first five sessions, as Claude's sidebar does: **Show more** shows ten more, then **Show all** the rest; **Expand all** shows them all, and a search shows every match
14. **New…** is the short form it was: name, color, badge or picture, title, notes and sync in fields, the check boxes below; it is filled and saved by the same code as **Edit…**'s list
15. Headings of the sessions view open and fold at once, without the rows sliding
16. A double-click in the sessions view no longer opens a session: Enter or **Open in** does, and a double-click on a heading opens or folds it
17. A session Claude shows unread has a blue dot at its row's end, and its menu has **Mark as read** or **Mark as unread**, as Claude's does; several sessions or a heading take it all at once. A profile that is open gets it once it closes
18. The sessions view's menus look like Claude's: **Open in**, then **Pin**, **Mark as read**, **Rename…**, the manager's own commands, and last **Delete…** and **Delete session everywhere…** in red, with the keys that do each at its right end; the profile list's **Delete…** is red too
19. The profile list's menu and the sessions view's menus have an icon before each command; **Export…** and **Import…** have a matching pair of icons, an arrow up and an arrow down
20. **Private sign-in…** opens in a window of its own (Microsoft Edge WebView2, part of Windows) when it can: the page knows no account, and once Claude Desktop is authorized the window gives the link to the profile itself and closes, with nothing to allow or choose, its browser folder deleted. Edge and Chrome stay a choice
21. **Sync settings…** looks like **Export…**: the profiles to sync with are check boxes in a row at the top, and what they sync is the same list, with its **Preview pane** on the right
22. In a profile that syncs, **Rename…**, **Pin**, **Mark as read** and **Delete…** in the sessions view change the session in the profiles of its group at once too (when the group syncs titles, pins, unread marks or deletions); one Ctrl+Z undoes it everywhere
23. A session's menu is shorter: **Open in**, **Pin**, **Mark as read**, **Rename…**, then **Preview**, **Share with**, **Export** (original files, Markdown, a web page, JSON or plain text), **Rewind and fork relationships…**, **Show folder**, and last **Delete…**. Copy, cut and paste stay on Ctrl+C, Ctrl+X and Ctrl+V; **Delete session everywhere** on Shift+Del
24. In a profile that syncs, a command chosen with Shift, Ctrl or Alt held changes the profile shown alone, not its whole group
25. **Preview** shows the conversation as a web page in a window of the program's own (WebView2), its links opening in your browser
26. A button left of the sessions' search box folds every heading, or opens them all when all are folded
27. Sessions view: headings start folded and the ones you open stay open; the view comes back as you left it, scrolled where it was, also after a restart, without being filled again; Esc goes back to the profiles.
28. Sessions view: as in Claude's sidebar, each session shows at its row's end where it runs (a laptop on this PC, a cloud, ...), and an orange dot instead while it is unread; a heading with an unread session has an orange dot too.
29. Sessions view: the unread marks are read again only when Claude's web storage changed, so the view opens faster.
30. The profile list's menu has Sessions view (V); Unlink sessions folder and Recover sessions take L and N as their keys.
31. The Sync settings dialog is narrower.
32. Edit… shows the same form as New… (several profiles: only how they sync); the profile's Claude settings are no longer edited there (Sync settings and Export show them).
33. New…: Save and sign in now, at the bottom left, makes the profile and opens its private sign-in.
34. Dragging profiles shows tiles over the foot of the list: Export (choose what it holds, then the folder), Copy profile (a new profile filled from it, its settings copied), Private sign-in and Delete. Letting them go on a folder still exports them there.
35. Chinese: private sign-in is called 无痕登录.
36. Settings: choose the browser Private sign-in starts with: the built-in window (default), Edge, Chrome, Brave or Firefox, each shown with its own icon. Brave and Firefox are new.
37. The program is now called Claude Delta. Its folders, settings and Start menu folder keep their earlier name, so an installed copy carries on as it was.
38. Private sign-in opens straight in the browser chosen in Settings, without its dialog. A Magic link on the clipboard (the sign-in link Claude mails) is offered, naming its address, when it opens and whenever one is copied while the sign-in window is open.

### Bug fixes

1. Exporting and importing an account works: the sign-in was left out unless checked, and one exported on another PC could not be imported without its password
2. Drop-down lists are as tall as the edit boxes beside them, with no gap below
3. Profiles dragged out of the list and brought back onto the manager before letting go no longer leave it unresponsive: the drag is simply cancelled
4. A profile exported while its Claude was open and imported into another profile no longer comes in signed out: the export left the sign-in out when its files were in use, and now closes the profile for it
5. Right-clicking a profile that is not selected shows that profile's menu, not the menu of the profile selected before
6. The question mark (and every icon) in the program's message boxes keeps its shape: a box widened for long text no longer stretched it.

### Breaking changes

1. **Backup & Restore…**, **Local profile tools…**, configuration snapshots, backup history, archive validation, the recent operation results and the manager configuration export are taken out: usage shows in the list, an export keeps the profile's own settings, and an import checks its archive. What they kept in the manager's folder is removed

### Upgrade guide

1. An export made before this version whose sign-in has a password still asks for it when imported
2. The sessions view groups by date now, as Claude does; a grouping you chose other than by folder stays. To group by folder again, choose **Group by** > **Folder** in the filter menu
3. **Star** and **Remove** in the sessions view are now **Pin** and **Delete**, Claude's words; they do what they did

## v2026.10.22

Main syncs again after Claude moved its data out of its package, and the manager sees it running again. Session sync keeps changes it could not make and keeps deleted sessions deleted, backups take profiles of any size, a restore that fails leaves the profile as it was, and Settings can keep profiles closed after a Claude update. Unlinking a session folder, deleting and bringing back sessions, and reading backups are safer too.

### New features

1. Settings: **Open profiles again after a Claude update**, on at first; off, a profile a Claude update closed stays closed (Windows still opens Main itself)

### Improvements

1. Session presets are merged across a sync group: each profile keeps its own and gets the others', and they are never a sync conflict
2. Writing a batch of sessions to a profile checks once which running Claude shares its entries, instead of reading every entry before and after each one
3. The vault looks for entries no version names at most once a day, instead of reading every version of every list at each sync
4. Searching sessions no longer keeps the window waiting while a long conversation is read
5. The clean-up list names a conversation's profiles as everywhere else ("Main, Work and Test")

### Bug fixes

1. Main, the regular Claude icon's profile, syncs its sessions, settings and layout again: since Claude moved its data to `%LOCALAPPDATA%\Claude-Data`, the manager found no sessions for it and left it out of its sync group
2. Main shows as Running again while it runs, so changes for it wait until it closes, and opening it from the manager no longer reports that it did not start
3. Sessions without a folder that Main made before the move stay its own: a sync neither rewrites them nor sends them as another profile's
4. A sync no longer sends a profile the same layout every time when its own session presets, or a page zoom it has none of yet, keep what it holds
5. A change sent to a profile that could not be made when it closed waits for the next time, five times at most, instead of being dropped and later undone by the profile's older entry
6. A session deleted everywhere no longer comes back in a profile it had been sent to while that profile was open
7. Syncing several groups at once: one that fails no longer keeps the others from saving their versions
8. The vault's list of deleted sessions is never written over when it cannot be read
9. Backups that include sessions are no longer limited to 512 MB: large files go into the archive a part at a time
10. A restore writes each settings file in one step, and a sign-in restore that fails puts the profile's sign-in back as it was
11. Restore no longer leaves out files with two dots in their name (`draft..v2.md`); names Windows keeps for devices are refused in every archive
12. A backup no longer fails because of one scheduled task it cannot name: that task counts as missing
13. Writing the web storage refuses a log that changed since it was read, and a database with more than 512 tables is no longer read in part
14. After a Claude update, a profile's watcher finds the new version within seconds instead of waiting up to two minutes
15. A window title given up because Claude kept setting its own is given again after a rename, a new title or a new Claude
16. Quitting a profile no longer logs an error when Claude quit and only a program it started was already ending
17. The sessions view's profile icon follows a new badge or picture; the Filter and Search options menus close when their button is clicked again
18. Lists with icons follow a light or dark mode switch and a new display scale while their dialog is open
19. Chinese: the waiting profiles and the clean-up list's sizes in Chinese punctuation and units
20. Giving a profile its own session folder again copies the sessions first and removes the link only once they all are there: a copy that fails no longer leaves the folder half filled
21. A session deleted in a profile whose deletion mark could not be written is tried again, so Claude does not take the session in again
22. A session brought back from the vault takes Claude's deletion marks away from profiles that are open too, once they close, each kept in a backup first
23. What is written into Claude's files is checked as complete JSON first: a value that is not is left out instead of written
24. A backup whose key asks for an unreasonable amount of work is refused at once instead of hanging the restore
25. Changes waiting for a profile no longer leave stray files in the manager's folder

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
