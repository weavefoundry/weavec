import test from 'node:test';
import assert from 'node:assert/strict';
import { sections, rewriteUrl, rewriteMarkdown, rfcStatus } from './content.mjs';

const routes = {
  'docs/annotations.md': '/reference/annotations/',
  'docs/annotations.md#placement': '/reference/annotation-placement/',
  'docs/rfcs/0030-prove-or-trap.md': '/rfcs/0030-prove-or-trap/',
};
test('cross-file anchors follow split sections', () => {
  assert.equal(
    rewriteUrl('../annotations.md#placement', 'docs/rfcs/README.md', routes),
    '/reference/annotation-placement/#placement',
  );
  assert.equal(
    rewriteUrl('#placement', 'docs/annotations.md', routes),
    '/reference/annotation-placement/#placement',
  );
  assert.equal(
    rewriteUrl('0030-prove-or-trap.md#soundness', 'docs/rfcs/0018-checked.md', routes),
    '/rfcs/0030-prove-or-trap/#soundness',
  );
});
test('code examples and external URLs are unchanged; reference links are rewritten', () => {
  const markdown =
    '[guide][g]\n\n[g]: annotations.md#placement\n\n`[code](annotations.md)`\n\n```md\n[code](annotations.md)\n```\n\n[external](https://example.com/)';
  const result = rewriteMarkdown(markdown, 'docs/architecture.md', routes);
  assert.ok(result.includes('[g]: /reference/annotation-placement/#placement'));
  assert.ok(result.includes('`[code](annotations.md)`'));
  assert.ok(result.includes('```md\n[code](annotations.md)\n```'));
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
test('an RFC status comes from its header block', () => {
  const superseded =
    '# RFC 0018: Title\n\n- **Status**: Superseded\n- **Supersedes / superseded by**: Superseded by RFC 0030\n\n> Superseded by [RFC 0030](0030-prove-or-trap.md).\n\nLater text names **Status**: Implemented.';
  assert.equal(rfcStatus(superseded), 'Superseded');
  assert.equal(rfcStatus('# RFC 0030: Title\n\n- **Status**: Accepted  \n'), 'Accepted');
  assert.equal(rfcStatus('# RFC 0099: No header\n'), 'Draft');
});
