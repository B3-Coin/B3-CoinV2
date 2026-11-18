#!/usr/bin/env python3
"""Receipt guard negatives; actual disconnected/native runs are a separate gate."""
import copy
from pathlib import Path
import tempfile
import unittest
import xml.etree.ElementTree as ET

from qualify_history import (NATIVE_SUITE, NATIVE_COUNTS, PYTHON_MODULES, PYTHON_COUNTS, IPV4_ROUTE_HEADER,
    MAX_EMPTY_ROUTE_BYTES, QualificationError, clean_environment, native_inventory, source_inventory,
    validate_native_xml, validate_python_receipt, verify_empty_ipv4_routes, write_json)


EXPECTED = {suite: frozenset('case_' + str(index) for index in range(count))
            for suite, count in NATIVE_COUNTS.items()}


def xml_report(cases=None, changed_suite=NATIVE_SUITE):
    cases_by_suite = {name: sorted(names) for name, names in EXPECTED.items()}
    if cases is not None:
        cases_by_suite[changed_suite] = cases
    def counters(count):
        return {'test_cases_passed': str(count), 'test_cases_passed_with_warnings': '0',
        'test_cases_failed': '0', 'test_cases_skipped': '0', 'test_cases_aborted': '0',
        'test_cases_timed_out': '0', 'test_suites_timed_out': '0'}
    zero = {'result': 'passed', 'assertions_failed': '0', 'warnings_failed': '0', 'expected_failures': '0'}
    document = ET.Element('TestResult')
    master = ET.SubElement(document, 'TestSuite', name='Bitcoin Core Test Suite', **zero,
                           **counters(sum(map(len, cases_by_suite.values()))))
    for name, cases in cases_by_suite.items():
        suite = ET.SubElement(master, 'TestSuite', name=name, **zero, **counters(len(cases)))
        for case in cases:
            ET.SubElement(suite, 'TestCase', name=case, **zero)
    return document


def receipt():
    cases = [module + '.Tests.test_synthetic_' + str(index)
             for module, count in PYTHON_COUNTS.items() for index in range(count)]
    return {'format': 'b3-history-python-test-results/1', 'modules': list(PYTHON_MODULES),
        'cases': cases, 'tests_run': len(cases), 'failures': 0, 'errors': 0, 'skipped': 0,
        'expected_failures': 0, 'unexpected_successes': 0, 'successful': True}


