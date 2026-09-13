#!/usr/bin/env python3
"""Expand WeaveC checked report version 3 into the compatible version 2 schema."""
import argparse
import copy
import json
from pathlib import Path
import os
import sys
import tempfile


class JsonStream:
    """Incremental JSON reader; only one function or shared table is decoded."""
    def __init__(self, stream, chunk_size=1024 * 1024):
        self.stream = stream
        self.chunk_size = chunk_size
        self.buffer = ''
        self.position = 0
        self.eof = False
        self.decoder = json.JSONDecoder()

    def more(self, size=None):
        self.buffer = self.buffer[self.position:]
        self.position = 0
        chunk = self.stream.read(size or self.chunk_size)
        self.buffer += chunk
        self.eof = not chunk

    def peek(self):
        while True:
            while self.position < len(self.buffer) and self.buffer[self.position].isspace():
                self.position += 1
            if self.position < len(self.buffer):
                return self.buffer[self.position]
            if self.eof:
                return ''
            self.more()

    def take(self, token):
        if self.peek() != token:
            raise ValueError(f'expected {token!r} in checked report')
        self.position += 1

    def value(self):
        self.peek()
        while True:
            try:
                value, end = self.decoder.raw_decode(self.buffer, self.position)
                # A number can continue into the next chunk.
                if not self.eof and (end == len(self.buffer) or
                                    (type(value) in (int, float) and
                                     self.buffer[end] not in ',]} \t\r\n')):
                    self.more()
                    continue
                self.position = end
                return value
            except json.JSONDecodeError:
                if self.eof:
                    raise
                # Shared tables can be large. Grow geometrically rather than
                # reparsing the same prefix once per fixed-size chunk.
                self.more(max(self.chunk_size, len(self.buffer) - self.position))

    def members(self):
        self.take('{')
        if self.peek() != '}':
            while True:
                key = self.value()
                if not isinstance(key, str):
                    raise ValueError('non-string object key')
                self.take(':')
                yield key
                if self.peek() != ',':
                    break
                self.take(',')
        self.take('}')

    def elements(self):
        self.take('[')
        if self.peek() != ']':
            while True:
                yield
                if self.peek() != ',':
                    break
                self.take(',')
        self.take(']')


def report_events(stream, chunk_size=1024 * 1024):
    """Yield root fields, unit fields and individual functions in file order."""
    reader = JsonStream(stream, chunk_size)
    for key in reader.members():
        if key != 'units':
            yield 'root', None, key, reader.value()
            continue
        for index, _ in enumerate(reader.elements()):
            for field in reader.members():
                if field != 'functions':
                    yield 'unit', index, field, reader.value()
                    continue
                for _ in reader.elements():
                    yield 'function', index, None, reader.value()
    if reader.peek():
        raise ValueError('trailing content in checked report')


class CompactTables:
    fields = ['property', 'outcome', 'location', 'subject', 'reason', 'calls']

    def __init__(self, document):
        if document['obligation_fields'] != self.fields:
            raise ValueError('unsupported obligation layout')
        self.strings = document['strings']
        self.locations = document['locations']
        self.paths = document['call_paths']
        self.obligations = document['obligation_records']

    @staticmethod
    def at(table, index):
        if type(index) is not int or index < 0 or index >= len(table):
            raise ValueError('invalid compact report reference')
        return table[index]

    def location(self, index):
        file, line, column = self.at(self.locations, index)
        return dict(file=self.at(self.strings, file), line=line, column=column)

    def obligation(self, index):
        prop, outcome, where, subject, reason, calls = self.at(self.obligations, index)
        return dict(property=self.at(self.strings, prop), outcome=self.at(self.strings, outcome),
                    location=self.location(where), subject=self.at(self.strings, subject),
                    reason=self.at(self.strings, reason),
                    calls=[self.location(i) for i in self.at(self.paths, calls)])

    def expand_function(self, function):
        result = dict(function, obligations=[self.obligation(i) for i in function['obligations']])
        if 'cases' in result:
            result['cases'] = [self.expand_function(case) for case in result['cases']]
        return result


def expand_report(document):
    """Resolve interned records without changing scope, outcomes, or provenance."""
    result = copy.deepcopy(document)
    if result.get('version') == 2:
        return result
    if result.get('version') != 3:
        raise ValueError('unsupported checked report version')
    tables = CompactTables(result)
    for key in ('strings', 'locations', 'call_paths', 'obligation_records', 'obligation_fields'):
        result.pop(key)
    for unit in result['units']:
        unit['functions'] = [tables.expand_function(function) for function in unit['functions']]
    result['version'] = 2
    return result


def stream_expand(path, output):
    """Expand with memory bounded by shared tables and one function record."""
    metadata, units = {}, {}
    with path.open(encoding='utf-8') as stream:
        for kind, index, key, value in report_events(stream):
            if kind == 'root':
                metadata[key] = value
            elif kind == 'unit':
                units.setdefault(index, {})[key] = value
    version = metadata.get('version')
    if version not in (2, 3):
        raise ValueError('unsupported checked report version')
    tables = CompactTables(metadata) if version == 3 else None
    if tables:
        for key in ('strings', 'locations', 'call_paths', 'obligation_records', 'obligation_fields'):
            metadata.pop(key)
    metadata['version'] = 2
    output.write(json.dumps(metadata)[:-1] + ', "units":[')
    current = -1
    first = True

    def start_unit(index):
        nonlocal current, first
        while current < index:
            if current >= 0:
                output.write(']},')
            current += 1
            first = True
            output.write(json.dumps(units[current])[:-1] + ', "functions":[')

    with path.open(encoding='utf-8') as stream:
        for kind, index, _, function in report_events(stream):
            if kind != 'function':
                continue
            start_unit(index)
            if not first:
                output.write(',')
            first = False
            output.write(json.dumps(tables.expand_function(function) if tables else function))
    if units:
        start_unit(max(units))
        output.write(']}')
    output.write(']}\n')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('report', type=Path)
    parser.add_argument('-o', '--output', type=Path)
    args = parser.parse_args()
    if args.output:
        # Atomic publication also permits expanding in place.
        temporary = None
        try:
            with tempfile.NamedTemporaryFile(mode='w', encoding='utf-8',
                                             dir=args.output.parent, delete=False) as output:
                temporary = Path(output.name)
                stream_expand(args.report, output)
            os.replace(temporary, args.output)
        finally:
            if temporary:
                temporary.unlink(missing_ok=True)
    else:
        stream_expand(args.report, sys.stdout)


if __name__ == '__main__':
    main()
