// Documentation publishing helpers. Source Markdown remains authoritative.
import path from 'node:path';
import GithubSlugger from 'github-slugger';
import { unified } from 'unified';
import remarkParse from 'remark-parse';
import { visit } from 'unist-util-visit';

export const repository = 'https://github.com/weavefoundry/weavec';

export function sections(markdown) {
  const tree = unified().use(remarkParse).parse(markdown);
  const headings = tree.children.filter((node) => node.type === 'heading' && node.depth === 2);
  const slugger = new GithubSlugger();
  const titleText = (node) => {
    let text = '';
    visit(node, (child) => {
      if (child.type === 'text' || child.type === 'inlineCode') text += child.value;
    });
    return text;
  };
  return headings.map((node, index) => ({
    title: titleText(node),
    slug: slugger.slug(titleText(node)),
    body: markdown.slice(node.position.start.offset, headings[index + 1]?.position.start.offset),
  }));
}

// The status an RFC declares in its header block (`- **Status**: Superseded`).
export function rfcStatus(markdown) {
  return markdown.match(/\*\*Status\*\*:\s*(.+)/)?.[1]?.trim() || 'Draft';
}

export function rewriteUrl(url, source, routes) {
  if (!url || /^(?:[a-z][a-z\d+.-]*:|\/\/)/i.test(url) || url.startsWith('/')) return url;
  const hashIndex = url.indexOf('#');
  const fragment = hashIndex < 0 ? '' : url.slice(hashIndex);
  const local = hashIndex < 0 ? url : url.slice(0, hashIndex);
  const resolved = local
    ? path.posix.normalize(path.posix.join(path.posix.dirname(source), decodeURI(local)))
    : source;
  const target = routes[resolved + fragment] || routes[resolved];
  if (target) return target + (target.includes('#') ? '' : fragment);
  if (resolved.startsWith('../')) throw new Error(`Link leaves repository: ${source}: ${url}`);
  return `${repository}/blob/main/${resolved}${fragment}`;
}

export function rewriteMarkdown(markdown, source, routes) {
  const tree = unified().use(remarkParse).parse(markdown);
  const replacements = [];
  visit(tree, (node) => {
    if (!['link', 'image', 'definition'].includes(node.type)) return;
    const next = rewriteUrl(node.url, source, routes);
    if (next === node.url) return;
    const start = node.position.start.offset;
    const raw = markdown.slice(start, node.position.end.offset);
    // Locate the URL after the label, rather than a matching string in its text.
    const delimiter =
      node.type === 'definition' ? raw.indexOf(']:') + 2 : raw.lastIndexOf('](') + 2;
    const offset = raw.indexOf(node.url, delimiter);
    if (offset < 0) throw new Error(`Cannot rewrite link in ${source}: ${raw}`);
    replacements.push({ start: start + offset, end: start + offset + node.url.length, text: next });
  });
  for (const item of replacements.sort((a, b) => b.start - a.start)) {
    markdown = markdown.slice(0, item.start) + item.text + markdown.slice(item.end);
  }
  return markdown;
}
