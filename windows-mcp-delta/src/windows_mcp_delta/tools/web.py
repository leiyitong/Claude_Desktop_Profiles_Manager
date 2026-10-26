"""WebFetch, WebSearch, Download: the web without a browser, in few tokens.

Windows-MCP's Scrape hands the page to the client to summarize (MCP
sampling); a client that cannot (Claude Desktop) gets the whole page back.
WebFetch keeps a page's main text only (navigation, headers, footers,
scripts and the like left out) as Markdown, links as their text unless
asked, and gives it a part at a time: offset and max_chars page through it,
find returns only the passages that hold a text. A page fetched is kept for
ten minutes, so paging does not fetch it again. JSON comes back indented,
a PDF as its text, an image as an image.
"""
from __future__ import annotations

import asyncio
import json
import os
import re
import threading
import time
from typing import Annotated
from urllib.parse import parse_qs, quote_plus, unquote, urlparse

import requests
from mcp.types import ToolAnnotations
from pydantic import Field

from windows_mcp_delta import state
from windows_mcp_delta.extend import delta

AGENT = ("Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) "
         "Chrome/130.0.0.0 Safari/537.36")
FETCH_MAX = 20 * 1024 * 1024      # bytes read of a page at most
DOWNLOAD_MAX = 2 * 1024 ** 3
KEEP_SECONDS = 600
TIMEOUT = 30
_DROPPED = ("script", "style", "noscript", "svg", "nav", "footer", "header", "aside", "form", "iframe", "template",
            "button", "dialog")
_cache: dict[tuple[str, bool], tuple[float, str, str]] = {}    # (url, links) -> (when, final url, text)
_cache_lock = threading.Lock()
_READ = ToolAnnotations(readOnlyHint=True, idempotentHint=True, openWorldHint=True)


def _session() -> requests.Session:
    session = requests.Session()
    session.headers.update({"User-Agent": AGENT, "Accept-Language": "en-US,en;q=0.9,zh-CN;q=0.8"})
    return session


def _get(url: str, limit: int = FETCH_MAX) -> tuple[requests.Response, bytes]:
    if not re.match(r"^https?://", url, re.I):
        url = "https://" + url
    response = _session().get(url, timeout=TIMEOUT, stream=True, allow_redirects=True)
    data = bytearray()
    for chunk in response.iter_content(65536):
        data += chunk
        if len(data) > limit:
            break
    return response, bytes(data[:limit])


def html_to_markdown(html: str, links: bool = False) -> tuple[str, str]:
    """A page's title and its main text as Markdown."""
    from bs4 import BeautifulSoup
    from markdownify import markdownify
    soup = BeautifulSoup(html, "html.parser")
    title = soup.title.get_text(" ", strip=True) if soup.title else ""
    for tag in soup(list(_DROPPED)):
        tag.decompose()
    for hidden in soup.select('[hidden], [aria-hidden="true"], [role="navigation"], [role="banner"], [role="contentinfo"]'):
        hidden.decompose()
    candidates = soup.find_all(["main", "article"]) or [soup.body or soup]
    main = max(candidates, key=lambda node: len(node.get_text(" ", strip=True)))
    if not links:
        for anchor in main.find_all("a"):
            anchor.unwrap()
    for image in main.find_all("img"):
        alt = image.get("alt", "").strip()
        image.replace_with(f"[image: {alt}]" if alt else "")
    text = markdownify(str(main), heading_style="ATX", bullets="-")
    text = re.sub(r"[ \t]+\n", "\n", text)
    text = re.sub(r"\n{3,}", "\n\n", text).strip()
    return title, text


def _decode(data: bytes, content_type: str) -> str:
    """A page's bytes as text: in the charset its header or its <meta> names, else UTF-8, else a guess."""
    named = re.search(r"charset=[\"']?([\w.:-]+)", content_type, re.I) or \
        re.search(rb"<meta[^>]+charset=[\"']?([\w.:-]+)", data[:4096], re.I)
    if named:
        name = named.group(1).decode("ascii", "ignore") if isinstance(named.group(1), bytes) else named.group(1)
        try:
            return data.decode(name, errors="replace")
        except LookupError:
            pass
    try:
        return data.decode("utf-8")
    except UnicodeDecodeError:
        try:
            from charset_normalizer import from_bytes
            best = from_bytes(data).best()
            if best is not None:
                return str(best)
        except ImportError:
            pass
        return data.decode("latin-1")


