import type { Metadata } from "next";
import Link from "next/link";
import { notFound } from "next/navigation";
import { Badge } from "@/components/ui/badge";
import { Breadcrumbs } from "@/components/shared/breadcrumbs";
import { formatDate } from "@/lib/utils";
import { blogPosts, getPostBySlug } from "@/lib/data/blog";

export function generateStaticParams() {
  return blogPosts.map((post) => ({ slug: post.slug }));
}

export function generateMetadata({ params }: { params: { slug: string } }): Metadata {
  const post = getPostBySlug(params.slug);
  return {
    title: post?.title ?? "Blog",
    description: post?.excerpt,
  };
}

export default function BlogPostPage({ params }: { params: { slug: string } }) {
  const post = getPostBySlug(params.slug);
  if (!post) return notFound();

  return (
    <div className="container-prose py-16">
      <Breadcrumbs items={[{ title: "Blog", href: "/blog" }, { title: post.title }]} />
      <div className="mt-6 flex items-center gap-2 text-xs text-muted-foreground">
        <Badge variant="secondary">{post.category}</Badge>
        <span>{formatDate(post.date)}</span>
        <span>·</span>
        <span>{post.readingTime}</span>
      </div>
      <h1 className="mt-4 text-balance text-4xl font-semibold tracking-tight">{post.title}</h1>
      <p className="mt-3 text-sm text-muted-foreground">By {post.author}</p>

      <div className="mt-10 space-y-5">
        {post.content.map((paragraph, i) => (
          <p key={i} className="leading-relaxed text-foreground/90">
            {paragraph}
          </p>
        ))}
      </div>

      <div className="mt-16 border-t border-border pt-8">
        <Link href="/blog" className="text-sm font-medium text-primary hover:underline">
          ← Back to the blog
        </Link>
      </div>
    </div>
  );
}
