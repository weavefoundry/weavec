import { test, expect } from '@playwright/test';
import AxeBuilder from '@axe-core/playwright';

test('homepage, onboarding, and both themes are accessible without horizontal overflow', async ({
  page,
}) => {
  for (const route of [
    '/',
    '/getting-started/installation/',
    '/reference/diagnostics/use-after-free/',
  ]) {
    await page.goto(route);
    await expect(page.locator('h1')).toBeVisible();
    for (const theme of ['light', 'dark']) {
      await page.locator('.header-links starlight-theme-select select').selectOption(theme);
      await expect(page.locator('html')).toHaveAttribute('data-theme', theme);
      await page.evaluate(() => document.fonts.ready);
      // Expressive Code makes overflowing blocks keyboard-focusable after layout.
      await expect
        .poll(() =>
          page
            .locator('.expressive-code pre')
            .evaluateAll((blocks) =>
              blocks.every(
                (block) =>
                  block.scrollWidth <= block.clientWidth || (block as HTMLElement).tabIndex === 0,
              ),
            ),
        )
        .toBe(true);
      expect(
        await page.evaluate(() => document.documentElement.scrollWidth <= window.innerWidth),
      ).toBe(true);
      const result = await new AxeBuilder({ page })
        .withTags(['wcag2a', 'wcag2aa', 'wcag21aa'])
        .analyze();
      expect(result.violations).toEqual([]);
    }
  }
});

test('search finds a diagnostic and opens a result', async ({ page }) => {
  await page.goto('/');
  await page.locator('button[data-open-modal]').click();
  const search = page.locator('.pagefind-ui__search-input');
  await search.fill('use-after-free');
  const results = page.locator('.pagefind-ui__result-link');
  await expect(results.first()).toBeVisible();
  await results.first().click();
  await expect(page.locator('h1')).toBeVisible();
  expect(page.url()).not.toBe('http://127.0.0.1:4321/');
});

test('RFC library filters by title, number, status, and empty state', async ({ page }) => {
  await page.goto('/rfcs/');
  await page.locator('#rfc-query').fill('0028');
  await expect(page.locator('.rfc-list li:visible')).toHaveCount(1);
  await expect(page.locator('.rfc-list li:visible')).toContainText(/opaque/i);
  await page.locator('#rfc-query').fill('no-such-design');
  await expect(page.locator('#rfc-empty')).toBeVisible();
  await page.locator('#rfc-query').fill('');
  await page.locator('#rfc-status').selectOption('Accepted');
  await expect(page.locator('.rfc-list li:visible').first()).toContainText('Accepted');
  const result = await new AxeBuilder({ page }).withTags(['wcag2a', 'wcag2aa']).analyze();
  expect(result.violations).toEqual([]);
});

test('documentation navigation and copy controls work', async ({ page, isMobile, context }) => {
  await page.goto('/getting-started/first-check/');
  const copy = page.locator('.expressive-code .copy button').first();
  await expect(copy).toBeVisible();
  await context.grantPermissions(['clipboard-read', 'clipboard-write']);
  await copy.click();
  expect(await page.evaluate(() => navigator.clipboard.readText())).toContain('node_free(alias)');
  if (isMobile) await page.getByRole('button', { name: 'Menu', exact: true }).click();
  await page
    .locator('#starlight__sidebar a')
    .filter({ hasText: /^Installation$/ })
    .click();
  await expect(page.locator('h1')).toHaveText('Installation');
  await page.getByRole('tab', { name: 'Ubuntu / Debian' }).click();
  await expect(page.getByRole('tabpanel', { name: 'Ubuntu / Debian' })).toBeVisible();
  await page.goto('/404.html');
  await expect(page.locator('h1')).toHaveText('Page not found');
});
