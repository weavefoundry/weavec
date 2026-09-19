// Build the website from the repository's maintained Markdown, without copies to edit.
import { readFile, writeFile, readdir, mkdir, rm, rename } from 'node:fs/promises';
import { execFileSync } from 'node:child_process';
import path from 'node:path';
import { fileURLToPath } from 'node:url';
import sharp from 'sharp';
import { repository, sections, rewriteMarkdown } from './content.mjs';

const root = fileURLToPath(new URL('../../', import.meta.url));
const docs = path.join(root, 'docs');
const output = path.join(docs, '.generated');
const pages = [];
const routes = {};
const read = (source) => readFile(path.join(root, source), 'utf8');
const cleanTitle = (text) => text.replaceAll('`', '');

async function writeChanged(destination, content) {
  const bytes = Buffer.isBuffer(content) ? content : Buffer.from(content);
  try {
    if ((await readFile(destination)).equals(bytes)) return;
  } catch (error) {
    if (error.code !== 'ENOENT') throw error;
  }
  // Keep imported files available while the dev server handles concurrent requests.
  const temporary = destination + '.tmp';
  await writeFile(temporary, bytes);
  await rename(temporary, destination);
}

function commitDate(source) {
  try {
    return (
      execFileSync('git', ['log', '-1', '--format=%cI', '--', source], {
        cwd: root,
        encoding: 'utf8',
        stdio: ['ignore', 'pipe', 'ignore'],
      }).trim() || false
    );
  } catch {
    return false; // Source archives have no Git history.
  }
}

function add(source, route, title, body, extra = {}) {
  pages.push({ source, route, title: cleanTitle(title), body, extra });
  routes[source] ??= `/${route}/`;
}

