import test from 'node:test';
import assert from 'node:assert/strict';
import { sections, rewriteUrl, rewriteMarkdown, rfcStatus } from './content.mjs';

const routes = {
  'docs/architecture.md': '/internals/architecture/',
  'docs/architecture.md#the-runtime': '/internals/the-runtime/',
  'docs/rfcs/0030-prove-or-trap.md': '/rfcs/0030-prove-or-trap/',
};
test('cross-file anchors follow split sections', () => {
  assert.equal(
    rewriteUrl('../architecture.md#the-runtime', 'docs/rfcs/README.md', routes),
    '/internals/the-runtime/#the-runtime',
  );
  assert.equal(
    rewriteUrl('#the-runtime', 'docs/architecture.md', routes),
    '/internals/the-runtime/#the-runtime',
  );
  assert.equal(
    rewriteUrl('0030-prove-or-trap.md#soundness', 'docs/rfcs/0018-checked.md', routes),
    '/rfcs/0030-prove-or-trap/#soundness',
  );
});
test('code examples and external URLs are unchanged; reference links are rewritten', () => {
  const markdown =
    '[guide][g]\n\n[g]: architecture.md#the-runtime\n\n`[code](architecture.md)`\n\n```md\n[code](architecture.md)\n```\n\n[external](https://example.com/)';
  const result = rewriteMarkdown(markdown, 'docs/roadmap.md', routes);
  assert.ok(result.includes('[g]: /internals/the-runtime/#the-runtime'));
  assert.ok(result.includes('`[code](architecture.md)`'));
  assert.ok(result.includes('```md\n[code](architecture.md)\n```'));
  assert.ok(result.includes('[external](https://example.com/)'));
});
test('repository files link to their source and cannot escape the repository', () => {
  assert.equal(
    rewriteUrl('../runtime/weavec_rt.h', 'docs/architecture.md', routes),
    'https://github.com/weavefoundry/weavec/blob/main/runtime/weavec_rt.h',
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