class QualificationTests(unittest.TestCase):
    expected = EXPECTED

    def check_xml(self, document):
        return validate_native_xml(ET.tostring(document), self.expected)

    def test_native_exact_nonempty_inventory_passes_with_zero_selected_skips(self):
        result = self.check_xml(xml_report())
        self.assertEqual(result['tests_passed'], 51)
        self.assertEqual(result['tests_skipped'], 0)
        self.assertEqual(result['suite_names'], list(NATIVE_COUNTS))
        self.assertEqual(set(result['suites']), set(NATIVE_COUNTS))
        for name, count in NATIVE_COUNTS.items():
            self.assertEqual(result['suites'][name]['tests_passed'], count)
            self.assertEqual(set(result['suites'][name]['cases']), self.expected[name])
        self.assertEqual(set(result['cases']), {suite + '.' + case for suite, names in self.expected.items() for case in names})
        self.assertEqual(len(result['xml_sha256']), 64)

    def test_native_missing_duplicate_substituted_and_zero_cases_refuse(self):
        for suite, names in self.expected.items():
            ordered = sorted(names)
            for changed in ((), ordered[:-1], [ordered[0]] * len(ordered), [*ordered[:-1], 'different_test']):
                with self.subTest(suite=suite, names=changed), self.assertRaises(QualificationError):
                    self.check_xml(xml_report(changed, suite))
        for expected in ({}, {NATIVE_SUITE: self.expected[NATIVE_SUITE]},
                         {**self.expected, 'unreviewed_suite': frozenset(('case',))},
                         {**self.expected, NATIVE_SUITE: frozenset(('case',))}):
            with self.subTest(expected=expected), self.assertRaises(QualificationError):
                validate_native_xml(ET.tostring(xml_report()), expected)

    def test_native_skipped_failed_missing_or_warning_counters_refuse(self):
        for field in ('test_cases_passed_with_warnings', 'test_cases_failed', 'test_cases_skipped',
                      'test_cases_aborted', 'test_cases_timed_out', 'test_suites_timed_out'):
            for suite in NATIVE_COUNTS:
                for value in ('1', None):
                    document = xml_report()
                    selected = next(item for item in document.iter('TestSuite') if item.get('name') == suite)
                    if value is None:
                        selected.attrib.pop(field)
                    else:
                        selected.set(field, value)
                    with self.subTest(suite=suite, field=field, value=value), self.assertRaises(QualificationError):
                        self.check_xml(document)

    def test_native_failed_case_master_or_expected_failures_refuse(self):
        for path, field, value in (('./TestSuite', 'result', 'failed'),
                                  ('./TestSuite/TestSuite/TestCase', 'result', 'skipped'),
                                  ('./TestSuite/TestSuite/TestCase', 'expected_failures', '1'),
                                  ('./TestSuite', 'warnings_failed', '1')):
            document = xml_report()
            document.find(path).set(field, value)
            with self.subTest(path=path, field=field), self.assertRaises(QualificationError):
                self.check_xml(document)

    def test_native_other_executed_suite_or_duplicate_selected_suite_refuses(self):
        for name in NATIVE_COUNTS:
            for attack in ('other', 'duplicate', 'missing', 'renamed'):
                document = xml_report()
                master = document.find('./TestSuite')
                selected = next(item for item in master if item.get('name') == name)
                if attack == 'other':
                    ET.SubElement(master, 'TestCase', name='unexpected', result='passed')
                elif attack == 'duplicate':
                    master.append(copy.deepcopy(selected))
                elif attack == 'missing':
                    master.remove(selected)
                else:
                    selected.set('name', 'unreviewed_suite')
                with self.subTest(suite=name, attack=attack), self.assertRaises(QualificationError):
                    self.check_xml(document)

    def test_native_malformed_xml_and_external_declarations_refuse(self):
        for encoded in (b'', b'<TestResult>', b'<Wrong/>', b'<!DOCTYPE x><TestResult/>',
                        b'<!ENTITY x "y"><TestResult/>'):
            with self.subTest(encoded=encoded), self.assertRaises(QualificationError):
                validate_native_xml(encoded, self.expected)

    def test_python_exact_modules_and_successful_count_pass(self):
        self.assertEqual(validate_python_receipt(receipt())['tests_run'], 96)
        self.assertEqual(PYTHON_COUNTS['test_proof_bundle'], 27)

    def test_empty_route_parser_accepts_kernel_variants_and_refuses_routes_unknown_or_oversized_data(self):
        header = '\t'.join(IPV4_ROUTE_HEADER).encode()
        for raw in (b'', b' \t\r\n', header, header + b'\n', b'  ' + b' '.join(header.split()) + b'\r\n'):
            with self.subTest(valid=raw):
                self.assertIsNone(verify_empty_ipv4_routes(raw))
        route = b'lo\t00000000\t00000000\t0001\t0\t0\t0\t00000000\t0\t0\t0'
        for raw in (route, header + b'\n' + route + b'\n', b'unknown header\n',
                    header.replace(b'Gateway', b'Wrong'), header + b'\n' + header,
                    header + b'\n0', header + b'\0', b'\xff',
                    b' ' * (MAX_EMPTY_ROUTE_BYTES + 1), None, ''):
            with self.subTest(invalid=raw), self.assertRaises(QualificationError):
                verify_empty_ipv4_routes(raw)

    def test_python_empty_skipped_failed_inconsistent_and_duplicate_cases_refuse(self):
        for attack in ('empty', 'skip', 'error', 'count', 'duplicate', 'module', 'unselected', 'expected', 'bool',
                       'preflight', 'unhashable', 'nonstring', 'malformed_id'):
            value = receipt()
            if attack == 'empty': value['cases'], value['tests_run'] = [], 0
            elif attack == 'skip': value['skipped'] = 1
            elif attack == 'error': value['errors'] = 1
            elif attack == 'count': value['tests_run'] += 1
            elif attack == 'duplicate': value['cases'][1] = value['cases'][0]
            elif attack == 'module': value['modules'].pop()
            elif attack == 'unselected': value['cases'][0] = 'different_module.Tests.test_bad'
            elif attack == 'expected': value['expected_failures'] = 1
            elif attack == 'bool': value['failures'] = False
            elif attack == 'preflight': value['cases'].pop(); value['tests_run'] -= 1
            elif attack == 'unhashable': value['cases'][0] = {'not': 'a test identity'}
            elif attack == 'nonstring': value['cases'][0] = 9
            elif attack == 'malformed_id': value['cases'][0] = 'test_history_store.\nmisleading'
            with self.subTest(attack=attack), self.assertRaises(QualificationError):
                validate_python_receipt(value)

    def test_native_source_inventory_refuses_empty_duplicate_or_other_suite(self):
        with tempfile.TemporaryDirectory() as temporary:
            source = Path(temporary) / 'native.cpp'
            for name in NATIVE_COUNTS:
                valid = ('BOOST_FIXTURE_TEST_SUITE(' + name + ', BasicTestingSetup)\n'
                         'BOOST_AUTO_TEST_CASE(first) {}\nBOOST_AUTO_TEST_CASE(second) {}\nBOOST_AUTO_TEST_SUITE_END()\n')
                source.write_text(valid)
                self.assertEqual(native_inventory(source, name), frozenset(('first', 'second')))
                source.write_text(valid.replace('BOOST_FIXTURE_TEST_SUITE', 'BOOST_AUTO_TEST_SUITE'))
                self.assertEqual(native_inventory(source, name), frozenset(('first', 'second')))
                for text in ('', valid.replace('second', 'first'), valid.replace(name, 'different')):
                    source.write_text(text)
                    with self.subTest(suite=name, text=text), self.assertRaises(QualificationError):
                        native_inventory(source, name)

    def test_source_inventory_detects_mutation_and_covers_history_and_native_files(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            (root / 'src/bridge').mkdir(parents=True)
            history = root / 'contrib/b3bridge/arbitrum_history'
            history.mkdir(parents=True)
            (root / 'src/bridge/proof.h').write_text('first')
            (history / 'history_store.py').write_text('source')
            first = source_inventory(root)
            self.assertEqual(len(first['files']), 2)
            (root / 'src/bridge/proof.h').write_text('second')
            self.assertNotEqual(first['sha256'], source_inventory(root)['sha256'])
            self.assertEqual(source_inventory(root), source_inventory(root))

    def test_clean_child_environment_contains_no_loader_proxy_rpc_or_test_overrides(self):
        environment = clean_environment(Path('/private/evidence/home'))
        self.assertEqual(set(environment), {'PATH', 'HOME', 'LANG', 'LC_ALL', 'TZ'})
        self.assertEqual(environment['PATH'], '/usr/bin:/bin')
        self.assertFalse(any(key.startswith(('LD_', 'PYTHON', 'BOOST_', 'B3_')) for key in environment))

    def test_receipt_writer_does_not_replace_prior_qualification(self):
        with tempfile.TemporaryDirectory() as temporary:
            target = Path(temporary) / 'report.json'
            write_json(target, {'passed': False})
            with self.assertRaises(FileExistsError):
                write_json(target, {'passed': True})
            self.assertIn('false', target.read_text())


if __name__ == '__main__':
    unittest.main(verbosity=2)
