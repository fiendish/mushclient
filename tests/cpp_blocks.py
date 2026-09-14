"""Extract C++ source blocks while skipping comments and literals."""
import re


_TOKEN = re.compile(r'//|/\*|R"|[\"\'{}]')


def block(source, signature):
    """Return a definition through its closing brace; raise ValueError on failure."""
    start = source.find(signature)
    if not signature or start < 0:
        raise ValueError(f'Missing source signature: {signature}')
    position = start
    depth = 0
    opened = False
    while match := _TOKEN.search(source, position):
        token = match.group()
        position = match.end()
        if token == '//':
            end = source.find('\n', position)
            while end >= 0 and source[position:end].rstrip('\r').endswith('\\'):
                end = source.find('\n', end + 1)
            position = len(source) if end < 0 else end + 1
        elif token == '/*':
            end = source.find('*/', position)
            if end < 0:
                raise ValueError(f'Unclosed block comment: {signature}')
            position = end + 2
        elif token == 'R"':
            opening = source.find('(', position)
            delimiter = source[position:opening]
            if (opening < 0 or len(delimiter) > 16 or
                    any(char.isspace() or char in '()\\' for char in delimiter)):
                raise ValueError(f'Invalid raw string delimiter: {signature}')
            closing = ')' + delimiter + '"'
            end = source.find(closing, opening + 1)
            if end < 0:
                raise ValueError(f'Unclosed raw string: {signature}')
            position = end + len(closing)
        elif token in ('"', "'"):
            while position < len(source):
                char = source[position]
                if char == token:
                    position += 1
                    break
                if char in '\r\n':
                    raise ValueError(f'Unclosed quoted literal: {signature}')
                if char == '\\':
                    position += 3 if source.startswith('\\\r\n', position) else 2
                else:
                    position += 1
            else:
                raise ValueError(f'Unclosed quoted literal: {signature}')
        elif token == '{':
            depth += 1
            opened = True
        else:
            if not opened:
                raise ValueError(f'Closing brace before source block: {signature}')
            depth -= 1
            if depth == 0:
                return source[start:position]
    if not opened:
        raise ValueError(f'Missing opening brace: {signature}')
    raise ValueError(f'Unclosed source block: {signature}')
