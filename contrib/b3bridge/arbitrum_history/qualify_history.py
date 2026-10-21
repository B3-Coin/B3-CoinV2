#!/usr/bin/env python3
"""Qualify offline proof components; never activate an Arbitrum source.

The native suite uses synthetic adversarial confirmed-proof fixtures inside a
fresh, disconnected Linux network namespace. Raw history transport tests have
their own scope. Neither result makes an RPC-seeded token fork RPC-independent.
"""
import argparse
import errno
import hashlib
import json
import os
from pathlib import Path
import re
import signal
import socket
import struct
import subprocess
import sys
import unittest
import xml.etree.ElementTree as ET

FORMAT = 'b3-arbitrum-history-component-qualification/1'
NATIVE_SUITE = 'bridge_arbitrum_source_tests'
PYTHON_MODULES = ('test_history_store', 'test_history_transport', 'test_feed_history', 'test_node_preflight',
                  'test_qualification')
PYTHON_COUNTS = {'test_history_store': 18, 'test_history_transport': 19, 'test_feed_history': 9,
                 'test_node_preflight': 10, 'test_qualification': 13}
MAX_REPORT_BYTES = 8 * 1024 * 1024
MAX_LOG_BYTES = 32 * 1024 * 1024
MAX_EMPTY_ROUTE_BYTES = 1024
IPV4_ROUTE_HEADER = ('Iface', 'Destination', 'Gateway', 'Flags', 'RefCnt', 'Use', 'Metric',
                     'Mask', 'MTU', 'Window', 'IRTT')


class QualificationError(ValueError):
    pass


def canonical(value):
    return json.dumps(value, sort_keys=True, separators=(',', ':'), allow_nan=False)


def read_bounded(path, maximum=MAX_REPORT_BYTES):
    path = Path(path)
    if path.is_symlink() or not path.is_file():
        raise QualificationError('required regular artifact is absent or aliased')
    with path.open('rb') as stream:
        data = stream.read(maximum + 1)
    if len(data) > maximum:
        raise QualificationError('artifact exceeds qualification bound')
    return data


def file_hash(path):
    path = Path(path)
    if path.is_symlink() or not path.is_file():
        raise QualificationError('pinned source/tool must be a regular non-symlink file')
    digest, total = hashlib.sha256(), 0
    with path.open('rb') as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b''):
            total += len(block)
            if total > 1024 * 1024 * 1024:
                raise QualificationError('individual pinned file exceeds bound')
            digest.update(block)
    return digest.hexdigest()


def write_json(path, value):
    # The parent creates a fresh private evidence directory; never replace a
    # prior run/report or repair a source/journal in this component qualifier.
    data = (canonical(value) + '\n').encode()
    if len(data) > MAX_REPORT_BYTES:
        raise QualificationError('result exceeds qualification bound')
    with Path(path).open('xb') as stream:
        stream.write(data)
        stream.flush()
        os.fsync(stream.fileno())


def source_inventory(root):
    root = Path(root).absolute()
    selected = []
    for directory in (root / 'src', root / 'contrib/b3bridge/arbitrum_history'):
        if not directory.is_dir() or directory.is_symlink():
            raise QualificationError('required source scope is absent or aliased')
        for current, directories, files in os.walk(directory, followlinks=False):
            if any((Path(current) / item).is_symlink() for item in directories):
                raise QualificationError('source scope traverses an aliased directory')
            directories[:] = sorted(item for item in directories if item != '__pycache__')
            for name in sorted(files):
                path = Path(current) / name
                if path.suffix != '.pyc':
                    selected.append(path)
                    if len(selected) > 20000:
                        raise QualificationError('source inventory exceeds bound')
    if not selected:
        raise QualificationError('source inventory is empty')
    entries = {path.relative_to(root).as_posix(): file_hash(path) for path in sorted(selected)}
    return {'files': entries, 'sha256': hashlib.sha256(canonical(entries).encode()).hexdigest()}


