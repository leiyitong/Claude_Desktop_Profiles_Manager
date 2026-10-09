/*
 * Help: what someone new to the program asks first, in plain words. The menu
 * bar's Help menu lists the questions; each answer shows in a message.
 */
#include "app.h"

#define HELP_TOPICS    8
#define IDM_HELP_FIRST 0x6200

static const WCHAR *Question(int topic)
{
    switch (topic) {
    case 0: return TR(L"What is this program for?");
    case 1: return TR(L"Where are my conversations saved?");
    case 2: return TR(L"How do profiles keep the same conversations?");
    case 3: return TR(L"What is different from Claude alone?");
    case 4: return TR(L"How do I use it every day?");
    case 5: return TR(L"What protects my conversations?");
    case 6: return TR(L"I deleted a conversation by mistake");
    default: return TR(L"Before uninstalling or reinstalling Claude");
    }
}

static const WCHAR *Answer(int topic)
{
    switch (topic) {
    case 0:
        return TR(L"It lets you use several Claude accounts on one PC, each in its own window: a profile. Each profile has its own sign-in, "
                  L"settings and list of conversations. Claude itself is not changed: this program only starts it with a separate folder for "
                  L"each profile.\n\n"
                  L"It can also keep the Code conversations of several profiles the same, so that you can switch to another account and go on "
                  L"where you left off.");
    case 1:
        return TR(L"A Code conversation is saved in two parts.\n\n"
                  L"Its content is saved once for every profile, in the .claude folder of your user folder (%USERPROFILE%\\.claude).\n\n"
                  L"The list on the left of Claude (titles, stars, archived conversations) is saved by each profile in its own folder: Main's "
                  L"inside Claude's app folder, which uninstalling Claude removes; the others' in %APPDATA%\\Claude-<name>. Claude shows only "
                  L"the conversations its list names.");
    case 2:
        return TR(L"Select a profile, open the Sessions menu, choose Sync with, then the other profile; or choose it under Sync in the "
                  L"profile's Edit\x2026 dialog. What to sync\x2026, in the same menu, chooses what they keep the same. From then on, each time "
                  L"one of them closes, or opens through this program, what changed in one goes to the others: new conversations, titles, "
                  L"stars, archived and deleted ones, and the rest you chose. The content is shared already.\n\n"
                  L"Use one profile at a time: a profile that is open gets the others' changes once it closes.");
    case 3:
        return TR(L"Claude alone has one sign-in and one list of conversations. Here each profile has its own folder, so its own sign-in, "
                  L"settings and list, while the content of the conversations stays in one place for all of them.\n\n"
                  L"Claude does not let profiles share one list: it does not save into a linked folder. So this program copies the changes "
                  L"from one list to the others, and keeps their recent versions.");
    case 4:
        return TR(L"Open each profile from this program, or from the desktop, taskbar or Start menu shortcuts it makes: the regular Claude "
                  L"icon always opens Main.\n\n"
                  L"Use one profile at a time: quit it (Quit here, or Quit in Claude's menu in the notification area) before you open "
                  L"another one.\n\n"
                  L"Do not rename, move or link the profile folders.");
    case 5:
        return TR(L"- The session vault keeps the versions of every list of the last 30 days (at least the 20 latest), outside Claude's "
                  L"folders, in %LOCALAPPDATA%\\Claude Desktop Profiles Manager\\vault: Recover sessions puts one back.\n"
                  L"- Before this program changes a list, it saves what it replaces (the last 20 changes).\n"
                  L"- A conversation deleted in Claude stays on this PC until it is cleaned up: Recently deleted brings it back.\n"
                  L"- Once a week, when Claude is closed, the .claude folder is copied beside it as .claude_auto_<date>; only the two latest "
                  L"copies are kept, the older ones are removed.");
    case 6:
        return TR(L"In the Sessions menu, choose Recently deleted\x2026: it lists the conversations that no profile shows any more but that "
                  L"are still on this PC. Check the ones you want back and click Restore: they come back in every profile that listed them; in "
                  L"a profile that is open, once it has been closed and opened again.\n\n"
                  L"Delete moves the ones checked to the Recycle Bin, so that nothing brings them back.");
    default:
        return TR(L"Uninstalling Claude removes Main's folder: its sign-in, settings and list. The conversations in .claude stay, and so do "
                  L"the other profiles.\n\n"
                  L"Before: quit Claude, right-click Main and choose Back up\x2026, then click Back up .claude\x2026 on the right of the "
                  L"window.\n\n"
                  L"After installing Claude again: open Main from this program, sign in with the same account, then quit it. If it syncs "
                  L"with another profile, its list comes back by itself; if not, use Recover sessions\x2026 in the Sessions menu.\n\n"
                  L"Uninstalling this program asks which profiles to keep: keep them all.");
    }
}

/* The menu bar's Help menu: the questions, filled as it opens. */
void Help_FillMenu(HMENU menu)
{
    int topic;
    for (topic = 0; topic < HELP_TOPICS; topic++) AppendMenuW(menu, MF_STRING, IDM_HELP_FIRST + (UINT)topic, Question(topic));
}

/* A question chosen: its answer in a message. FALSE for another command. */
BOOL Help_Command(HWND owner, UINT command)
{
    if (command < IDM_HELP_FIRST || command >= IDM_HELP_FIRST + HELP_TOPICS) return FALSE;
    Ui_Ask(owner, IDI_INFORMATION, Answer((int)(command - IDM_HELP_FIRST)), TR(L"OK"), NULL, FALSE);
    return TRUE;
}
