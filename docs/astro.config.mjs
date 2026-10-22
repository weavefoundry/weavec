import { defineConfig } from 'astro/config';
import starlight from '@astrojs/starlight';
import { satteri } from '@astrojs/markdown-satteri';
import { watchContent } from './scripts/watch-content.mjs';
import { accessibleTables } from './scripts/accessible-tables.mjs';

const item = (label, slug) => ({ label, slug });
export default defineConfig({
  site: 'https://weavec.com',
  trailingSlash: 'always',
  markdown: { processor: satteri({ hastPlugins: [accessibleTables] }) },
  integrations: [
    watchContent(),
    starlight({
      title: 'WeaveC',
      description:
        'Run-time memory safety for existing C code: a drop-in C compiler that guards every memory access. Built on Clang and LLVM.',
      logo: { src: './src/assets/mark.svg', replacesTitle: false },
      favicon: '/favicon.svg',
      social: [
        {
          icon: 'github',
          label: 'WeaveC on GitHub',
          href: 'https://github.com/weavefoundry/weavec',
        },
      ],
      editLink: { baseUrl: 'https://github.com/weavefoundry/weavec/edit/main/docs/pages/' },
      lastUpdated: true,
      credits: false,
      markdown: { processedDirs: ['./.generated'] },
      customCss: [
        '@fontsource-variable/inter',
        '@fontsource-variable/space-grotesk',
        '@fontsource-variable/jetbrains-mono',
        './src/styles/custom.css',
      ],
      components: {
        Header: './src/components/Header.astro',
        Footer: './src/components/Footer.astro',
        PageTitle: './src/components/PageTitle.astro',
        Head: './src/components/Head.astro',
        Hero: './src/components/Home.astro',
      },
      expressiveCode: {
        themes: ['github-dark', 'github-light'],
        styleOverrides: {
          borderRadius: '0.65rem',
          codeFontSize: '0.84rem',
          codeFontFamily: '"JetBrains Mono Variable", monospace',
        },
      },
      sidebar: [
        {
          label: 'Start here',
          items: [
            item('Introduction', 'getting-started/introduction'),
            item('Installation', 'getting-started/installation'),
            item('Your first check', 'getting-started/first-check'),
          ],
        },
        {
          label: 'Use WeaveC',
          items: [
            item('Adopt incrementally', 'guides/adoption'),
            item('Build integration', 'guides/build-integration'),
            item('Unsafe boundaries', 'guides/unsafe'),
          ],
        },
        {
          label: 'Reference',
          collapsed: true,
          items: [
            item('Command line', 'reference/cli'),
            item('Safety guarantees', 'reference/guarantees'),
            item('The weavec.h header', 'reference/annotations'),
            item('Compatibility', 'reference/compatibility'),
            item('Troubleshooting', 'reference/troubleshooting'),
          ],
        },
        {
          label: 'Project',
          collapsed: true,
          items: [
            item('Release notes', 'project/releases'),
            item('Roadmap', 'project/roadmap'),
            item('Architecture', 'internals/architecture'),
            item('RFC library', 'rfcs'),
            item('Contributing', 'contributing/overview'),
            item('Developer guide', 'contributing/development'),
            item('Website guide', 'contributing/website'),
          ],
        },
      ],
    }),
  ],
});
