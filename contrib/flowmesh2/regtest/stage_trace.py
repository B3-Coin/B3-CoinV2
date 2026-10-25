#!/usr/bin/env python3
"""Opt-in stage timestamps for the latency investigation (M0-QUAL-01, task 1.4).

Off unless FM2_STAGE_TRACE_DIR names a directory; then every harness process
that imports this module appends JSON lines to <dir>/<program>-<pid>.jsonl:

  {"t": <CLOCK_MONOTONIC ns>, "ev": <name>, ...fields}

All processes of one host share CLOCK_MONOTONIC, so the files merge on one
time axis (stage_report.py). benchmark.py --stage-trace calls enable(). Frequent calls are summed per name instead
(total()) and written once at exit as an "ev": "totals" line. Tracing only
writes these files; it never changes what the harness does or decides.
"""

import atexit
from contextlib import contextmanager
import json
import os
from pathlib import Path
import sys
import threading
import time

_DIR = os.environ.get('FM2_STAGE_TRACE_DIR', '')
ENABLED = bool(_DIR)
_lock = threading.Lock()
_stream = None
_totals = {}


def enable(directory):
    """Turn tracing on for this process and every harness process it starts."""
    global _DIR, ENABLED
    _DIR = str(directory)
    os.environ['FM2_STAGE_TRACE_DIR'] = _DIR
    ENABLED = True


def now():
    return time.monotonic_ns()


def _write(line):
    global _stream
    with _lock:
        if _stream is None:
            directory = Path(_DIR)
            directory.mkdir(parents=True, exist_ok=True)
            name = Path(sys.argv[0]).stem or 'python'
            _stream = (directory / f'{name}-{os.getpid()}.jsonl').open('a', encoding='utf-8', buffering=1)
        _stream.write(line + '\n')


def event(name, t=None, **fields):
    """Record one timestamped event (t defaults to now)."""
    if ENABLED:
        _write(json.dumps({'t': now() if t is None else t, 'ev': name, **fields}, separators=(',', ':'), default=str))


def total(category, name, duration_ns):
    """Add one call of category/name taking duration_ns to the per-process totals."""
    if ENABLED:
        with _lock:
            item = _totals.setdefault(category + '/' + name, [0, 0])
            item[0] += 1
            item[1] += duration_ns


@contextmanager
def span(name, **fields):
    """Record name with its start t and duration dur (ns) around a block."""
    if not ENABLED:
        yield
        return
    started = now()
    try:
        yield
    finally:
        event(name, t=started, dur=now() - started, **fields)


def _timed(owner, attribute, category, key=None, record=None):
    """Replace owner.attribute with a wrapper that adds its duration to the totals.

    key(args, kwargs) names the call within its category; record(name, args,
    kwargs, result, started, duration) may also write a timestamped event.
    """
    original = getattr(owner, attribute)

    def wrapper(*args, **kwargs):
        started = now()
        result = None
        try:
            result = original(*args, **kwargs)
            return result
        finally:
            duration = now() - started
            name = key(args, kwargs) if key else attribute
            total(category, name, duration)
            if record is not None:
                record(name, args, kwargs, result, started, duration)

    wrapper.__wrapped__ = original
    setattr(owner, attribute, wrapper)


# Worker commands on the measured path; each one gets its own event.
WORKER_EVENTS = {'submit', 'candidate', 'pbft_start', 'pbft_timeout', 'network_dispatch', 'network_send'}


def install(role):
    """Instrument this process's harness classes when tracing is on; else do nothing.

    role is 'client' (the benchmark) or 'operator' (operator_wrapper.py).
    """
    if not ENABLED:
        return
    event('process', role=role, argv=sys.argv[1:2])
    import custody
    import local_process
    custody_atomic = custody.atomic_json

    def atomic(path, value):
        started = now()
        try:
            return custody_atomic(path, value)
        finally:
            total('fsync', 'atomic_json/' + Path(path).name.split('.')[0][:40], now() - started)
    custody.atomic_json = atomic
    _timed(custody.LocalRpc, '_exchange', 'evm_rpc',
           key=lambda a, k: a[1]['method'] if isinstance(a[1], dict) else 'batch')
    _timed(custody.Cast, 'run', 'cast_subprocess', key=lambda a, k: str(a[1]) if len(a) > 1 else 'cast')
    _timed(local_process.B3Rpc, '_exchange', 'b3_rpc',
           key=lambda a, k: a[1]['method'] if isinstance(a[1], dict) else 'batch')

    def worker(name, args, kwargs, result, started, duration):
        if name in WORKER_EVENTS:
            fields = {}
            if isinstance(result, dict):
                for field in ('applied', 'code', 'height'):
                    if field in result and not isinstance(result[field], (dict, list)):
                        fields[field] = result[field]
            event('worker', t=started, dur=duration, cmd=name, **fields)
    _timed(local_process.JsonProcess, 'request', 'worker', key=lambda a, k: str(a[1]), record=worker)
    if role == 'client':
        import https_api

        def client(name, args, kwargs, result, started, duration):
            reply = result if isinstance(result, dict) else {}
            event('https', t=started, dur=duration, call=name, code=reply.get('code'),
                  included=bool(reply.get('included')))
        _timed(https_api.HttpsClient, 'request', 'https_client',
               key=lambda a, k: a[1] + ' ' + a[2].split('/')[2], record=client)
        return
    import automatic_batches
    import https_api
    import network_runtime
    # operator_wrapper.py runs as __main__; patch the class that process uses.
    main = sys.modules.get('__main__')
    if main is not None and hasattr(main, 'Operator'):
        operator_class = main.Operator
    else:
        import operator_wrapper
        operator_class = operator_wrapper.Operator

    def spanned(name):
        return lambda _n, args, kwargs, result, started, duration: event(name, t=started, dur=duration)
    _timed(https_api.HttpsGateway, 'handle', 'https_server', record=spanned('https_handle'))
    _timed(automatic_batches.AutomaticBatches, 'enqueue', 'automatic', record=spanned('enqueue'))
    _timed(network_runtime.NetworkRuntime, 'seal', 'runtime', record=spanned('seal'))
    _timed(network_runtime.NetworkRuntime, '_start_batch', 'runtime', record=spanned('start_batch'))
    _timed(network_runtime.NetworkRuntime, 'step', 'runtime')
    _timed(network_runtime.NetworkRuntime, '_flush', 'runtime')
    _timed(operator_class, '_observe', 'operator', record=spanned('observe'))
    # The source and custody re-checks that run before signing or dispatching.
    for name in ('_validate_pending', '_verify_retained', '_validate_proposal'):
        _timed(operator_class, name, 'operator', record=spanned(name.lstrip('_')))
    _timed(network_runtime.NetworkRuntime, '_authorize', 'runtime', record=spanned('authorize'))

    def finished(name, args, kwargs, result, started, duration):
        if isinstance(result, dict) and result.get('applied'):
            event('applied', t=started + duration)
    _timed(network_runtime.NetworkRuntime, '_finish', 'runtime', record=finished)


def _flush_totals():
    if ENABLED and _totals:
        event('totals', calls={key: {'count': value[0], 'ns': value[1]} for key, value in sorted(_totals.items())})


atexit.register(_flush_totals)