def _page_text(url: str, links: bool):
    """The page at `url` as text: (final url, its text), or an Image; kept a while."""
    key = (url, links)
    with _cache_lock:
        kept = _cache.get(key)
    if kept and time.time() - kept[0] < KEEP_SECONDS:
        return kept[1], kept[2]
    response, data = _get(url)
    if response.status_code >= 400:
        raise RuntimeError(f"HTTP {response.status_code} {response.reason} from {response.url}")
    kind = response.headers.get("Content-Type", "").split(";")[0].strip().lower()
    final = response.url
    if kind.startswith("image/"):
        from fastmcp.utilities.types import Image
        return Image(data=data, format=kind.split("/", 1)[1].split("+")[0])
    if kind == "application/pdf" or data[:5] == b"%PDF-":
        import io
        from pypdf import PdfReader
        reader = PdfReader(io.BytesIO(data))
        text = "\n\n".join(f"--- page {n} ---\n{(page.extract_text() or '').strip()}" for n, page in enumerate(reader.pages, 1))
    else:
        body = _decode(data, response.headers.get("Content-Type", ""))
        if "json" in kind:
            try:
                text = json.dumps(json.loads(body), indent=1, ensure_ascii=False)
            except ValueError:
                text = body
        elif "html" in kind or body.lstrip()[:15].lower().startswith(("<!doctype html", "<html")):
            title, text = html_to_markdown(body, links)
            text = f"Title: {title}\n\n{text}" if title else text
        else:
            text = body
    with _cache_lock:
        _cache[key] = (time.time(), final, text)
        for old in [k for k, v in _cache.items() if time.time() - v[0] > KEEP_SECONDS]:
            _cache.pop(old, None)
    return final, text


def _passages(text: str, find: str, regex: bool, around: int, limit: int) -> list[tuple[int, str]]:
    """The places `find` is in `text`: (offset, the passage around it), passages that overlap merged."""
    pattern = re.compile(find if regex else re.escape(find), re.IGNORECASE)
    spans: list[list[int]] = []
    for match in pattern.finditer(text):
        low, high = max(0, match.start() - around), min(len(text), match.end() + around)
        if spans and low <= spans[-1][1]:
            spans[-1][1] = high
        else:
            spans.append([low, high])
        if len(spans) > limit:
            break
    return [(low, ("…" if low else "") + text[low:high].strip() + ("…" if high < len(text) else "")) for low, high in spans[:limit]]


@delta.add(
    "WebFetch",
    description=("Fetches a web page (or JSON, text, a PDF, an image) and returns its main text as Markdown: navigation, "
                 "headers, footers and scripts left out, links as plain text unless links=true. Long pages come a part "
                 "at a time (max_chars, then offset for the next part); find returns only the passages that hold a "
                 "text (or a regular expression with regex=true), with their offsets. A page is kept ten minutes, so "
                 "paging does not fetch it again. Cheaper than Scrape or a browser for reading."),
    annotations=_READ,
)
async def web_fetch(
    url: Annotated[str, Field(description="The address (https:// assumed when left out).")],
    find: Annotated[str | None, Field(description="Return only the passages that hold this.")] = None,
    regex: Annotated[bool, Field(description="find is a regular expression.")] = False,
    offset: Annotated[int, Field(description="Where to start in the page's text (characters).", ge=0)] = 0,
    max_chars: Annotated[int, Field(description="How much text at most.", ge=500, le=200000)] = 20000,
    links: Annotated[bool, Field(description="Keep links as Markdown links (their addresses cost tokens).")] = False,
):
    try:
        result = await asyncio.to_thread(_page_text, url.strip(), links)
    except requests.RequestException as error:
        return f"Error: could not fetch {url}: {error}"
    except RuntimeError as error:
        return f"Error: {error}"
    if not isinstance(result, tuple):
        return result   # an image
    final, text = result
    head = f"URL: {final}" + (f" (asked: {url})" if final.rstrip("/") != url.strip().rstrip("/") and "://" in url else "")
    if find:
        try:
            found = _passages(text, find, regex, 300, 30)
        except re.error as error:
            return f"Error: bad regular expression: {error}"
        if not found:
            return f"{head}\n{find!r} is not in the page ({len(text)} characters)."
        out, used = [], 0
        for at, passage in found:
            if used + len(passage) > max_chars and out:
                out.append(f"(more passages: find something narrower, or offset={at})")
                break
            out.append(f"[offset {at}] {passage}")
            used += len(passage)
        return head + f" · {len(found)} passage{'s' if len(found) != 1 else ''}\n\n" + "\n\n".join(out)
    if offset >= len(text) and text:
        return f"{head}\n(offset {offset} is past the end: the page has {len(text)} characters)"
    part = text[offset:offset + max_chars]
    end = offset + len(part)
    tail = f"\n\n(characters {offset}–{end} of {len(text)}; offset={end} for more)" if end < len(text) or offset else ""
    return f"{head}\n\n{part}{tail}"


