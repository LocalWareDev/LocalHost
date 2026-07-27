import type { Metadata } from "next";
import { notFound } from "next/navigation";
import { DocContent } from "@/components/docs/doc-content";
import { getAllDocPages, getDocPageBySlug } from "@/lib/data/docs";

export function generateStaticParams() {
  return getAllDocPages().map((page) => ({ slug: page.slug }));
}

export function generateMetadata({ params }: { params: { slug: string } }): Metadata {
  const page = getDocPageBySlug(params.slug);
  return {
    title: page?.title ?? "Documentation",
    description: page?.description,
  };
}

export default function DocPageRoute({ params }: { params: { slug: string } }) {
  const page = getDocPageBySlug(params.slug);
  if (!page) return notFound();
  return <DocContent page={page} />;
}
