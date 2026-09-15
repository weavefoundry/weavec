import { defineCollection } from 'astro:content';
import { z } from 'astro/zod';
import { glob } from 'astro/loaders';
import { docsSchema, i18nSchema } from '@astrojs/starlight/schema';
import { i18nLoader } from '@astrojs/starlight/loaders';

export const collections = {
  i18n: defineCollection({ loader: i18nLoader(), schema: i18nSchema() }),
  docs: defineCollection({
    loader: glob({ pattern: '**/*.{md,mdx}', base: './.generated' }),
    schema: docsSchema({ extend: z.object({ rfcStatus: z.string().optional() }) }),
  }),
};