def _duckduckgo(query: str, limit: int) -> list[tuple[str, str, str]]:
    from bs4 import BeautifulSoup
    response = _session().post("https://html.duckduckgo.com/html/", data={"q": query}, timeout=TIMEOUT)
    response.raise_for_status()
    soup = BeautifulSoup(response.text, "html.parser")
    results = []
    for item in soup.select(".result"):
        anchor = item.select_one("a.result__a")
        if not anchor or "result--ad" in (item.get("class") or []):
            continue
        href = anchor.get("href", "")
        if "uddg=" in href:   # DuckDuckGo's redirect: the address is in it
            href = unquote(parse_qs(urlparse(href).query).get("uddg", [href])[0])
        snippet = item.select_one(".result__snippet")
        results.append((anchor.get_text(" ", strip=True), href, snippet.get_text(" ", strip=True) if snippet else ""))
        if len(results) >= limit:
            break
    if not results and "anomaly" in response.text.lower():
        raise RuntimeError("DuckDuckGo asked for a check that a person is searching: try again later, or open "
                           f"https://duckduckgo.com/?q={quote_plus(query)} with the App tool")
    return results


@delta.add(
    "WebSearch",
    description=("Searches the web (DuckDuckGo) and returns the results: title, address and a line about each. "
                 "WebFetch then reads the pages worth reading."),
    annotations=_READ,
)
async def web_search(
    query: Annotated[str, Field(description="What to search for; site:example.com narrows it to a site.")],
    limit: Annotated[int, Field(description="How many results.", ge=1, le=30)] = 8,
) -> str:
    try:
        results = await asyncio.to_thread(_duckduckgo, query, limit)
    except (requests.RequestException, RuntimeError) as error:
        return f"Error: the search failed: {error}"
    if not results:
        return f"No results for {query!r}."
    return "\n".join(f"{n}. {title}\n   {href}" + (f"\n   {snippet}" if snippet else "")
                     for n, (title, href, snippet) in enumerate(results, 1))


@delta.add(
    "Download",
    description=("Downloads a file from the web to a path on this PC (a folder: the file keeps its name), without "
                 "passing it through the conversation."),
    annotations=ToolAnnotations(readOnlyHint=False, destructiveHint=True, idempotentHint=True, openWorldHint=True),
)
async def download(
    url: Annotated[str, Field(description="The file's address.")],
    path: Annotated[str | None, Field(description="Where to save it: a file, or a folder (the working directory by default).")] = None,
    overwrite: Annotated[bool, Field(description="Replace a file already there.")] = False,
) -> str:
    def fetch() -> str:
        target = state.resolve(path) if path else state.cwd()
        response = _session().get(url, timeout=TIMEOUT, stream=True, allow_redirects=True)
        if response.status_code >= 400:
            return f"Error: HTTP {response.status_code} {response.reason} from {response.url}"
        if os.path.isdir(target) or (path or "").endswith(("/", "\\")):
            name = None
            disposition = response.headers.get("Content-Disposition", "")
            match = re.search(r"filename\*?=(?:UTF-8'')?\"?([^\";]+)", disposition, re.I)
            if match:
                name = unquote(match.group(1)).strip()
            name = os.path.basename(name or unquote(urlparse(response.url).path)) or "download"
            target = os.path.join(target, re.sub(r'[<>:"/\\|?*\x00-\x1f]', "_", name))
        if os.path.exists(target) and not overwrite:
            return f"Error: {target} exists (overwrite=true replaces it)."
        os.makedirs(os.path.dirname(target) or ".", exist_ok=True)
        size = 0
        partial = target + ".part"
        with open(partial, "wb") as file:
            for chunk in response.iter_content(1 << 20):
                size += len(chunk)
                if size > DOWNLOAD_MAX:
                    file.close()
                    os.remove(partial)
                    return f"Error: larger than {DOWNLOAD_MAX >> 30} GB: not downloaded."
                file.write(chunk)
        os.replace(partial, target)
        kind = response.headers.get("Content-Type", "").split(";")[0]
        return f"Saved {target} ({size:,} bytes{', ' + kind if kind else ''})."

    try:
        return await asyncio.to_thread(fetch)
    except (requests.RequestException, OSError) as error:
        return f"Error: could not download {url}: {error}"
