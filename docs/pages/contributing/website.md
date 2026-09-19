---
title: Work on the documentation website
description: Preview, edit, validate, and deploy the Astro and Starlight website maintained in docs/.
---

The website lives in `docs/` and uses Astro with Starlight. It builds independently of LLVM and the compiler. Use Node.js 24 LTS and npm; the lockfile pins the dependency tree.

## Local development

From the repository root:

```sh
cd docs
nvm use
npm ci
npm run dev
```

Open the local URL printed by Astro. To inspect the production build, including search:

```sh
npm run build
npm run preview
```

Pagefind creates its search index during the production build. Validate search in the production preview.

## Where content lives

| Source                               | Purpose                                                        |
| ------------------------------------ | -------------------------------------------------------------- |
| `docs/pages/`                        | Authored onboarding, task guides, and website pages.           |
| `docs/annotations.md`                | Authoritative annotation and diagnostic reference.             |
| `docs/rfcs/`                         | Authoritative design records and their statuses.               |
| `docs/architecture.md`               | Architecture source, split by section for the site.            |
| `docs/data/diagnostic-remedies.json` | Practical resolution guidance for each diagnostic.             |
| `docs/examples/`                     | Tutorial source files included directly in published examples. |
| `docs/src/`                          | Components, styles, and content configuration.                 |
| `docs/scripts/`                      | Content preparation and validation.                            |

`npm run prepare:content` creates `docs/.generated/`. Do not edit or commit that directory. The preparation step preserves the original guides, rewrites repository links to site routes, and gives generated pages edit links pointing to their real source.

When adding a top-level heading to a split source guide, update its mapping in `scripts/prepare-content.mjs`. An unmapped heading fails the build so content cannot silently disappear. RFC entries and diagnostic pages are discovered automatically; new diagnostic IDs require resolution guidance.

The RFC library reads status from each RFC's own metadata. Page dates use Git commit history. The version badge comes from the root CMake project version, and release notes come from the generated `CHANGELOG.md`.

## Validate changes

```sh
npm test
npm run build
npx playwright install chromium
npm run test:browser
```

The build checks TypeScript, every internal page and fragment link, local assets, canonical URLs, metadata, search output, and the 404 page. Browser tests exercise desktop and mobile navigation, search, RFC filtering, both themes, and accessibility.

Tutorial examples are also exercised against WeaveC by the compiler CI: `weavec` must report the use-after-free example and accept its fix, and `weavec-cc` must record the lookup example's checked index in the ledger and trap on a negative index. You can run them locally from the repository root; `weavec-cc` is taken from the same directory unless `--weavec-cc` names it:

```sh
python3 docs/scripts/check-examples.py --weavec build/dev/bin/weavec
```

## GitHub Pages deployment

The `Documentation` workflow builds and checks pull requests. On `main`, successful website validation publishes the static output to GitHub Pages. It also runs after successful compiler CI so a semantic-release commit made by `GITHUB_TOKEN` refreshes the version badge and generated release notes.

The workflow can be run manually from the Actions tab. Deployment uses the `github-pages` environment and GitHub's short-lived deployment credentials. Generated output stays out of Git.

In repository **Settings → Pages**, select **GitHub Actions** as the build source and set the custom domain to `weavec.com`. Enable **Enforce HTTPS** when the certificate is ready. A `CNAME` file is unnecessary for a custom Actions deployment; the domain is managed in repository settings.

## Cloudflare DNS

Use these records with **DNS only** (gray cloud) and automatic TTL. Replace conflicting web records for these names; preserve unrelated mail or verification records.

| Type  | Name  | Target                   |
| ----- | ----- | ------------------------ |
| A     | `@`   | `185.199.108.153`        |
| A     | `@`   | `185.199.109.153`        |
| A     | `@`   | `185.199.110.153`        |
| A     | `@`   | `185.199.111.153`        |
| CNAME | `www` | `weavefoundry.github.io` |

With `weavec.com` configured as the Pages custom domain, GitHub redirects the `www` variant to it. DNS and certificate issuance can take time; HTTPS enforcement is available after GitHub provisions the certificate. Check the current [GitHub custom-domain instructions](https://docs.github.com/en/pages/configuring-a-custom-domain-for-your-github-pages-site/managing-a-custom-domain-for-your-github-pages-site) when changing this setup.

For domain ownership protection, the organization owner can add `weavec.com` under the organization's Pages settings and publish the unique TXT verification record GitHub supplies. The TXT value is account-specific.

## API documentation

The existing `docs/CMakeLists.txt` remains the optional Doxygen target. Build it with `WEAVEC_BUILD_DOCS=ON` when Doxygen is installed. The public site provides curated architecture and interface guides; Doxygen output is a separate developer artifact.