def native_inventory(source):
    text = read_bounded(source).decode('utf-8')
    suites = re.findall(r'BOOST_(?:AUTO|FIXTURE)_TEST_SUITE\s*\(\s*([A-Za-z_][A-Za-z0-9_]*)', text)
    cases = re.findall(r'BOOST_(?:AUTO|FIXTURE)_TEST_CASE\s*\(\s*([A-Za-z_][A-Za-z0-9_]*)', text)
    if suites != [NATIVE_SUITE] or not cases or len(set(cases)) != len(cases):
        raise QualificationError('native source has an empty/ambiguous suite inventory')
    return frozenset(cases)


def validate_native_xml(raw, expected):
    if not expected or len(raw) > MAX_REPORT_BYTES or b'<!DOCTYPE' in raw or b'<!ENTITY' in raw:
        raise QualificationError('native report is empty, unbounded or has external declarations')
    try:
        document = ET.fromstring(raw)
    except ET.ParseError as error:
        raise QualificationError('native XML is malformed') from error
    masters = document.findall('TestSuite')
    if document.tag != 'TestResult' or len(masters) != 1 or masters[0].get('result') != 'passed':
        raise QualificationError('native master result is absent or failed')
    selected = [item for item in document.iter('TestSuite') if item.get('name') == NATIVE_SUITE]
    if len(selected) != 1:
        raise QualificationError('native selected suite is absent or duplicated')
    suite = selected[0]
    cases = list(suite.iter('TestCase'))
    names = [item.get('name') for item in cases]
    if not names or len(set(names)) != len(names) or set(names) != set(expected):
        raise QualificationError('native executed inventory differs from pinned source')
    if suite.get('test_cases_passed') != str(len(expected)):
        raise QualificationError('native successful case count differs from inventory')
    for key in ('test_cases_passed_with_warnings', 'test_cases_failed', 'test_cases_skipped',
                'test_cases_aborted', 'test_cases_timed_out', 'test_suites_timed_out'):
        if suite.get(key) != '0':
            raise QualificationError('native suite has skipped, failed or ambiguous counters')
    for row in [masters[0], *suite.iter()]:
        if row.tag not in ('TestSuite', 'TestCase') or row.get('result') != 'passed':
            raise QualificationError('native selected case/suite did not pass')
        if any(row.get(key) != '0' for key in ('assertions_failed', 'warnings_failed', 'expected_failures')):
            raise QualificationError('native report contains failed or ambiguous assertions')
    if any(row.get('result') != 'skipped' for row in document.iter('TestCase') if row not in cases):
        raise QualificationError('native unselected tests executed outside exact filter')
    return {'suite': NATIVE_SUITE, 'tests_passed': len(cases), 'tests_skipped': 0,
            'cases': sorted(names), 'xml_sha256': hashlib.sha256(raw).hexdigest()}


def validate_python_receipt(value):
    if (not isinstance(value, dict) or set(value) != {'format', 'modules', 'cases', 'tests_run',
            'failures', 'errors', 'skipped', 'expected_failures', 'unexpected_successes', 'successful'} or
            value['format'] != 'b3-history-python-test-results/1' or value['modules'] != list(PYTHON_MODULES) or
            type(value['tests_run']) is not int or value['tests_run'] <= 0 or
            not isinstance(value['cases'], list) or value['tests_run'] != len(value['cases']) or
            value['successful'] is not True):
        raise QualificationError('Python test receipt is empty, inconsistent or unknown')
    if (any(not isinstance(case, str) or not re.fullmatch(r'[A-Za-z0-9_.]{1,512}', case)
            for case in value['cases']) or len(set(value['cases'])) != len(value['cases'])):
        raise QualificationError('Python test identities are malformed or duplicated')
    if any(type(value[key]) is not int or value[key] != 0 for key in
           ('failures', 'errors', 'skipped', 'expected_failures', 'unexpected_successes')):
        raise QualificationError('Python suite failed or skipped tests')
    for module in PYTHON_MODULES:
        if not any(isinstance(case, str) and case.startswith(module + '.') for case in value['cases']):
            raise QualificationError('a required Python module executed no tests')
    if any(sum(case.startswith(module + '.') for case in value['cases']) != count
           for module, count in PYTHON_COUNTS.items()):
        raise QualificationError('Python module did not execute its exact reviewed case count')
    if any(not isinstance(case, str) or not any(case.startswith(module + '.') for module in PYTHON_MODULES)
           for case in value['cases']):
        raise QualificationError('Python receipt contains an unselected test')
    return value


