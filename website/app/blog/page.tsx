import type { Metadata } from "next";
import Link from "next/link";
import { ArrowRight } from "lucide-react";
import { Badge } from "@/components/ui/badge";
import { Card, CardContent } from "@/components/ui/card";
import { Breadcrumbs } from "@/components/shared/breadcrumbs";
import { formatDate } from "@/lib/utils";
import { blogPosts, blogCategories } from "@/lib/data/blog";

export const metadata: Metadata = {
  title: "Blog",
  description: "Engineering notes, release updates, and security advisories from the LocalHost team.",
};

export default function BlogPage({ searchParams }: { searchParams?: { category?: string } }) {
  const activeCategory = searchParams?.category;
  const posts = activeCategory ? blogPosts.filter((p) => p.category === activeCategory) : blogPosts;

  return (
    <div className="pb-24">
      <section className="border-b border-border bg-muted/20">
        <div className="container py-16">
          <Breadcrumbs items={[{ title: "Blog" }]} />
          <h1 className="mt-6 max-w-2xl text-balance text-4xl font-semibold tracking-tight sm:text-5xl">
            Engineering blog
          </h1>
          <p className="mt-4 max-w-2xl text-lg text-muted-foreground">
            Notes from the team building LocalHost: architecture decisions, release notes, and
            security advisories.
          </p>
        </div>
      </section>

      <section className="container py-10">
        <div className="flex flex-wrap gap-2">
          <Link href="/blog">
            <Badge variant={!activeCategory ? "default" : "outline"} className="cursor-pointer px-3 py-1">
              All
            </Badge>
          </Link>
          {blogCategories.map((category) => (
            <Link key={category} href={`/blog?category=${encodeURIComponent(category)}`}>
              <Badge variant={activeCategory === category ? "default" : "outline"} className="cursor-pointer px-3 py-1">
                {category}
              </Badge>
            </Link>
          ))}
        </div>

        <div className="mt-8 grid grid-cols-1 gap-5 md:grid-cols-2">
          {posts.map((post) => (
            <Link key={post.slug} href={`/blog/${post.slug}`}>
              <Card className="h-full transition-colors hover:border-primary/40">
                <CardContent className="flex h-full flex-col p-6">
                  <div className="flex items-center gap-2 text-xs text-muted-foreground">
                    <Badge variant="secondary">{post.category}</Badge>
                    <span>{formatDate(post.date)}</span>
                    <span>·</span>
                    <span>{post.readingTime}</span>
                  </div>
                  <h2 className="mt-3 text-lg font-semibold">{post.title}</h2>
                  <p className="mt-2 flex-1 text-sm text-muted-foreground">{post.excerpt}</p>
                  <span className="mt-4 inline-flex items-center gap-1 text-sm font-medium text-primary">
                    Read more <ArrowRight className="h-3.5 w-3.5" />
                  </span>
                </CardContent>
              </Card>
            </Link>
          ))}
          {posts.length === 0 && (
            <p className="text-sm text-muted-foreground">No posts in this category yet.</p>
          )}
        </div>
      </section>
    </div>
  );
}
