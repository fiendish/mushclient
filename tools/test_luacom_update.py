"""Test publication boundaries without sending requests or changing repositories."""
from contextlib import nullcontext
from pathlib import Path
import subprocess
import unittest
from unittest.mock import patch

import luacom_vendor as vendor
import update_luacom as update


class PublicationTests(unittest.TestCase):
    def setUp(self):
        self.root = Path('/test-host')
        self.target, self.base, self.head = '1' * 40, '2' * 40, '3' * 40
        self.branch = 'update-luacom-' + self.target
        self.pin = dict(version=1, repository=vendor.REPOSITORY, revision='0' * 40,
                        patches=[])
        self.login = 'fiendish'
        self.prs = []
        self.remote = None
        self.remote_url = self.push_url = 'https://github.com/fiendish/mushclient.git'
        self.calls = []
        self.committed = False
        self.api_failure = False
        self.create_failure = False
        self.race_branch = False
        self.race_pr = False
        self.race_base = False
        self.expected = {'example.cpp': vendor.VendorFile('100644', b'source\n')}
        for target, replacement in (
            ('update_luacom.api', self.api), ('update_luacom.gh', self.gh),
            ('update_luacom.pull_requests', self.pull_requests),
            ('update_luacom.upstream_revision', lambda: self.target),
            ('luacom_vendor.git', self.git), ('luacom_vendor.run', lambda *a, **k: None),
            ('luacom_vendor.read_pin', lambda root: self.pin),
            ('luacom_vendor.source_repository', lambda *a, **k: nullcontext(self.root)),
            ('luacom_vendor.reconstruct', lambda *a: self.expected),
            ('update_luacom.committed_files', lambda *a: self.expected),
        ):
            mock = patch(target, replacement)
            mock.start()
            self.addCleanup(mock.stop)

    def pr(self, login='fiendish', state='open'):
        return {'user': {'login': login}, 'state': state,
                'head': {'ref': self.branch, 'sha': self.head, 'repo': {'full_name': update.HOST}},
                'base': {'ref': 'master', 'repo': {'full_name': update.HOST}},
                'html_url': 'https://github.com/fiendish/mushclient/pull/20'}

    def api(self, endpoint):
        self.calls.append(('api', endpoint))
        if endpoint == 'user':
            return {'login': self.login}
        if endpoint.endswith('/commits/master'):
            return {'sha': '9' * 40 if self.race_base and self.committed else self.base}
        if endpoint.endswith('/pulls/20'):
            return self.pr()
        raise AssertionError(endpoint)

    def pull_requests(self, branch):
        self.assertEqual(branch, self.branch)
        self.calls.append(('search', branch))
        if self.api_failure:
            raise subprocess.CalledProcessError(1, ['gh', 'api'])
        if self.race_pr and self.remote:
            return [self.pr(login='someone-else')]
        return self.prs

    def gh(self, *args):
        self.calls.append(('gh', *args))
        self.assertEqual(args[:2], ('pr', 'create'))
        self.assertIn('--draft', args)
        if self.create_failure:
            raise subprocess.CalledProcessError(1, ['gh', *args])
        self.prs = [self.pr()]
        return self.prs[0]['html_url']

    def git(self, root, *args):
        self.calls.append(('git', *args))
        if args[:2] == ('remote', 'get-url'):
            return (self.push_url if '--push' in args else self.remote_url).encode() + b'\n'
        if args[0] == 'ls-remote':
            value = '9' * 40 if self.race_branch and self.committed else self.remote
            return f'{value}\trefs/heads/{self.branch}\n'.encode() if value else b''
        if args[0] == 'status':
            return b''
        if args[0] == 'rev-parse':
            return (self.head if self.committed else self.base).encode()
        if 'commit' in args:
            self.committed = True
            return b''
        if args[0] == 'log':
            return ('Update LuaCOM to ' + self.target[:12]).encode()
        if args[0] == 'diff':
            return b'luacom/example.cpp\nthird_party/luacom/manifest.json\n'
        if 'push' in args:
            self.remote = self.head
            return b''
        if args[0] in ('switch', 'add', 'fetch'):
            return b''
        if args[0] == 'show':
            import json
            return json.dumps(dict(self.pin, revision=self.target)).encode()
        raise AssertionError(args)

    def mutations(self):
        return [call for call in self.calls if (call[0] == 'gh' or
                (call[0] == 'git' and any(arg in call[1:] for arg in ('push', 'commit', 'switch', 'add'))))]

    def remote_mutations(self):
        return [call for call in self.mutations() if call[0] == 'gh' or 'push' in call[1:]]

    def test_success_checks_owner_after_creating_a_draft(self):
        update.publish(self.root, self.target)
        actions = self.remote_mutations()
        self.assertEqual(len(actions), 2)
        self.assertIn('push', actions[0])
        self.assertEqual(actions[1][:3], ('gh', 'pr', 'create'))
        self.assertEqual(self.calls[-2:], [('api', 'user'), ('api', 'repos/fiendish/mushclient/pulls/20')])

    def test_wrong_identity_stops_before_mutation(self):
        self.login = 'github-actions[bot]'
        with self.assertRaisesRegex(ValueError, 'authenticate'):
            update.publish(self.root, self.target)
        self.assertEqual(self.mutations(), [])

    def test_wrong_fetch_or_push_destination_stops_before_mutation(self):
        for field in ('remote_url', 'push_url'):
            with self.subTest(field=field):
                original = getattr(self, field)
                setattr(self, field, 'https://github.com/other/mushclient.git')
                with self.assertRaisesRegex(ValueError, 'effective origin'):
                    update.publish(self.root, self.target)
                self.assertEqual(self.mutations(), [])
                setattr(self, field, original)

    def test_multiple_push_destinations_fail(self):
        self.push_url += '\nhttps://github.com/other/mushclient.git'
        with self.assertRaisesRegex(ValueError, 'effective origin'):
            update.publish(self.root, self.target)
        self.assertEqual(self.mutations(), [])

    def test_failed_search_stops_before_mutation(self):
        self.api_failure = True
        with self.assertRaises(subprocess.CalledProcessError):
            update.publish(self.root, self.target)
        self.assertEqual(self.mutations(), [])

    def test_foreign_or_closed_pr_stops_before_mutation(self):
        for pr in (self.pr(login='someone-else'), self.pr(state='closed')):
            self.prs = [pr]
            with self.assertRaises(ValueError):
                update.publish(self.root, self.target)
            self.assertEqual(self.mutations(), [])

    def test_orphan_remote_branch_stops_before_mutation(self):
        self.remote = self.head
        with self.assertRaisesRegex(ValueError, 'unexpected head'):
            update.publish(self.root, self.target)
        self.assertEqual(self.mutations(), [])

    def test_existing_matching_pr_needs_no_mutation(self):
        self.prs, self.remote = [self.pr()], self.head
        update.publish(self.root, self.target)
        self.assertEqual(self.mutations(), [])

    def test_existing_drift_stops_without_mutation(self):
        self.prs, self.remote = [self.pr()], self.head
        with patch('update_luacom.committed_files', return_value={}):
            with self.assertRaisesRegex(ValueError, 'source drift'):
                update.publish(self.root, self.target)
        self.assertEqual(self.mutations(), [])

    def test_concurrent_branch_creation_stops_before_push(self):
        self.race_branch = True
        with self.assertRaisesRegex(ValueError, 'unexpected head'):
            update.publish(self.root, self.target)
        self.assertEqual(self.remote_mutations(), [])

    def test_concurrent_pr_creation_stops_after_push(self):
        self.race_pr = True
        with self.assertRaisesRegex(ValueError, 'already has a pull request'):
            update.publish(self.root, self.target)
        self.assertEqual(len(self.remote_mutations()), 1)
        self.assertIn('push', self.remote_mutations()[0])

    def test_changed_base_stops_before_push(self):
        self.race_base = True
        with self.assertRaisesRegex(ValueError, 'master changed'):
            update.publish(self.root, self.target)
        self.assertEqual(self.remote_mutations(), [])

    def test_failed_creation_leaves_branch_and_stops_retry(self):
        self.create_failure = True
        with self.assertRaises(subprocess.CalledProcessError):
            update.publish(self.root, self.target)
        self.assertEqual(self.remote, self.head)
        self.committed = False
        self.calls.clear()
        with self.assertRaisesRegex(ValueError, 'unexpected head'):
            update.publish(self.root, self.target)
        self.assertEqual(self.mutations(), [])


if __name__ == '__main__':
    unittest.main()
