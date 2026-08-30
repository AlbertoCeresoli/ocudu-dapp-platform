# SPDX-FileCopyrightText: Copyright (C) 2026 DeepSig Inc.
# SPDX-License-Identifier: BSD-3-Clause-Clear
"""One-to-one SCTP association using only the standard library.

Each E3 PDU is one SCTP message. Sending uses a single ``send``; receiving
accumulates partial deliveries until the kernel sets ``MSG_EOR``. Event
notifications are not subscribed, so ``MSG_NOTIFICATION`` deliveries are
skipped defensively.
"""

from __future__ import annotations

import errno
import socket
from typing import Optional

MSG_NOTIFICATION = 0x8000
IPPROTO_SCTP = getattr(socket, "IPPROTO_SCTP", 132)


class SctpTransportError(OSError):
    """Raised when the association fails or a message cannot be framed."""


class SctpAssociation:
    """Blocking SCTP client association with message-boundary framing."""

    def __init__(
        self,
        host: str,
        port: int,
        timeout: float = 2.0,
        maximum_message_bytes: int = 2 * 1024 * 1024,
        socket_buffer_bytes: int = 2 * 1024 * 1024,
    ):
        self.host = host
        self.port = port
        self.timeout = timeout
        self.maximum_message_bytes = maximum_message_bytes
        family = socket.AF_INET6 if ":" in host else socket.AF_INET
        self._socket: Optional[socket.socket] = socket.socket(family, socket.SOCK_STREAM, IPPROTO_SCTP)
        try:
            # Best effort; the kernel clamps to net.core.{r,w}mem_max.
            self._socket.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, socket_buffer_bytes)
            self._socket.setsockopt(socket.SOL_SOCKET, socket.SO_SNDBUF, socket_buffer_bytes)
            self._socket.settimeout(timeout)
            self._socket.connect((host, port))
        except OSError as error:
            self._socket.close()
            self._socket = None
            raise SctpTransportError(f"cannot connect SCTP to {host}:{port}: {error}") from error

    # ------------------------------------------------------------------
    def close(self) -> None:
        if self._socket is not None:
            try:
                self._socket.close()
            finally:
                self._socket = None

    def __enter__(self) -> "SctpAssociation":
        return self

    def __exit__(self, *_exc) -> None:
        self.close()

    @property
    def connected(self) -> bool:
        return self._socket is not None

    def fileno(self) -> int:
        if self._socket is None:
            raise SctpTransportError("association is closed")
        return self._socket.fileno()

    def set_timeout(self, timeout: Optional[float]) -> None:
        if self._socket is None:
            raise SctpTransportError("association is closed")
        self.timeout = timeout if timeout is not None else 0.0
        self._socket.settimeout(timeout)

    # ------------------------------------------------------------------
    def send(self, message: bytes) -> None:
        """Send one complete message; SCTP preserves the boundary."""
        if self._socket is None:
            raise SctpTransportError("association is closed")
        if not message or len(message) > self.maximum_message_bytes:
            raise SctpTransportError("message is empty or exceeds the transport bound")
        try:
            sent = self._socket.send(message)
        except OSError as error:
            raise SctpTransportError(f"SCTP send failed: {error}") from error
        if sent != len(message):
            raise SctpTransportError("short SCTP send")

    def receive(self, timeout: Optional[float] = None) -> Optional[bytes]:
        """Receive one complete message.

        Returns ``None`` when the timeout elapses before any bytes arrive.
        Raises :class:`SctpTransportError` when the peer closes the association
        or a message exceeds the bound.
        """
        if self._socket is None:
            raise SctpTransportError("association is closed")
        previous = self._socket.gettimeout()
        if timeout is not None:
            self._socket.settimeout(timeout)
        message = bytearray()
        try:
            while True:
                try:
                    chunk, _ancillary, flags, _address = self._socket.recvmsg(64 * 1024)
                except socket.timeout:
                    if message:
                        raise SctpTransportError("timed out inside a partially received message")
                    return None
                except OSError as error:
                    if error.errno in (errno.ECONNRESET, errno.EPIPE, errno.ENOTCONN):
                        self.close()
                        raise SctpTransportError("association closed by peer") from error
                    raise SctpTransportError(f"SCTP receive failed: {error}") from error
                if not chunk:
                    self.close()
                    raise SctpTransportError("association closed by peer")
                if flags & MSG_NOTIFICATION:
                    continue
                message.extend(chunk)
                if len(message) > self.maximum_message_bytes:
                    raise SctpTransportError("message exceeds the transport bound")
                if flags & socket.MSG_EOR:
                    return bytes(message)
        finally:
            if timeout is not None and self._socket is not None:
                self._socket.settimeout(previous)
