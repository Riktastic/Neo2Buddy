"""Python API wrapper for AlphaSmart Neo2 Buddy (portal HTTP client + CLI)."""

from .client import ALPHAWORD_APPLET_ID, STOCK_APPLET_IDS, Neo2BuddyClient
from .exceptions import ApiError, AuthError, BusyError, Neo2BuddyError

__all__ = [
    "ALPHAWORD_APPLET_ID",
    "STOCK_APPLET_IDS",
    "Neo2BuddyClient",
    "Neo2BuddyError",
    "AuthError",
    "BusyError",
    "ApiError",
]

__version__ = "1.0.0"
