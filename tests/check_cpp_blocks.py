"""Check C++ block extraction and explicit failures for malformed input."""
import unittest

from cpp_blocks import block


class BlockTests(unittest.TestCase):
    def test_ignores_comments_and_literals(self):
        bodies = [
            'if (true) { return; }',
            '// }\nreturn;',
            '/* } { } */ return;',
            'const char *s = "}";',
            "char c = '}';",
            r'const char *s = "\"}";',
            r"char c = '\''; // }",
            r'const char *s = "\\}";',
            'const char *s = "\\\n}";',
            'const char *s = "\\\r\n}";',
            '// continued \\\n} still a comment\nreturn;',
            '// continued \\\r\n} still a comment\r\nreturn;',
            'const char *s = R"(} " /* { */)";',
            'const char *s = u8R"tag(} " )wrong" // {\n)tag";',
            'const char *s = LR"abcdefghijklmnop(})abcdefghijklmnop";',
        ]
        for body in bodies:
            with self.subTest(body=body):
                definition = 'void sample() {\n' + body + '\n}'
                source = '// preceding {\n' + definition + '\nvoid later() {}'
                self.assertEqual(block(source, 'void sample()'), definition)

    def test_ignores_braces_before_body(self):
        definition = 'class Sample /* } */ { int value; }'
        self.assertEqual(block(definition + '; class Later {};', 'class Sample'), definition)

    def test_preserves_crlf(self):
        definition = 'void sample() {\r\n// }\r\nreturn;\r\n}'
        self.assertEqual(block(definition + '\r\nvoid later() {}', 'void sample()'), definition)

    def test_reports_malformed_input(self):
        cases = [
            ('void different() {}', 'Missing source signature'),
            ('void sample()', 'Missing opening brace'),
            ('void sample() {', 'Unclosed source block'),
            ('void sample() }', 'Closing brace before source block'),
            ('void sample() { /* }', 'Unclosed block comment'),
            ('void sample() { "}', 'Unclosed quoted literal'),
            ("void sample() { '}", 'Unclosed quoted literal'),
            ('void sample() { "}\n}', 'Unclosed quoted literal'),
            ('void sample() { "\\', 'Unclosed quoted literal'),
            ('void sample() { R"tag(})wrong"; }', 'Unclosed raw string'),
            ('void sample() { R"tag', 'Invalid raw string delimiter'),
            ('void sample() { R"bad tag(})bad tag"; }', 'Invalid raw string delimiter'),
            ('void sample() { R"abcdefghijklmnopq(})abcdefghijklmnopq"; }',
             'Invalid raw string delimiter'),
        ]
        for source, message in cases:
            with self.subTest(source=source):
                with self.assertRaisesRegex(ValueError, message + ': void sample'):
                    block(source, 'void sample()')


if __name__ == '__main__':
    unittest.main()
