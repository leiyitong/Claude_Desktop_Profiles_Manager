import os
import sys
from pathlib import Path

os.environ.setdefault("WINDOWS_MCP_DELTA_NO_ESC_HOOK", "1")   # no real keyboard hook in the tests

ROOT = Path(__file__).resolve().parent
sys.path.insert(0, str(ROOT.parent / "src"))
try:
    import windows_mcp  # noqa: F401  (the real one, on Windows)
except ImportError:
    sys.path.insert(0, str(ROOT / "stub"))
