import { defineCollection } from 'astro:content';
import { z } from 'astro/zod';
import { glob } from 'astro/loaders';
import { docsSchema } from '@astrojs/starlight/schema';

export const collections = {
  docs: defineCollection({
    loader: glob({ pattern: '**/*.{md,mdx}', base: './.generated' }),
    schema: docsSchema({ extend: z.object({ rfcStatus: z.string().optional() }) }),
  }),
};
