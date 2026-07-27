# LocalHost — Marketing & Docs Site

Next.js 14 (App Router) + TypeScript + Tailwind CSS + shadcn/ui + Framer Motion.

## Getting started

```bash
npm install
npm run dev
```

Open http://localhost:3000.

## Build

```bash
npm run build
npm start
```

## Structure

- `app/` — routes (home, products, enterprise, pricing, documentation, download, blog, support, about, contact, legal, 404)
- `components/layout/` — navbar, mega menu, command palette (Ctrl+K), footer, theme switcher, cookie consent
- `components/sections/` — homepage/product page sections (hero, features, security, performance, testimonials, CTA, etc.)
- `components/shared/` — reusable UI (pricing cards, comparison tables, code blocks, FAQ accordion, timeline, etc.)
- `components/ui/` — shadcn/ui primitives
- `lib/data/` — static content (nav, products, pricing, docs, blog, testimonials, downloads)

## Notes

- Dark/light theme via `next-themes`.
- SEO: per-page metadata, `sitemap.ts`, `robots.ts`, JSON-LD.
- Not yet run through `npm install` / `npm run build` in this environment — verify locally before deploying, and check disk space first (this dependency set typically needs several hundred MB for `node_modules`).