async function whole(source, route, extra = {}) {
  const markdown = await read(source);
  const title = markdown.match(/^# (.+)$/m)?.[1];
  if (!title) throw new Error(`Missing title: ${source}`);
  add(source, route, title, markdown.replace(/^# .+\n+/m, ''), extra);
}

async function split(source, mappings, introRoute, introTitle) {
  const markdown = await read(source);
  const parts = sections(markdown);
  const intro = markdown.slice(0, markdown.indexOf('\n## ')).replace(/^# .+\n+/, '');
  routes[source] = `/${introRoute}/`;
  add(
    source,
    introRoute,
    introTitle,
    intro +
      '\n\n## In this guide\n\n' +
      parts
        .map((part) => {
          const route = mappings[part.title];
          if (!route) throw new Error(`Unmapped heading in ${source}: ${part.title}`);
          return `- [${cleanTitle(part.title)}](/${route}/)`;
        })
        .join('\n'),
  );
  for (const part of parts) {
    const route = mappings[part.title];
    routes[source + '#' + part.slug] = `/${route}/`;
    // Keep the source heading anchor valid after promoting it to the page title.
    const body = `<span id="${part.slug}"></span>\n\n` + part.body.replace(/^## .+\n+/, '');
    add(source, route, part.title, body);
    for (const heading of part.body.matchAll(/^#{3,6} (.+)$/gm)) {
      const { slug } = await import('github-slugger');
      routes[source + '#' + slug(cleanTitle(heading[1]))] = `/${route}/`;
    }
  }
}

await mkdir(output, { recursive: true });
const stale = new Set(
  (await readdir(output, { recursive: true })).filter((name) => /\.(md|mdx|json)$/.test(name)),
);

await split(
  'docs/checked-code.md',
  {
    'Select functions': 'guides/select-functions',
    'Build through the compiler': 'guides/checked-builds',
    'Understand contracts': 'guides/contracts',
    'Check traversals and cursor helpers': 'guides/traversals',
    'Check input cases and union members': 'guides/cases-and-unions',
    'Read a report': 'guides/reports',
    'Check opaque pointers and callback interfaces': 'guides/callbacks',
    'Current limits': 'reference/checked-limits',
    'Linked containers': 'guides/linked-containers',
    'Recursive object ownership': 'guides/recursive-ownership',
    'Growable buffers and vectors': 'guides/growable-buffers',
    'C runtime contracts': 'guides/runtime-contracts',
    'Opaque objects and private library state': 'guides/opaque-objects',
    'Composing recursive and stateful helpers (RFC 0029)': 'guides/composing-helpers',
  },
  'guides/checked-code',
  'Checked code',
);

await split(
  'docs/annotations.md',
  {
    Placement: 'reference/annotation-placement',
    Diagnostics: 'reference/diagnostics',
    'What the checker understands': 'reference/checker-coverage',
    'Related pointer arguments': 'guides/related-pointers',
    'C integers and dynamic bounds': 'guides/integers-and-bounds',
    'Controlling diagnostics': 'reference/diagnostic-controls',
    'Compatibility with other annotation schemes': 'reference/annotation-compatibility',
    'Checked selection (RFC 0018)': 'reference/checked-annotations',
  },
  'reference/annotations',
  'Annotations',
);

await whole('docs/incremental-analysis.md', 'guides/incremental-analysis');
await whole('docs/development.md', 'contributing/development');
await whole('docs/roadmap.md', 'project/roadmap');
await whole('docs/rfcs/README.md', 'rfcs/process');
pages.at(-1).title = 'The RFC process';
pages.at(-1).body = pages.at(-1).body.split('## Index')[0];

const architecture = await read('docs/architecture.md');
const architectureParts = sections(architecture);
const architectureMappings = Object.fromEntries(
  architectureParts.map((part) => [part.title, `internals/${part.slug}`]),
);
await split('docs/architecture.md', architectureMappings, 'internals/architecture', 'Architecture');

const rfcs = [];
for (const filename of (await readdir(path.join(docs, 'rfcs'))).sort()) {
  if (!/^\d{4}-/.test(filename) || filename.startsWith('0000')) continue;
  const source = `docs/rfcs/${filename}`;
  const body = await read(source);
  const status = body.match(/\*\*Status\*\*:\s*(.+)/)?.[1]?.trim() || 'Draft';
  const route = `rfcs/${filename.replace(/\.md$/, '')}`;
  await whole(source, route, { rfcStatus: status });
  rfcs.push({
    number: filename.slice(0, 4),
    title: cleanTitle(body.match(/^# RFC \d+: (.+)$/m)?.[1] || filename),
    status,
    href: `/${route}/`,
  });
}

// Each diagnostic keeps its exact source description and gains a focused remedy.
const diagnosticPage = pages.find((page) => page.route === 'reference/diagnostics');
const remedies = JSON.parse(await read('docs/data/diagnostic-remedies.json'));
const diagnosticRows = diagnosticPage.body
  .split('\n')
  .filter((line) => /^\| `[^`]+`\s*\|/.test(line));
const diagnosticIndex = [];
for (const line of diagnosticRows) {
  const match = line.match(/^\| `([^`]+)`\s*\|\s*(\w+)\s*\|\s*(.*?)\s*\|$/);
  if (!match) throw new Error(`Cannot parse diagnostic: ${line}`);
  const [, id, severity, description] = match;
  const remedy = remedies[id];
  if (!remedy) throw new Error(`Add documentation guidance for ${id}`);
  add(
    'docs/annotations.md',
    `reference/diagnostics/${id}`,
    id,
    `Default severity: **${severity}** · Identifier: \`weavec::${id}\`\n\n## What it means\n\n${description}\n\n## How to resolve it\n\n${remedy}\n\n## Related reference\n\n[Diagnostic controls](/reference/diagnostic-controls/) · [Checked guarantees](/reference/guarantees/)`,
  );
  diagnosticIndex.push(`| [\`${id}\`](/reference/diagnostics/${id}/) | ${severity} |`);
}
diagnosticPage.body =
  '<span id="diagnostics"></span>\n\nEvery WeaveC diagnostic ends with a stable identifier such as `[weavec::use-after-free]`. Open an entry for its exact meaning, conditions, and resolution guidance.\n\n| Diagnostic | Default severity |\n| --- | --- |\n' +
  diagnosticIndex.join('\n') +
  '\n\nStart with the first diagnostic and follow its source notes. A failed checked obligation remains a failure even if its ordinary diagnostic is lowered to a warning. See [diagnostic controls](/reference/diagnostic-controls/).';

try {
  await whole('CHANGELOG.md', 'project/releases');
  pages.at(-1).title = 'Release notes';
  pages.at(-1).body =
    'Release notes are generated from Conventional Commits. Download source archives and checksums from [GitHub Releases](https://github.com/weavefoundry/weavec/releases). See [installation](/getting-started/installation/) for the supported setup.\n\n' +
    pages.at(-1).body;
} catch (error) {
  if (error.code !== 'ENOENT') throw error;
  add(
    'README.md',
    'project/releases',
    'Releases',
    'Download available source releases and checksums from [GitHub Releases](https://github.com/weavefoundry/weavec/releases). See [installation](/getting-started/installation/) to build and install. Release notes are published here when the generated changelog is available.',
  );
}

async function authored(directory) {
  for (const entry of await readdir(directory, { withFileTypes: true })) {
    const file = path.join(directory, entry.name);
    if (entry.isDirectory()) await authored(file);
    else if (/\.mdx?$/.test(entry.name)) {
      const source = path.relative(root, file);
      const route = path
        .relative(path.join(docs, 'pages'), file)
        .replace(/\.mdx?$/, '')
        .replace(/\/index$/, '');
      const raw = await readFile(file, 'utf8');
      pages.push({ source, route, raw });
      routes[source] = route === 'index' ? '/' : `/${route}/`;
    }
  }
}
await authored(path.join(docs, 'pages'));
routes['README.md'] = '/getting-started/introduction/';
routes['docs/rfcs/README.md'] = '/rfcs/';
for (const section of ['process', 'when-an-rfc-is-required'])
  routes['docs/rfcs/README.md#' + section] = '/rfcs/process/';

if (new Set(pages.map((page) => page.route)).size !== pages.length)
  throw new Error('Two documentation sources have the same route.');

for (const page of pages) {
  const extension = page.source.endsWith('.mdx') ? '.mdx' : '.md';
  const destination = path.join(output, page.route + extension);
  stale.delete(path.relative(output, destination));
  await mkdir(path.dirname(destination), { recursive: true });
  let content;
  if (page.raw) {
    let raw = page.raw;
    if (!/^lastUpdated:/m.test(raw))
      raw = raw.replace(/^---\n/, `---\nlastUpdated: ${commitDate(page.source)}\n`);
    for (const match of raw.matchAll(/<!-- example:([\w.-]+) -->/g)) {
      raw = raw.replace(
        match[0],
        '```c title="' + match[1] + '"\n' + (await read('docs/examples/' + match[1])) + '```',
      );
    }
    if (!/^editUrl:/m.test(raw))
      raw = raw.replace(
        /^---\n/,
        `---\neditUrl: ${JSON.stringify(repository + '/edit/main/' + page.source)}\n`,
      );
    content = rewriteMarkdown(raw, page.source, routes);
  } else {
    const lastUpdated = commitDate(page.source);
    const description = `${page.title} — WeaveC documentation for inferred ownership, borrowing, and checked C code.`;
    const frontmatter = {
      title: page.title,
      description,
      editUrl: `${repository}/edit/main/${page.source}`,
      lastUpdated,
      ...page.extra,
    };
    content =
      '---\n' +
      Object.entries(frontmatter)
        .map(
          ([key, value]) =>
            `${key}: ${key === 'lastUpdated' && value ? value : JSON.stringify(value)}`,
        )
        .join('\n') +
      '\n---\n\n' +
      rewriteMarkdown(page.body, page.source, routes);
  }
  await writeChanged(destination, content);
}
const cmake = await read('CMakeLists.txt');
const version = cmake.match(/VERSION\s+(\d+\.\d+\.\d+)/)?.[1] || 'development';
await writeChanged(
  path.join(output, 'project.json'),
  JSON.stringify({ version, rfcs, routes }, null, 2),
);
stale.delete('project.json');
for (const filename of stale) await rm(path.join(output, filename));
await writeChanged(
  path.join(docs, 'public/social-card.png'),
  await sharp(path.join(docs, 'public/social-card.svg')).png().toBuffer(),
);
console.log(
  `Prepared ${pages.length} pages, ${rfcs.length} RFCs, and ${diagnosticRows.length} diagnostic entries for WeaveC ${version}.`,
);
