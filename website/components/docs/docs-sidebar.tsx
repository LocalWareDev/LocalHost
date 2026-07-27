"use client";

import * as React from "react";
import Link from "next/link";
import { usePathname } from "next/navigation";
import { Search } from "lucide-react";
import { cn } from "@/lib/utils";
import { Input } from "@/components/ui/input";
import { docSections } from "@/lib/data/docs";

export function DocsSidebar() {
  const pathname = usePathname();
  const [query, setQuery] = React.useState("");

  const filteredSections = docSections
    .map((section) => ({
      ...section,
      pages: section.pages.filter((page) =>
        page.title.toLowerCase().includes(query.toLowerCase())
      ),
    }))
    .filter((section) => section.pages.length > 0);

  return (
    <aside className="w-full shrink-0 lg:sticky lg:top-20 lg:h-[calc(100vh-6rem)] lg:w-64 lg:overflow-y-auto">
      <div className="relative mb-4">
        <Search className="pointer-events-none absolute left-2.5 top-1/2 h-3.5 w-3.5 -translate-y-1/2 text-muted-foreground" />
        <Input
          value={query}
          onChange={(e) => setQuery(e.target.value)}
          placeholder="Search documentation…"
          className="h-9 pl-8 text-sm"
          aria-label="Search documentation"
        />
      </div>
      <nav className="space-y-6">
        {filteredSections.map((section) => (
          <div key={section.title}>
            <p className="px-2 text-xs font-semibold uppercase tracking-wide text-muted-foreground">
              {section.title}
            </p>
            <ul className="mt-2 space-y-0.5">
              {section.pages.map((page) => {
                const href = `/documentation/${page.slug}`;
                const active = pathname === href;
                return (
                  <li key={page.slug}>
                    <Link
                      href={href}
                      className={cn(
                        "block rounded-md px-2 py-1.5 text-sm transition-colors hover:bg-accent hover:text-foreground",
                        active ? "bg-accent font-medium text-foreground" : "text-muted-foreground"
                      )}
                    >
                      {page.title}
                    </Link>
                  </li>
                );
              })}
            </ul>
          </div>
        ))}
        {filteredSections.length === 0 && (
          <p className="px-2 text-sm text-muted-foreground">No pages match "{query}".</p>
        )}
      </nav>
    </aside>
  );
}
