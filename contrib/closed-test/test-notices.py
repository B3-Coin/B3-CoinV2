#!/usr/bin/env python3
"""Synthetic notice transport checks; no credentials or network required."""
import importlib.util
import io
from pathlib import Path
import ssl
import unittest
from unittest import mock
import urllib.error
import urllib.request
import urllib.response

HERE = Path(__file__).resolve().parent
SPEC = importlib.util.spec_from_file_location('notice_collector', HERE / 'collect-notices.py')
NOTICES = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(NOTICES)

TOKEN = 'synthetic_notice_test_marker_not_a_credential'
API = 'https://api.github.com/repos/qt/qtbase/git/trees/v6.11.1?recursive=1'
RAW = 'https://raw.githubusercontent.com/qt/qtbase/v6.11.1/LICENSES/LGPL-3.0-only.txt'


class Response(io.BytesIO):
    def __init__(self, data):
        super().__init__(data)
        self.requested = []

    def read(self, size=-1):
        self.requested.append(size)
        return super().read(size)


class NoticeTransportTests(unittest.TestCase):
    def call(self, url=API, data=b'public notice', maximum=1024, token=TOKEN):
        response = Response(data)
        context = ssl.create_default_context()
        opener = mock.Mock()
        opener.open.return_value = response
        with mock.patch.dict(NOTICES.os.environ, {'B3_NOTICE_GITHUB_TOKEN': token}), \
             mock.patch.object(NOTICES.ssl, 'create_default_context', return_value=context), \
             mock.patch.object(NOTICES.urllib.request, 'build_opener', return_value=opener) as build, \
             mock.patch.object(NOTICES.urllib.request, 'urlopen', return_value=response) as plain:
            result = NOTICES.get(url, maximum)
        return result, response, context, opener, build, plain

    def test_exact_public_tree_requests_get_scoped_authorization(self):
        for module in ('qtbase', 'qtsvg'):
            for version in ('6.8.3', '6.11.1'):
                url = f'https://api.github.com/repos/qt/{module}/git/trees/v{version}?recursive=1'
                with self.subTest(url=url):
                    result, response, context, opener, build, plain = self.call(url)
                    self.assertEqual(result, b'public notice')
                    request = opener.open.call_args.args[0]
                    self.assertEqual(request.full_url, url)
                    self.assertEqual(request.get_header('Authorization'), 'Bearer ' + TOKEN)
                    self.assertEqual(opener.open.call_args.kwargs, {'timeout': 30})
                    handlers = build.call_args.args
                    self.assertTrue(any(isinstance(h, NOTICES.RefuseCredentialRedirect) for h in handlers))
                    https = next(h for h in handlers if isinstance(h, urllib.request.HTTPSHandler))
                    self.assertIs(https._context, context)
                    plain.assert_not_called()
                    self.assertEqual(response.requested, [1025])

    def test_raw_notice_download_never_gets_authorization(self):
        result, _, _, opener, build, plain = self.call(RAW)
        self.assertEqual(result, b'public notice')
        self.assertIsNone(plain.call_args.args[0].get_header('Authorization'))
        build.assert_not_called()
        opener.open.assert_not_called()

    def test_other_authorities_ports_and_api_paths_never_get_authorization(self):
        urls = (
            API.replace('api.github.com', 'api.github.com.evil.invalid'),
            API.replace('api.github.com', 'api.github.com:443'),
            API.replace('api.github.com', 'api.github.com:8443'),
            API.replace('api.github.com', 'user@api.github.com'),
            API.replace('https:', 'http:'),
            API.replace('api.github.com', 'API.GITHUB.COM'),
            API.replace('/qt/qtbase/', '/other/qtbase/'),
            API.replace('/qtbase/', '/qtdeclarative/'),
            API.replace('v6.11.1', 'master'),
            API.replace('v6.11.1', 'v7.0.0'),
            API.replace('?recursive=1', '?recursive=1&extra=1'),
            API + '#fragment',
            API.replace('/git/trees/', '/git/%74rees/'),
            'https://example.invalid/notice.txt',
        )
        for url in urls:
            with self.subTest(url=url):
                _, _, _, opener, build, plain = self.call(url)
                self.assertIsNone(plain.call_args.args[0].get_header('Authorization'))
                build.assert_not_called()
                opener.open.assert_not_called()

    def test_absent_or_empty_token_preserves_public_fetch(self):
        for token in ('',):
            _, _, context, opener, build, plain = self.call(token=token)
            self.assertIsNone(plain.call_args.args[0].get_header('Authorization'))
            self.assertEqual(plain.call_args.kwargs, {'context': context, 'timeout': 30})
            build.assert_not_called()
            opener.open.assert_not_called()
        with mock.patch.dict(NOTICES.os.environ, {}, clear=True), \
             mock.patch.object(NOTICES.urllib.request, 'urlopen', return_value=Response(b'ok')) as plain:
            self.assertEqual(NOTICES.get(API, 1024), b'ok')
            self.assertIsNone(plain.call_args.args[0].get_header('Authorization'))

    def test_all_credentialed_redirects_refused_before_second_request(self):
        request = urllib.request.Request(API, headers={'Authorization': 'Bearer ' + TOKEN})
        handler = NOTICES.RefuseCredentialRedirect()
        for status in (301, 302, 303, 307, 308):
            for destination in (API, RAW, 'https://example.invalid/steal',
                                'http://api.github.com/repos/qt/qtbase/git/trees/v6.11.1?recursive=1'):
                with self.subTest(status=status, destination=destination), \
                     self.assertRaisesRegex(urllib.error.HTTPError, 'refuse redirects') as caught:
                    handler.redirect_request(request, None, status, 'redirect', {}, destination)
                caught.exception.close()

    def test_actual_opener_handler_chain_refuses_redirects(self):
        class SyntheticHTTPS(urllib.request.HTTPSHandler):
            def __init__(self, status, destination):
                super().__init__()
                self.status, self.destination, self.requests = status, destination, []

            def https_open(self, request):
                self.requests.append(request)
                response = urllib.response.addinfourl(io.BytesIO(b''),
                    {'location': self.destination}, request.full_url, self.status)
                response.msg = 'synthetic redirect'
                return response

        for status in (300, 301, 302, 303, 304, 305, 306, 307, 308, 399):
            for destination in (API, RAW, 'https://example.invalid/steal'):
                with self.subTest(status=status, destination=destination):
                    transport = SyntheticHTTPS(status, destination)
                    opener = urllib.request.build_opener(transport, NOTICES.RefuseCredentialRedirect())
                    request = urllib.request.Request(API, headers={'Authorization': 'Bearer ' + TOKEN})
                    with self.assertRaises(urllib.error.HTTPError) as caught:
                        opener.open(request, timeout=30)
                    caught.exception.close()
                    self.assertEqual([r.full_url for r in transport.requests], [API])

    def test_authenticated_failure_does_not_fall_back_to_unauthenticated(self):
        opener = mock.Mock()
        error = urllib.error.HTTPError(API, 403, 'synthetic rate refusal', {}, None)
        opener.open.side_effect = error
        with mock.patch.dict(NOTICES.os.environ, {'B3_NOTICE_GITHUB_TOKEN': TOKEN}), \
             mock.patch.object(NOTICES.urllib.request, 'build_opener', return_value=opener), \
             mock.patch.object(NOTICES.urllib.request, 'urlopen') as plain, \
             self.assertRaises(urllib.error.HTTPError):
            NOTICES.get(API, 1024)
        error.close()
        plain.assert_not_called()

    def test_tls_failure_does_not_downgrade_or_retry(self):
        opener = mock.Mock()
        opener.open.side_effect = ssl.SSLCertVerificationError('synthetic invalid certificate')
        with mock.patch.dict(NOTICES.os.environ, {'B3_NOTICE_GITHUB_TOKEN': TOKEN}), \
             mock.patch.object(NOTICES.urllib.request, 'build_opener', return_value=opener), \
             mock.patch.object(NOTICES.urllib.request, 'urlopen') as plain, \
             self.assertRaises(ssl.SSLCertVerificationError):
            NOTICES.get(API, 1024)
        self.assertEqual(opener.open.call_count, 1)
        plain.assert_not_called()

    def test_response_read_remains_bounded(self):
        for url in (API, RAW):
            with self.subTest(url=url), self.assertRaisesRegex(RuntimeError, 'exceeds bound'):
                self.call(url, data=b'x' * 9, maximum=8)
            result, response, *_ = self.call(url, data=b'x' * 8, maximum=8)
            self.assertEqual(result, b'x' * 8)
            self.assertEqual(response.requested, [9])

    def test_both_paths_keep_tls_hostname_and_certificate_verification(self):
        for url in (API, RAW):
            with self.subTest(url=url):
                _, _, context, _, _, _ = self.call(url)
                self.assertTrue(context.check_hostname)
                self.assertEqual(context.verify_mode, ssl.CERT_REQUIRED)

    def test_malformed_token_refused_without_echo_or_request(self):
        for token in ('synthetic\nheader', 'synthetic\rheader', 'x' * 4097):
            with self.subTest(length=len(token)), \
                 mock.patch.dict(NOTICES.os.environ, {'B3_NOTICE_GITHUB_TOKEN': token}), \
                 mock.patch.object(NOTICES.urllib.request, 'build_opener') as build, \
                 mock.patch.object(NOTICES.urllib.request, 'urlopen') as plain, \
                 self.assertRaisesRegex(RuntimeError, '^Malformed CI notice token$'):
                NOTICES.get(API, 1024)
            build.assert_not_called()
            plain.assert_not_called()


if __name__ == '__main__':
    unittest.main()