def verify_empty_ipv4_routes(raw):
    # Linux/WSL may represent an empty table as zero bytes or a header only.
    # Every nonempty data/unknown line is a refusal; never infer an empty table
    # from a malformed/truncated route record or an oversized proc response.
    if type(raw) is not bytes or len(raw) > MAX_EMPTY_ROUTE_BYTES:
        raise QualificationError('native namespace route table exceeds its exact bound')
    try:
        text = raw.decode('ascii')
    except UnicodeError as error:
        raise QualificationError('native namespace route table is not ASCII') from error
    if not text.strip():
        return
    lines = text.strip().splitlines()
    if len(lines) != 1 or tuple(re.split(r'[ \t]+', lines[0].strip())) != IPV4_ROUTE_HEADER:
        raise QualificationError('native namespace has an IPv4 route or unknown table header')


def disconnected_namespace(expected_parent):
    if sys.platform != 'linux':
        raise QualificationError('native disconnected qualification requires Linux')
    own = os.readlink('/proc/self/ns/net')
    if own == expected_parent or not re.fullmatch(r'net:\[[0-9]+\]', own):
        raise QualificationError('native verifier did not enter a fresh network namespace')
    if {name for _, name in socket.if_nameindex()} != {'lo'}:
        raise QualificationError('native namespace has a non-loopback network interface')
    # A reused sysfs mount can still represent the parent's namespace. Query
    # the new namespace's socket directly (Linux SIOCGIFFLAGS), without mounts.
    import fcntl
    with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as interface:
        reply = fcntl.ioctl(interface.fileno(), 0x8913, struct.pack('256s', b'lo'))
    if len(reply) < 18 or reply[:16].split(b'\0', 1)[0] != b'lo':
        raise QualificationError('namespace-local loopback flags are unavailable')
    flags = struct.unpack_from('H', reply, 16)[0]
    if flags & 1:
        raise QualificationError('native namespace loopback is active')
    verify_empty_ipv4_routes(read_bounded('/proc/net/route', MAX_EMPTY_ROUTE_BYTES))
    failures = []
    allowed = {errno.ENETUNREACH, errno.EHOSTUNREACH, errno.ENETDOWN, errno.EAFNOSUPPORT,
               errno.EPROTONOSUPPORT, errno.EADDRNOTAVAIL}
    for family, destination in ((socket.AF_INET, ('198.18.0.1', 9)),
                                (socket.AF_INET6, ('2001:db8::1', 9))):
        for style in (socket.SOCK_STREAM, socket.SOCK_DGRAM):
            handle = None
            try:
                handle = socket.socket(family, style)
                handle.settimeout(.5)
                handle.connect(destination)
            except OSError as error:
                if error.errno not in allowed:
                    raise QualificationError('network canary did not fail with a disconnected-kernel error') from error
                failures.append({'family': 'IPv4' if family == socket.AF_INET else 'IPv6',
                                 'transport': 'TCP' if style == socket.SOCK_STREAM else 'UDP', 'errno': error.errno})
            else:
                raise QualificationError('network canary unexpectedly reached a destination')
            finally:
                if handle is not None:
                    handle.close()
    return {'format': 'b3-disconnected-native-namespace/1', 'namespace': own,
            'different_from_parent': True, 'only_interface': 'lo', 'loopback_up': False,
            'ipv4_routes': 0, 'canaries': failures}


def clean_environment(home):
    return {'PATH': '/usr/bin:/bin', 'HOME': str(home), 'LANG': 'C', 'LC_ALL': 'C', 'TZ': 'UTC'}


