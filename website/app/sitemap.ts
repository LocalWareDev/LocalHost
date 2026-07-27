import type { MetadataRoute } from "next";
import { siteConfig } from "@/lib/site-config";
import { products } from "@/lib/data/products";
import { getAllDocPages } from "@/lib/data/docs";
import { blogPosts } from "@/lib/data/blog";

export default function sitemap(): MetadataRoute.Sitemap {
  const staticRoutes = [
    "",
    "/products",
    "/enterprise",
    "/documentation",
    "/pricing",
    "/download",
    "/blog",
    "/support",
    "/about",
    "/contact",
    "/privacy",
    "/terms",
  ];

  const productRoutes = products.map((p) => `/products/${p.slug}`);
  const docRoutes = getAllDocPages().map((p) => `/documentation/${p.slug}`);
  const blogRoutes = blogPosts.map((p) => `/blog/${p.slug}`);

  const allRoutes = [...staticRoutes, ...productRoutes, ...docRoutes, ...blogRoutes];

  return allRoutes.map((route) => ({
    url: `${siteConfig.url}${route}`,
    lastModified: new Date(),
    changeFrequency: route === "" ? "daily" : "weekly",
    priority: route === "" ? 1 : 0.7,
  }));
}
