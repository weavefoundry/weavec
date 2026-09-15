import { defineConfig } from 'astro/config';
import starlight from '@astrojs/starlight';
import { watchContent } from './scripts/watch-content.mjs';

const item = (label, slug) => ({ label, slug });
export default defineConfig({
  site: 'https://weavec.com',
  trailingSlash: 'always',
  integrations: [
    watchContent(),
    starlight({
      title: 'WeaveC',
      description:
        'Inferred ownership, borrowing, and checked memory safety for existing C code. Built on Clang and LLVM.',
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
            item('Ownership & borrowing', 'guides/ownership'),
            item('Adopt incrementally', 'guides/adoption'),
            item('Build integration', 'guides/build-integration'),
            item('Whole-program analysis', 'guides/whole-program'),
            item('Unsafe boundaries', 'guides/unsafe'),
            item('Cache & performance', 'guides/incremental-analysis'),
          ],
        },
        {
          label: 'Checked code',
          collapsed: true,
          items: [
            item('Overview', 'guides/checked-code'),
            item('Select functions', 'guides/select-functions'),
            item('Checked builds', 'guides/checked-builds'),
            item('Understand contracts', 'guides/contracts'),
            item('Read a report', 'guides/reports'),
            item('Traversals & cursors', 'guides/traversals'),
            item('Cases & unions', 'guides/cases-and-unions'),
            item('Callbacks & interfaces', 'guides/callbacks'),
            item('Linked containers', 'guides/linked-containers'),
            item('Recursive ownership', 'guides/recursive-ownership'),
            item('Growable buffers', 'guides/growable-buffers'),
            item('C runtime contracts', 'guides/runtime-contracts'),
            item('Opaque objects', 'guides/opaque-objects'),
            item('Related pointers', 'guides/related-pointers'),
            item('Integers & bounds', 'guides/integers-and-bounds'),
          ],
        },
        {
          label: 'Reference',
          collapsed: true,
          items: [
            item('Command line', 'reference/cli'),
            item('Annotations', 'reference/annotations'),
            item('Annotation placement', 'reference/annotation-placement'),
            item('Checked annotations', 'reference/checked-annotations'),
            item('Diagnostics', 'reference/diagnostics'),
            item('Diagnostic controls', 'reference/diagnostic-controls'),
            item('Safety guarantees', 'reference/guarantees'),
            item('Checked limits', 'reference/checked-limits'),
            item('Checker coverage', 'reference/checker-coverage'),
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
            item('Validation records', 'internals/validation'),
            item('Contributing', 'contributing/overview'),
            item('Developer guide', 'contributing/development'),
            item('Website guide', 'contributing/website'),
          ],
        },
      ],
    }),
  ],
});
