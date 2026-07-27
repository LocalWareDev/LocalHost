import type { Metadata } from "next";
import Link from "next/link";
import { ArrowRight } from "lucide-react";
import { docSections } from "@/lib/data/docs";

export const metadata: Metadata = {
  title: "Documentation",
  description: "LocalHost documentation: installation, virtual machines, networking, storage, CLI, and API reference.",
};

export default function DocumentationIndexPage() {
  return (
    <div className="max-w-4xl">
      <h1 className="text-3xl font-semibold tracking-tight">LocalHost Documentation</h1>
      <p className="mt-3 text-lg text-muted-foreground">
        Everything you need to install, operate, and automate LocalHost — from a first VM to a
        managed fleet.
      </p>

      <div className="mt-10 grid grid-cols-1 gap-4 sm:grid-cols-2">
        {docSections.flatMap((section) => section.pages).map((page) => (
          <Link
            key={page.slug}
            href={`/documentation/${page.slug}`}
            className="rounded-lg border border-border p-5 transition-colors hover:border-primary/40 hover:bg-accent/40"
          >
            <div className="flex items-center justify-between">
              <h2 className="font-semibold">{page.title}</h2>
              <ArrowRight className="h-4 w-4 text-muted-foreground" />
            </div>
            <p className="mt-1.5 text-sm text-muted-foreground">{page.description}</p>
          </Link>
        ))}
      </div>
    </div>
  );
}
