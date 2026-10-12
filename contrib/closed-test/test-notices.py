#!/usr/bin/env python3
"""Synthetic notice transport checks; no credentials or network required."""
import importlib.util
import hashlib
import io
import json
from pathlib import Path
import ssl
import tarfile
import tempfile
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
            'https://raw.githubusercontent.com/Homebrew/homebrew-core/' + '1' * 40 + '/Formula/g/glib.rb',
            'https://download.gnome.org/sources/glib/2.88/glib-2.88.3.tar.xz',
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


class GlibSourceTests(unittest.TestCase):
    VERSION = '2.88.3'
    URL = 'https://download.gnome.org/sources/glib/2.88/glib-2.88.3.tar.xz'
    LICENSE = b'Synthetic license test data, not a distributed license.\n'

    def archive(self, rows=None):
        rows = rows if rows is not None else [
            ('glib-2.88.3/LICENSES/LGPL-2.1-or-later.txt', self.LICENSE, None),
            ('glib-2.88.3/COPYING', b'', 'LICENSES/LGPL-2.1-or-later.txt'),
            ('glib-2.88.3/subprojects/example/LICENSES/MIT.txt', b'Synthetic MIT fixture', None),
            ('glib-2.88.3/unrelated.c', b'not executed', None)]
        output = io.BytesIO()
        with tarfile.open(fileobj=output, mode='w:xz') as archive:
            for name, data, target in rows:
                member = tarfile.TarInfo(name)
                if target:
                    member.type, member.linkname = tarfile.SYMTYPE, target
                else:
                    member.size = len(data)
                archive.addfile(member, io.BytesIO(data) if not target else None)
        return output.getvalue()

    def installation(self, directory, *, version='2.88.3', receipt_version='2.88.3', url=None, sha='a' * 64):
        installed = Path(directory) / 'glib-2.88.3'
        (installed / '.brew').mkdir(parents=True)
        (installed / 'lib/pkgconfig').mkdir(parents=True)
        (installed / '.brew/glib.rb').write_text(
            f'class Glib < Formula\n  url "{url or self.URL}"\n  sha256 "{sha}"\n  depends_on "pcre2"\nend\n')
        (installed / 'INSTALL_RECEIPT.json').write_text(json.dumps({'source': {'versions': {'stable': receipt_version}}}))
        (installed / 'lib/pkgconfig/glib-2.0.pc').write_text(f'Version: {version}\n')
        return installed

    def test_license_and_copying_symlink_are_read_without_extraction(self):
        rows = NOTICES.glib_archive_notices(self.archive(), self.VERSION)
        self.assertEqual(rows['COPYING'], (self.LICENSE, {'source_symlink_target': 'LICENSES/LGPL-2.1-or-later.txt'}))
        self.assertIn('subprojects/example/LICENSES/MIT.txt', rows)
        self.assertNotIn('unrelated.c', rows)

    def test_wrong_root_traversal_duplicate_or_external_copying_refused(self):
        for rows in (
                [('other/LICENSES/LGPL-2.1-or-later.txt', self.LICENSE, None)],
                [('glib-2.88.3/../escaped', b'x', None)],
                [('glib-2.88.3/COPYING', b'', '/outside')],
                [('glib-2.88.3/COPYING', b'', '../outside')],
                [('glib-2.88.3/COPYING', b'x', None)] * 2):
            with self.subTest(rows=rows), self.assertRaises(RuntimeError):
                NOTICES.glib_archive_notices(self.archive(rows), self.VERSION)

    def test_non_regular_notice_and_missing_lgpl_refused(self):
        with self.assertRaisesRegex(RuntimeError, 'Unsafe GLib notice'):
            NOTICES.glib_archive_notices(self.archive([
                ('glib-2.88.3/LICENSES/LGPL-2.1-or-later.txt', b'', 'elsewhere')]), self.VERSION)
        with self.assertRaisesRegex(RuntimeError, 'LGPL license missing'):
            NOTICES.glib_archive_notices(self.archive([]), self.VERSION)

    def test_notice_size_bound(self):
        with self.assertRaisesRegex(RuntimeError, 'Unsafe GLib notice'):
            NOTICES.glib_archive_notices(self.archive([
                ('glib-2.88.3/LICENSES/LGPL-2.1-or-later.txt', b'x' * (2 * 1024 * 1024 + 1), None)]), self.VERSION)

    def test_installed_recipe_receipt_and_pkgconfig_agree(self):
        with tempfile.TemporaryDirectory() as directory:
            installed = self.installation(directory)
            identity, recipe = NOTICES.glib_source_identity(installed)
        self.assertEqual(identity, {'version': self.VERSION, 'url': self.URL, 'sha256': 'a' * 64})
        self.assertIn(b'class Glib', recipe)

    def test_installed_version_and_origin_mismatch_refused(self):
        for kwargs in ({'receipt_version': '2.90.0'}, {'version': 'bogus'},
                       {'url': 'https://example.invalid/source.tar.xz'}, {'sha': 'bad'}):
            with self.subTest(kwargs=kwargs), tempfile.TemporaryDirectory() as directory:
                installed = self.installation(directory, **kwargs)
                with self.assertRaises(RuntimeError):
                    NOTICES.glib_source_identity(installed)

    def test_redirected_provenance_refused(self):
        with tempfile.TemporaryDirectory() as directory:
            installed = self.installation(directory)
            (installed / 'INSTALL_RECEIPT.json').unlink()
            (installed / 'INSTALL_RECEIPT.json').symlink_to('.brew/glib.rb')
            with self.assertRaisesRegex(RuntimeError, 'unsafe installed GLib provenance'):
                NOTICES.glib_source_identity(installed)

    def collector_inputs(self, directory, *, variant=0):
        source = self.archive()
        installed = self.installation(directory, sha=hashlib.sha256(source).hexdigest())
        recipe = (installed / '.brew/glib.rb').read_bytes()
        # Synthetic exact-byte fixtures exercise the three recipe choices;
        # these are not claimed to be corresponding GLib source recipes.
        if variant:
            recipe += b'# synthetic post-install template variant\n'
        if variant == 2:
            recipe += b'# synthetic network sandbox variant\n'
        (installed / '.brew/glib.rb').write_bytes(recipe)
        public_recipe = recipe.replace(b'  depends_on', b'  bottle do\n    rebuild 1\n  end\n\n  depends_on')
        patch, introspection = b'synthetic patch, never executed', b'synthetic build resource'
        commit = str(variant + 1) * 40
        pin = {'recipes': {hashlib.sha256(recipe).hexdigest(): {
                   'homebrew_commit': commit,
                   'formula_sha256': hashlib.sha256(public_recipe).hexdigest()}},
               'patch_sha256': hashlib.sha256(patch).hexdigest(),
               'introspection_url': 'https://download.gnome.org/synthetic-resource.tar.xz',
               'introspection_sha256': hashlib.sha256(introspection).hexdigest()}
        base = 'https://raw.githubusercontent.com/Homebrew/homebrew-core/' + commit + '/'
        inputs = {base + 'Formula/g/glib.rb': public_recipe,
                  base + 'Patches/glib/hardcoded-paths.diff': patch,
                  self.URL: source, pin['introspection_url']: introspection}
        return installed, pin, inputs

    def test_three_reviewed_recipe_hashes_select_exact_public_pins(self):
        expected = {
            'd2ebed597d64ab5518ea78604840e28fb9839fa923bd3917bef5f50b76d369be': (
                '190a6fa77b81b8ffbeeee1022b852063d55d71cc',
                '18ec76babcd6aebbdfeca6f09f9bfb24b3694d4e432c926c7f9612ee53515742'),
            '1c0b615f2c23a2340eda0a52581e9157631c3dfd55227edcf3e14ea565e0a36f': (
                '222fed87fa0d1a1a847a2b610a808a48d5afb526',
                '658003a495be4141e3f5856ff34a1334c58fe7a0fc745afbc84258394472a6db'),
            '0105d4552c1cc68b38cdcd23c4df05c2cb0d93ce44929f581204a6d33dd16d59': (
                '11758a37429b3c8830127b550146c95b2c7ac1c9',
                'd28135a54cacf6e9016a97a412c7cb74b124865d883115dcbf07106237d4aaef'),
        }
        self.assertEqual(set(NOTICES.GLIB_SOURCE[self.VERSION]['recipes']), set(expected))
        for digest, (commit, formula_sha) in expected.items():
            with self.subTest(digest=digest):
                selected = NOTICES.glib_source_pin(self.VERSION, digest)
                self.assertEqual(selected['homebrew_commit'], commit)
                self.assertEqual(selected['formula_sha256'], formula_sha)
                self.assertEqual(selected['patch_sha256'],
                                 'd846efd0bf62918350da94f850db33b0f8727fece9bfaf8164566e3094e80c97')

    def test_each_exact_recipe_variant_collects_only_its_selected_public_inputs(self):
        for variant in range(3):
            with self.subTest(variant=variant), tempfile.TemporaryDirectory() as directory:
                installed, pin, inputs = self.collector_inputs(directory, variant=variant)
                retained = {}
                with mock.patch.dict(NOTICES.GLIB_SOURCE, {self.VERSION: pin}, clear=True), \
                        mock.patch.object(NOTICES, 'get', side_effect=lambda url, maximum: inputs[url]) as fetch:
                    NOTICES.collect_glib_source(installed, lambda path, data, provenance: retained.update({path: (data, provenance)}))
                self.assertEqual(fetch.call_count, 4)
                self.assertEqual(retained['installed/glib/source/homebrew-glib.rb'][0],
                                 (installed / '.brew/glib.rb').read_bytes())
                self.assertEqual(retained['installed/glib/source/homebrew-glib.rb'][1]['homebrew_commit'],
                                 str(variant + 1) * 40)

    def test_exact_source_patch_recipe_and_resource_are_retained(self):
        with tempfile.TemporaryDirectory() as directory:
            installed, pin, inputs = self.collector_inputs(directory)
            retained = {}
            with mock.patch.dict(NOTICES.GLIB_SOURCE, {self.VERSION: pin}, clear=True), \
                    mock.patch.object(NOTICES, 'get', side_effect=lambda url, maximum: inputs[url]):
                self.assertIsNone(NOTICES.collect_glib_source(installed, lambda path, data, provenance: retained.update({path: (data, provenance)})))
        prefix = 'installed/glib/source/'
        self.assertIn(prefix + 'glib-2.88.3.tar.xz', retained)
        self.assertIn(prefix + 'homebrew-core/Patches/glib/hardcoded-paths.diff', retained)
        self.assertIn(prefix + 'gobject-introspection-1.86.0.tar.xz', retained)
        self.assertIn(prefix + 'BUILD-PROVENANCE.json', retained)
        self.assertEqual(retained['installed/glib/COPYING'][0], self.LICENSE)
        self.assertNotIn(b'tmp', retained[prefix + 'BUILD-PROVENANCE.json'][0])

    def test_bad_source_recipe_patch_or_resource_checksum_retains_nothing(self):
        for position, expected in ((0, 'recipe checksum'), (1, 'patch checksum'),
                                   (2, 'recipe checksum'), (3, 'resource checksum')):
            with self.subTest(position=position), tempfile.TemporaryDirectory() as directory:
                installed, pin, inputs = self.collector_inputs(directory)
                inputs[list(inputs)[position]] += b'changed'
                retained = mock.Mock()
                with mock.patch.dict(NOTICES.GLIB_SOURCE, {self.VERSION: pin}, clear=True), \
                        mock.patch.object(NOTICES, 'get', side_effect=lambda url, maximum: inputs[url]), \
                        self.assertRaisesRegex(RuntimeError, expected):
                    NOTICES.collect_glib_source(installed, retained)
                retained.assert_not_called()

    def test_one_byte_recipe_mutation_refuses_before_download_or_retention_with_hash_only(self):
        with tempfile.TemporaryDirectory() as directory:
            installed, pin, inputs = self.collector_inputs(directory)
            with (installed / '.brew/glib.rb').open('ab') as stream:
                stream.write(b'\n')
            changed_sha = hashlib.sha256((installed / '.brew/glib.rb').read_bytes()).hexdigest()
            retained = mock.Mock()
            with mock.patch.dict(NOTICES.GLIB_SOURCE, {self.VERSION: pin}, clear=True), \
                    mock.patch.object(NOTICES, 'get') as fetch, \
                    self.assertRaises(RuntimeError) as error:
                NOTICES.collect_glib_source(installed, retained)
            self.assertEqual(str(error.exception),
                             'Installed GLib build recipe needs pin review; sha256=' + changed_sha)
            fetch.assert_not_called()
            retained.assert_not_called()

    def test_selected_digest_does_not_replace_exact_public_recipe_comparison(self):
        with tempfile.TemporaryDirectory() as directory:
            installed, pin, inputs = self.collector_inputs(directory)
            original_pin = next(iter(pin['recipes'].values()))
            with (installed / '.brew/glib.rb').open('ab') as stream:
                stream.write(b'# different installed build context\n')
            changed_sha = hashlib.sha256((installed / '.brew/glib.rb').read_bytes()).hexdigest()
            # Even a wrongly configured selector entry must not bypass the
            # separate byte comparison. No hash collision is simulated.
            pin['recipes'][changed_sha] = original_pin
            retained = mock.Mock()
            with mock.patch.dict(NOTICES.GLIB_SOURCE, {self.VERSION: pin}, clear=True), \
                    mock.patch.object(NOTICES, 'get', side_effect=lambda url, maximum: inputs[url]) as fetch, \
                    self.assertRaisesRegex(RuntimeError, 'build recipe differs'):
                NOTICES.collect_glib_source(installed, retained)
            self.assertEqual(fetch.call_count, 1)
            retained.assert_not_called()

    def test_unknown_version_refuses_without_download(self):
        with tempfile.TemporaryDirectory() as directory:
            installed = self.installation(directory, version='2.90.0', receipt_version='2.90.0',
                                          url=self.URL.replace('2.88', '2.90').replace('.3.tar', '.0.tar'))
            with mock.patch.object(NOTICES, 'get') as fetch, self.assertRaisesRegex(RuntimeError, 'pin review'):
                NOTICES.collect_glib_source(installed, mock.Mock())
            fetch.assert_not_called()

    def test_archive_count_and_total_declared_bytes_are_bounded(self):
        archive = mock.MagicMock()
        archive.__enter__.return_value = archive
        for byte_overflow in (False, True):
            def members():
                for i in range(1 if byte_overflow else 50001):
                    member = tarfile.TarInfo('glib-2.88.3/source-' + str(i))
                    member.size = 512 * 1024 * 1024 + 1 if byte_overflow else 0
                    yield member
            archive.__iter__.side_effect = members
            with self.subTest(byte_overflow=byte_overflow), \
                    mock.patch.object(NOTICES.tarfile, 'open', return_value=archive), \
                    self.assertRaisesRegex(RuntimeError, 'unbounded GLib source'):
                NOTICES.glib_archive_notices(b'synthetic', self.VERSION)
        archive.extractfile.assert_not_called()


if __name__ == '__main__':
    unittest.main()
