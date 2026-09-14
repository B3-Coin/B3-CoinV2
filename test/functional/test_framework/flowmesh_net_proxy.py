#!/usr/bin/env python3
# Copyright (c) 2026 The B3Coin Core developers
# Distributed under the MIT software license.
"""Bounded loopback FMN2 byte relay for isolated network qualification.

It never signs, decodes application objects, or changes bytes. The public
105-byte hello identifies the channel and originating transport key. Only
post-handshake BULK bytes involving the selected test operator are delayed.
The first 105+65 bytes in each direction remain unthrottled so the fault does
not masquerade as a handshake failure. This is network emulation, not WAN.
"""

import socket
import threading
import time


class BulkFaultProfile:
    def __init__(self):
        self.lock = threading.Lock()
        self.key = None
        self.hold = False
        self.rate = 0
        self.delayed_bytes = 0
        self.transferred = [0, 0, 0]

    def configure(self, key=None, *, hold=False, rate=0):
        assert rate >= 0
        with self.lock:
            self.key, self.hold, self.rate = key, hold, rate

    def policy(self, involved):
        with self.lock:
            selected = self.key is not None and self.key in involved
            return (self.hold, self.rate) if selected else (False, 0)

    def record(self, channel, size, delayed):
        with self.lock:
            self.transferred[channel] += size
            if delayed:
                self.delayed_bytes += size

    def snapshot(self):
        with self.lock:
            return {"channel_bytes": list(self.transferred),
                    "delayed_bulk_bytes": self.delayed_bytes,
                    "bulk_rate_per_connection_direction": self.rate,
                    "bulk_held": self.hold}


class FlowMeshLoopbackProxy:
    """At most 24 connections, two <=512-byte pumps each, one local listener."""
    MAX_CONNECTIONS = 24

    def __init__(self, port, target_port, profile):
        self.port, self.target_port, self.profile = port, target_port, profile
        self.target_key = None
        self.stop_event = threading.Event()
        self.lock = threading.Lock()
        self.connections = set()
        self.threads = []
        self.listener = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        self.listener.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        self.listener.bind(("127.0.0.1", port))
        self.listener.listen(16)
        self.listener.settimeout(0.1)
        self.worker = threading.Thread(target=self._accept, daemon=True)
        self.worker.start()

    def _accept(self):
        while not self.stop_event.is_set():
            try:
                downstream, _ = self.listener.accept()
            except socket.timeout:
                continue
            except OSError:
                break
            with self.lock:
                if len(self.connections) >= self.MAX_CONNECTIONS:
                    downstream.close()
                    continue
                self.connections.add(downstream)
                # Keep completed thread objects bounded too.
                self.threads = [thread for thread in self.threads if thread.is_alive()]
                thread = threading.Thread(target=self._connection, args=(downstream,), daemon=True)
                self.threads.append(thread)
                thread.start()

    def _connection(self, downstream):
        upstream = None
        done = threading.Event()
        returning = None
        try:
            downstream.settimeout(0.1)
            hello = bytearray()
            deadline = time.monotonic() + 5
            while len(hello) < 105 and not self.stop_event.is_set():
                if time.monotonic() >= deadline:
                    return
                try:
                    part = downstream.recv(105 - len(hello))
                except socket.timeout:
                    continue
                if not part:
                    return
                hello.extend(part)
            if len(hello) != 105 or hello[:6] != b"FMN2\x02\x00" or hello[38] > 2:
                return
            upstream = socket.create_connection(("127.0.0.1", self.target_port), timeout=1)
            upstream.settimeout(0.1)
            upstream.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
            downstream.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
            upstream.sendall(hello)
            channel = hello[38]
            involved = (bytes(hello[40:73]).hex(),)
            returning = threading.Thread(target=self._pump,
                                         args=(upstream, downstream, channel, involved, 0, done),
                                         daemon=True)
            returning.start()
            self._pump(downstream, upstream, channel, involved, 105, done)
        except OSError:
            pass  # A stopped test node is an intentional reconnect condition.
        finally:
            done.set()
            for sock in (downstream, upstream):
                if sock is not None:
                    try:
                        sock.shutdown(socket.SHUT_RDWR)
                    except OSError:
                        pass
                    sock.close()
            if returning is not None:
                returning.join(timeout=1)
            with self.lock:
                self.connections.discard(downstream)

    def _pump(self, source, destination, channel, involved, seen, done):
        try:
            while not self.stop_event.is_set() and not done.is_set():
                try:
                    data = source.recv(min(512, 170 - seen) if seen < 170 else 512)
                except socket.timeout:
                    continue
                if not data:
                    break
                delayed = False
                if channel == 2 and seen >= 170:
                    delay_until = None
                    while not self.stop_event.is_set() and not done.is_set():
                        hold, rate = self.profile.policy((*involved, self.target_key))
                        delayed = delayed or hold or rate > 0
                        if hold:
                            done.wait(0.02)
                            continue
                        if not rate:
                            break
                        if delay_until is None:
                            delay_until = time.monotonic() + len(data) / rate
                        if time.monotonic() >= delay_until:
                            break
                        done.wait(min(0.02, delay_until - time.monotonic()))
                offset = 0
                while offset < len(data) and not self.stop_event.is_set() and not done.is_set():
                    try:
                        count = destination.send(data[offset:])
                    except socket.timeout:
                        continue
                    if count <= 0:
                        return
                    offset += count
                seen += offset
                self.profile.record(channel, offset, delayed)
        except OSError:
            pass
        finally:
            done.set()

    def stop(self):
        self.stop_event.set()
        self.listener.close()
        self.worker.join(timeout=1)
        with self.lock:
            connections, threads = list(self.connections), list(self.threads)
        for sock in connections:
            try:
                sock.shutdown(socket.SHUT_RDWR)
            except OSError:
                pass
        deadline = time.monotonic() + 3
        for thread in threads:
            thread.join(timeout=max(0, deadline - time.monotonic()))
