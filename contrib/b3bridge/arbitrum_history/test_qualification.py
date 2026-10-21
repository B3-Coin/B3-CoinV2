#!/usr/bin/env python3
"""Receipt guard negatives; actual disconnected/native runs are a separate gate."""
import copy
from pathlib import Path
import tempfile
import unittest
import xml.etree.ElementTree as ET

from qualify_history import (NATIVE_SUITE, PYTHON_MODULES, PYTHON_COUNTS, IPV4_ROUTE_HEADER,
    MAX_EMPTY_ROUTE_BYTES, QualificationError, clean_environment, native_inventory, source_inventory,
    validate_native_xml, validate_python_receipt, verify_empty_ipv4_routes, write_json)


def xml_report(cases=('valid_commitment', 'reject_omitted_receipt')):
    counters = {'test_cases_passed': str(len(cases)), 'test_cases_passed_with_warnings': '0',
        'test_cases_failed': '0', 'test_cases_skipped': '0', 'test_cases_aborted': '0',
        'test_cases_timed_out': '0', 'test_suites_timed_out': '0'}
    zero = {'result': 'passed', 'assertions_failed': '0', 'warnings_failed': '0', 'expected_failures': '0'}
    document = ET.Element('TestResult')
    master = ET.SubElement(document, 'TestSuite', name='Bitcoin Core Test Suite', **zero, **counters)
    suite = ET.SubElement(master, 'TestSuite', name=NATIVE_SUITE, **zero, **counters)
    for name in cases:
        ET.SubElement(suite, 'TestCase', name=name, **zero)
    return document


def receipt():
    cases = [module + '.Tests.test_synthetic_' + str(index)
             for module, count in PYTHON_COUNTS.items() for index in range(count)]
    return {'format': 'b3-history-python-test-results/1', 'modules': list(PYTHON_MODULES),
        'cases': cases, 'tests_run': len(cases), 'failures': 0, 'errors': 0, 'skipped': 0,
        'expected_failures': 0, 'unexpected_successes': 0, 'successful': True}


class QualificationTests(unittest.TestCase):
    expected = frozenset(('valid_commitment', 'reject_omitted_receipt'))

    def check_xml(self, document):
        return validate_native_xml(ET.tostring(document), self.expected)

    def test_native_exact_nonempty_inventory_passes_with_zero_selected_skips(self):
        result = self.check_xml(xml_report())
        self.assertEqual(result['tests_passed'], 2)
        self.assertEqual(result['tests_skipped'], 0)
        self.assertEqual(set(result['cases']), self.expected)
        self.assertEqual(len(result['xml_sha256']), 64)

    def test_native_missing_duplicate_substituted_and_zero_cases_refuse(self):
        for names in ((), ('valid_commitment',), ('valid_commitment', 'valid_commitment'),
                      ('valid_commitment', 'different_test')):
            with self.subTest(names=names), self.assertRaises(QualificationError):
                self.check_xml(xml_report(names))
        with self.assertRaises(QualificationError):
            validate_native_xml(ET.tostring(xml_report()), frozenset())

    def test_native_skipped_failed_missing_or_warning_counters_refuse(self):
        for field in ('test_cases_passed_with_warnings', 'test_cases_failed', 'test_cases_skipped',
                      'test_cases_aborted', 'test_cases_timed_out', 'test_suites_timed_out'):
            for value in ('1', None):
                document = xml_report()
                selected = document.find('./TestSuite/TestSuite')
                if value is None:
                    selected.attrib.pop(field)
                else:
                    selected.set(field, value)
                with self.subTest(field=field, value=value), self.assertRaises(QualificationError):
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
        for attack in ('other', 'duplicate'):
            document = xml_report()
            if attack == 'other':
                ET.SubElement(document.find('./TestSuite'), 'TestCase', name='unexpected', result='passed')
            else:
                document.find('./TestSuite').append(copy.deepcopy(document.find('./TestSuite/TestSuite')))
            with self.subTest(attack=attack), self.assertRaises(QualificationError):
                self.check_xml(document)

    def test_native_malformed_xml_and_external_declarations_refuse(self):
        for encoded in (b'', b'<TestResult>', b'<Wrong/>', b'<!DOCTYPE x><TestResult/>',
                        b'<!ENTITY x "y"><TestResult/>'):
            with self.subTest(encoded=encoded), self.assertRaises(QualificationError):
                validate_native_xml(encoded, self.expected)

    def test_python_exact_modules_and_successful_count_pass(self):
        self.assertEqual(validate_python_receipt(receipt())['tests_run'], 69)

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
            valid = ('BOOST_FIXTURE_TEST_SUITE(' + NATIVE_SUITE + ', BasicTestingSetup)\n'
                     'BOOST_AUTO_TEST_CASE(first) {}\nBOOST_AUTO_TEST_CASE(second) {}\nBOOST_AUTO_TEST_SUITE_END()\n')
            source.write_text(valid)
            self.assertEqual(native_inventory(source), frozenset(('first', 'second')))
            source.write_text(valid.replace('BOOST_FIXTURE_TEST_SUITE', 'BOOST_AUTO_TEST_SUITE'))
            self.assertEqual(native_inventory(source), frozenset(('first', 'second')))
            for text in ('', valid.replace('second', 'first'), valid.replace(NATIVE_SUITE, 'different')):
                source.write_text(text)
                with self.subTest(text=text), self.assertRaises(QualificationError):
                    native_inventory(source)

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
