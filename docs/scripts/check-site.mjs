// Validate the complete output, including cross-page fragments and local assets.
import { readdir, readFile, stat } from 'node:fs/promises';
import path from 'node:path';
import { load } from 'cheerio';

const directory = path.resolve('dist');
const pages = new Map();
const errors = [];
async function crawl(folder) {
  for (const entry of await readdir(folder, { withFileTypes: true })) {
    const file = path.join(folder, entry.name);
    if (entry.isDirectory()) await crawl(file);
    else if (entry.name.endsWith('.html')) {
      const relative = path.relative(directory, file).split(path.sep).join('/');
      const url = '/' + relative.replace(/index\.html$/, '');
      pages.set(url, { file, $: load(await readFile(file, 'utf8')) });
    }
  }
}
await crawl(directory);
for (const [url, { $ }] of pages) {
  const missing = (message) => errors.push(`${url}: ${message}`);
  if ($('h1').length !== 1) missing(`expected one h1, found ${$('h1').length}`);
  if (!$('title').text().trim()) missing('missing title');
  if (!$('meta[name="description"]').attr('content')) missing('missing description');
  const canonical = $('link[rel="canonical"]').attr('href');
  if (!canonical?.startsWith('https://weavec.com/')) missing('missing canonical domain');
  if ($('a[href*=".generated"],a[href*="/src/content/"]').length)
    missing('generated source leaked into links');
  for (const element of $('a[href],img[src],script[src],link[href]').toArray()) {
    const attribute = ['img', 'script'].includes(element.tagName) ? 'src' : 'href';
    const raw = $(element).attr(attribute);
    if (!raw || /^(mailto:|tel:|data:|javascript:)/i.test(raw)) continue;
    const target = new URL(raw, `https://weavec.com${url}`);
    const repoPath = target.pathname.match(
      /^\/weavefoundry\/weavec\/(?:blob|edit|tree)\/main\/(.+)$/,
    );
    if (target.origin === 'https://github.com' && repoPath) {
      try {
        await stat(path.resolve('..', decodeURI(repoPath[1])));
      } catch {
        missing(`missing repository target ${raw}`);
      }
    }
    if (target.origin !== 'https://weavec.com') continue;
    const pathname = decodeURI(target.pathname);
    const targetPage = pages.get(pathname) || pages.get(pathname + '/');
    if (targetPage) {
      if (target.hash) {
        const id = decodeURIComponent(target.hash.slice(1));
        if (
          !targetPage
            .$('[id]')
            .toArray()
            .some((node) => targetPage.$(node).attr('id') === id)
        )
          missing(`missing anchor ${raw}`);
      }
    } else {
      try {
        await stat(path.join(directory, pathname));
      } catch {
        missing(`missing local target ${raw}`);
      }
    }
  }
}
for (const file of [
  'sitemap-index.xml',
  'sitemap-0.xml',
  'robots.txt',
  'social-card.png',
  'favicon.svg',
  'pagefind/pagefind.js',
  '404.html',
]) {
  try {
    await stat(path.join(directory, file));
  } catch {
    errors.push(`Missing required output: ${file}`);
  }
}
if (errors.length) {
  console.error(errors.join('\n'));
  throw new Error(`${errors.length} site validation errors`);
}
console.log(
  `Validated ${pages.size} HTML pages: titles, descriptions, canonical URLs, links, anchors, and assets.`,
);