def run_process(command, log, *, cwd, environment, timeout):
    def limits():
        import resource
        resource.setrlimit(resource.RLIMIT_FSIZE, (MAX_LOG_BYTES, MAX_LOG_BYTES))
    with Path(log).open('xb') as stream:
        process = subprocess.Popen(command, cwd=cwd, env=environment, stdin=subprocess.DEVNULL,
            stdout=stream, stderr=subprocess.STDOUT, close_fds=True, start_new_session=True, preexec_fn=limits)
        try:
            code = process.wait(timeout=timeout)
        except BaseException:
            try:
                os.killpg(process.pid, signal.SIGKILL)
            except ProcessLookupError:
                pass
            process.wait()
            raise
    if Path(log).stat().st_size > MAX_LOG_BYTES:
        raise QualificationError('process diagnostics exceed bound')
    if code != 0:
        raise QualificationError('required qualification process failed')


def _cases(suite):
    for item in suite:
        if isinstance(item, unittest.TestSuite):
            yield from _cases(item)
        else:
            yield item.id()


def child_python(directory, report):
    sys.path.insert(0, str(Path(directory).absolute()))
    suite = unittest.defaultTestLoader.loadTestsFromNames(PYTHON_MODULES)
    names = sorted(_cases(suite))
    result = unittest.TextTestRunner(verbosity=2).run(suite)
    receipt = {'format': 'b3-history-python-test-results/1', 'modules': list(PYTHON_MODULES),
        'cases': names, 'tests_run': result.testsRun, 'failures': len(result.failures), 'errors': len(result.errors),
        'skipped': len(result.skipped), 'expected_failures': len(result.expectedFailures),
        'unexpected_successes': len(result.unexpectedSuccesses), 'successful': result.wasSuccessful()}
    write_json(report, receipt)
    validate_python_receipt(receipt)


def qualify(args):
    if sys.platform != 'linux':
        raise QualificationError('actual component qualification requires Linux network namespaces')
    os.umask(0o077)
    source = Path(args.source_root).absolute()
    directory = source / 'contrib/b3bridge/arbitrum_history'
    evidence = Path(args.evidence_dir).absolute()
    evidence.mkdir(mode=0o700, parents=False, exist_ok=False)
    # Invoke resolved files, so a distribution's python3 symlink is resolved
    # once rather than reinterpreted between hashing and execution.
    tools = {'python': Path(args.python).resolve(strict=True), 'test_binary': Path(args.test_binary).resolve(strict=True),
             'unshare': Path(args.unshare).resolve(strict=True)}
    before_tools = {key: file_hash(value) for key, value in tools.items()}
    before_source = source_inventory(source)
    expected = native_inventory(source / 'src/test/bridge_arbitrum_source_tests.cpp')
    home = evidence / 'home'
    home.mkdir(mode=0o700)
    environment = clean_environment(home)
    script = directory / 'qualify_history.py'
    python_receipt = evidence / 'python-results.json'
    run_process([str(tools['python']), '-I', '-B', str(script), '--child-python', str(directory), str(python_receipt)],
                evidence / 'python-tests-private.log', cwd=source, environment=environment, timeout=args.timeout)
    python_result = validate_python_receipt(json.loads(read_bounded(python_receipt)))
    native_xml = evidence / 'native-results.xml'
    namespace_receipt = evidence / 'namespace-results.json'
    run_process([str(tools['unshare']), '--user', '--map-root-user', '--net', str(tools['python']), '-I', '-B',
                 str(script), '--child-native', os.readlink('/proc/self/ns/net'), str(namespace_receipt),
                 str(tools['test_binary']), str(native_xml)], evidence / 'native-tests-private.log',
                cwd=source, environment=environment, timeout=args.timeout)
    native_result = validate_native_xml(read_bounded(native_xml), expected)
    namespace_result = json.loads(read_bounded(namespace_receipt))
    if (namespace_result.get('different_from_parent') is not True or namespace_result.get('loopback_up') is not False or
            namespace_result.get('ipv4_routes') != 0 or len(namespace_result.get('canaries', [])) != 4 or
            namespace_result.get('same_after_native') is not True or len(namespace_result.get('post_run_canaries', [])) != 4):
        raise QualificationError('disconnected native namespace receipt is incomplete')
    if before_tools != {key: file_hash(value) for key, value in tools.items()} or before_source != source_inventory(source):
        raise QualificationError('pinned source or executable changed during qualification')
    report = {'format': FORMAT, 'component_checks_passed': True, 'mainnet_enabled': False, 'writes_enabled': False,
        'arbitrum_source_acceptance_qualified': False, 'no_rpc_end_to_end_qualified': False,
        'receipt_transaction_identity_qualified': False, 'native_operator_handoff_qualified': False,
        'live_history_acquisition_qualified': False, 'production_activation': False,
        'source_sha256': before_source['sha256'], 'source_files': before_source['files'], 'tool_sha256': before_tools,
        'source_scope': ['src/', 'contrib/b3bridge/arbitrum_history/'],
        'python_history_tests': python_result, 'native_confirmed_proof_tests': native_result,
        'python_tests_by_module': PYTHON_COUNTS,
        'native_network_namespace': namespace_result,
        'scope': 'Synthetic adversarial native confirmed-proof fixtures without network access; raw-object history transport tests separately scoped',
        'rpc_seeded_usdt0_fork_is_rpc_free': False,
        'limitations': ['Compiled deployment registry and operator source activation remain separately gated',
            'CAS hashes and peer provenance do not establish chain authority',
            'No maintained Nitro replay, complete live Arbitrum history availability or fast sequencer finality is qualified',
            'Synthetic fixtures bind complete transaction bodies and receipt indices to their roots; live authenticated transaction identity and native operator fact handoff remain unqualified',
            'The independent real-token fork uses RPC-derived state and retains its separate provider finality policy'],
        'artifact_sha256': {name: file_hash(evidence / name) for name in
            ('python-results.json', 'python-tests-private.log', 'native-results.xml',
             'native-tests-private.log', 'namespace-results.json')}}
    write_json(evidence / 'history-component-report.json', report)
    return report


