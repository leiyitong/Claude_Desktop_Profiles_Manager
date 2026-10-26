import asyncio
import http.server
import json
import threading

import pytest

from windows_mcp_delta import state
from windows_mcp_delta.tools import web

PAGE = """<!doctype html><html><head><title>Release notes</title><script>var x = 1;</script></head>
<body><header><a href="/">Home</a> <a href="/docs">Docs</a></header>
<nav><ul><li>Menu one</li><li>Menu two</li></ul></nav>
<main><h1>Version 2.0</h1><p>The <a href="/sync">sync</a> is faster.</p>
<p>%s</p><img src="x.png" alt="a chart"><ul><li>First change</li><li>Second change</li></ul></main>
<footer>Copyright</footer></body></html>""" % (" ".join(f"word{i}" for i in range(3000)))


class Handler(http.server.BaseHTTPRequestHandler):
    def do_GET(self):
        if self.path == "/page":
            body, kind = PAGE.encode(), "text/html; charset=utf-8"
        elif self.path == "/data":
            body, kind = json.dumps({"a": [1, 2], "名字": "值"}).encode(), "application/json"
        elif self.path == "/moved":
            self.send_response(302)
            self.send_header("Location", "/page")
            self.end_headers()
            return
        elif self.path == "/file.bin":
            body, kind = bytes(range(256)) * 10, "application/octet-stream"
        else:
            self.send_response(404)
            self.end_headers()
            return
        self.send_response(200)
        self.send_header("Content-Type", kind)
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def log_message(self, *args):
        pass


@pytest.fixture(scope="module")
def site():
    server = http.server.ThreadingHTTPServer(("127.0.0.1", 0), Handler)
    threading.Thread(target=server.serve_forever, daemon=True).start()
    yield f"http://127.0.0.1:{server.server_port}"
    server.shutdown()


@pytest.fixture(autouse=True)
def fresh(monkeypatch):
    web._cache.clear()
    for name in ("HTTP_PROXY", "HTTPS_PROXY", "http_proxy", "https_proxy", "ALL_PROXY", "all_proxy"):
        monkeypatch.delenv(name, raising=False)
    monkeypatch.setenv("NO_PROXY", "127.0.0.1,localhost")


def test_main_text_only(site):
    out = asyncio.run(web.web_fetch(site + "/page", max_chars=2000))
    assert out.startswith(f"URL: {site}/page") and "Title: Release notes" in out and "# Version 2.0" in out
    assert "The sync is faster." in out
    assert "[image: a chart]" in asyncio.run(web.web_fetch(site + "/page", find="a chart"))
    for gone in ("Menu one", "Copyright", "var x", "Docs"):
        assert gone not in out
    assert "offset=" in out and "of " in out
    page2 = asyncio.run(web.web_fetch(site + "/page", offset=2000, max_chars=1000))
    assert "characters 2000–3000" in page2
    linked = asyncio.run(web.web_fetch(site + "/page", links=True, max_chars=500))
    assert "[sync](/sync)" in linked


def test_find_passages(site):
    out = asyncio.run(web.web_fetch(site + "/page", find="Second change"))
    assert "1 passage" in out and "[offset " in out and "Second change" in out and "word10 " not in out
    assert "is not in the page" in asyncio.run(web.web_fetch(site + "/page", find="nowhere"))
    assert asyncio.run(web.web_fetch(site + "/page", find="word2999|word1500", regex=True)).count("[offset ") == 2


def test_json_redirect_and_errors(site):
    out = asyncio.run(web.web_fetch(site + "/data"))
    assert '"名字": "值"' in out
    moved = asyncio.run(web.web_fetch(site + "/moved", max_chars=500))
    assert moved.startswith(f"URL: {site}/page (asked: {site}/moved)")
    assert asyncio.run(web.web_fetch(site + "/missing")).startswith("Error: HTTP 404")


def test_download(site, tmp_path):
    state.reset()
    state.set_cwd(str(tmp_path))
    out = asyncio.run(web.download(site + "/file.bin"))
    assert out.startswith(f"Saved {tmp_path / 'file.bin'} (2,560 bytes")
    assert (tmp_path / "file.bin").read_bytes() == bytes(range(256)) * 10
    assert "exists" in asyncio.run(web.download(site + "/file.bin"))
    assert asyncio.run(web.download(site + "/file.bin", path="sub/x.bin")).startswith("Saved")
    assert (tmp_path / "sub" / "x.bin").exists()
    state.reset()


def test_search_parses_results(monkeypatch):
    html = """<div class="result"><a class="result__a" href="//duckduckgo.com/l/?uddg=https%3A%2F%2Fexample.com%2Fa&rut=x">Example A</a>
    <a class="result__snippet">About A</a></div>
    <div class="result result--ad"><a class="result__a" href="https://ads.example">Ad</a></div>
    <div class="result"><a class="result__a" href="https://example.org/b">Example B</a></div>"""

    class Response:
        text = html

        def raise_for_status(self):
            pass

    monkeypatch.setattr(web.requests.Session, "post", lambda self, *a, **k: Response())
    out = asyncio.run(web.web_search("example"))
    assert out == "1. Example A\n   https://example.com/a\n   About A\n2. Example B\n   https://example.org/b"
