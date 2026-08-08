"""Exceptions for the Neo2 Buddy remote client."""


class Neo2BuddyError(Exception):
    """Base error for remote API failures."""


class AuthError(Neo2BuddyError):
    """Login or token rejected."""


class BusyError(Neo2BuddyError):
    """Device is busy (e.g. backup already running)."""


class ApiError(Neo2BuddyError):
    """HTTP or API-level failure."""

    def __init__(self, message: str, *, status_code: int | None = None, body: str | None = None):
        super().__init__(message)
        self.status_code = status_code
        self.body = body
