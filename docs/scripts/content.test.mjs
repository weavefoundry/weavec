import test from 'node:test';
import assert from 'node:assert/strict';
import { sections, rewriteUrl, rewriteMarkdown } from './content.mjs';

const routes = {
  'docs/checked-code.md': '/guides/checked-code/',
  'docs/checked-code.md#read-a-report': '/guides/reports/',
  'docs/rfcs/0018-checked-code-and-safety-contracts.md':
    '/rfcs/0018-checked-code-and-safety-contracts/',
};
test('cross-file anchors follow split sections', () => {
  assert.equal(
    rewriteUrl('../checked-code.md#read-a-report', 'docs/rfcs/README.md', routes),
    '/guides/reports/#read-a-report',
  );
  assert.equal(
    rewriteUrl('#read-a-report', 'docs/checked-code.md', routes),
    '/guides/reports/#read-a-report',
  );
});
test('code examples and external URLs are unchanged; reference links are rewritten', () => {
  const markdown =
    '[guide][g]\n\n[g]: checked-code.md#read-a-report\n\n`[code](checked-code.md)`\n\n```md\n[code](checked-code.md)\n```\n\n[external](https://example.com/)';
  const result = rewriteMarkdown(markdown, 'docs/annotations.md', routes);
  assert.ok(result.includes('[g]: /guides/reports/#read-a-report'));
  assert.ok(result.includes('`[code](checked-code.md)`'));
  assert.ok(result.includes('```md\n[code](checked-code.md)\n```'));
  assert.ok(result.includes('[external](https://example.com/)'));
});
test('repository files link to their source and cannot escape the repository', () => {
  assert.equal(
    rewriteUrl('../lib/Core/Place.cpp', 'docs/architecture.md', routes),
    'https://github.com/weavefoundry/weavec/blob/main/lib/Core/Place.cpp',
  );
  assert.throws(() => rewriteUrl('../../private.txt', 'docs/file.md', routes), /leaves repository/);
});
test('section splitting ignores headings inside code fences', () => {
  const result = sections('# Guide\n\n## First\n\n```md\n## Example\n```\n\n## Second\ntext');
  assert.deepEqual(
    result.map((part) => part.title),
    ['First', 'Second'],
  );
  assert.ok(result[0].body.includes('## Example'));
});
