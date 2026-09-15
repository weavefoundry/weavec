# WeaveC documentation

The public documentation site is [weavec.com](https://weavec.com), built with
Astro and Starlight and deployed by `.github/workflows/docs.yml`.

## Work on the site

Use Node.js 24 LTS (see `.nvmrc`):

```sh
cd docs
npm ci
npm run dev
```

For the production build and search preview:

```sh
npm test
npm run build
npm run preview
```

See [the website contributor guide](pages/contributing/website.md) for the
content layout, browser tests, deployment, and custom-domain DNS records.

The original Markdown guides and RFCs remain the source of truth. The build
splits and imports them into `.generated/`; do not edit or commit generated
content. Author new site-specific content in `pages/`.

## Compiler and design documentation

- [Developer guide](development.md)
- [Architecture](architecture.md)
- [Annotations and diagnostics](annotations.md)
- [Checked code](checked-code.md)
- [RFCs](rfcs/README.md)
- [Roadmap](roadmap.md)

The existing CMake `docs` target still generates the optional Doxygen API
reference when configured with `WEAVEC_BUILD_DOCS=ON` and Doxygen is installed.
