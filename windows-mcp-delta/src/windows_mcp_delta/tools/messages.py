"""SendMessage, Inbox: messages between the conversations of this PC's
profiles, through the mailbox (mailbox.py). Idle wakes on them; Inbox reads
what is waiting without waiting.
"""
from __future__ import annotations

from typing import Annotated

from mcp.types import ToolAnnotations
from pydantic import Field

from windows_mcp_delta import caller, claude, mailbox
from windows_mcp_delta.extend import delta
from windows_mcp_delta.tools.sessions import _find_chat


@delta.add(
    "SendMessage",
    description=("Sends a message to another conversation on this PC: to a profile (its name or folder, as Profiles "
                 "lists them: whichever of its conversations reads its inbox first takes it), to one conversation "
                 "(its id: a message's 'conversation', or ListSessions kind=chat), or to every other profile (all). "
                 "It waits in their inbox until they call Idle or Inbox; a reply comes back the same way."),
    annotations=ToolAnnotations(readOnlyHint=False, destructiveHint=False, idempotentHint=False, openWorldHint=False),
)
def send_message(
    to: Annotated[str, Field(description="A profile's name or folder, a conversation id, or all.")],
    text: Annotated[str, Field(description="The message.")],
    kind: Annotated[str, Field(description="A word the receiver can wait for (Idle's message condition), e.g. done.")] = "message",
) -> str:
    if not text.strip():
        return "Error: the message is empty."
    me = mailbox.session_id()
    mine = caller.conversation() or None
    every = claude.profiles()
    if to.strip().lower() in ("all", "*"):
        targets = [p.folder for p in every if p.folder != me]
        if not targets:
            return "No other profile to send to."
        for folder in targets:
            mailbox.post(folder, text, kind=kind, from_conversation=mine)
        return f"Sent to {len(targets)} profiles: {', '.join(targets)}."
    profile = claude.find_profile(to, every)
    conversation = None
    if profile is None:
        chat = _find_chat(to)
        if chat:
            conversation, profile_folder = chat[0], chat[1].get("profile", "")
            profile = claude.find_profile(profile_folder, every)
            folder = profile.folder if profile else profile_folder
        elif claude.UUID.fullmatch(to.strip()):
            return f"Error: conversation {to} has not used these tools, so its profile is not known: send to the profile."
        else:
            return f"Error: no profile or conversation {to!r}. Profiles: {', '.join(p.folder for p in every)}."
    else:
        folder = profile.folder
    mailbox.post(folder, text, kind=kind, conversation=conversation, from_conversation=mine)
    where = f"conversation {conversation[:8]} of {folder}" if conversation else folder
    return f"Sent to {where}" + (" (yourself)" if folder == me and not conversation else "") + "."


@delta.add(
    "Inbox",
    description=("The messages waiting for this conversation, now (Idle waits for them instead): taken, so each is "
                 "read once, unless keep=true."),
    annotations=ToolAnnotations(readOnlyHint=False, destructiveHint=False, idempotentHint=False, openWorldHint=False),
)
def inbox(
    keep: Annotated[bool, Field(description="Leave them in the inbox.")] = False,
    sender: Annotated[str | None, Field(description="Only those from this profile (its folder).")] = None,
    kind: Annotated[str | None, Field(description="Only those of this kind.")] = None,
) -> str:
    read = mailbox.peek if keep else mailbox.take
    messages = read(sender, kind, None, caller.conversation() or None)
    if not messages:
        return "No messages."
    return mailbox.describe(messages)