def main(argv=None):
    argv = sys.argv[1:] if argv is None else argv
    if argv[:1] == ['--child-python'] and len(argv) == 3:
        child_python(argv[1], argv[2])
        return 0
    if argv[:1] == ['--child-native'] and len(argv) == 5:
        namespace = disconnected_namespace(argv[1])
        command = [argv[3], '--run_test=' + NATIVE_SUITE, '--report_format=XML', '--report_level=detailed',
                   '--report_sink=' + argv[4], '--log_level=test_suite']
        result = subprocess.run(command, stdin=subprocess.DEVNULL, close_fds=True)
        if result.returncode != 0:
            raise QualificationError('native proof suite failed inside disconnected namespace')
        after = disconnected_namespace(argv[1])
        if after['namespace'] != namespace['namespace']:
            raise QualificationError('native verifier namespace changed during execution')
        namespace['same_after_native'] = True
        namespace['post_run_canaries'] = after['canaries']
        write_json(argv[2], namespace)
        return 0
    parser = argparse.ArgumentParser(description=__doc__)
    for flag in ('source-root', 'evidence-dir', 'test-binary', 'python', 'unshare'):
        parser.add_argument('--' + flag, required=True)
    parser.add_argument('--timeout', type=int, default=300)
    args = parser.parse_args(argv)
    if not 1 <= args.timeout <= 1800:
        raise QualificationError('qualification timeout is outside its bound')
    report = qualify(args)
    print(canonical({key: report[key] for key in ('format', 'component_checks_passed', 'mainnet_enabled',
        'arbitrum_source_acceptance_qualified', 'no_rpc_end_to_end_qualified')}))
    return 0


if __name__ == '__main__':
    try:
        raise SystemExit(main())
    except (QualificationError, OSError, subprocess.SubprocessError, ValueError):
        print('history component qualification refused; inspect private bounded diagnostics', file=sys.stderr)
        raise SystemExit(1)
