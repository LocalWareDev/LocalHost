import Link from "next/link";
import { ChevronRight, Home } from "lucide-react";

export interface Crumb {
  title: string;
  href?: string;
}

export function Breadcrumbs({ items }: { items: Crumb[] }) {
  return (
    <nav aria-label="Breadcrumb" className="flex items-center text-sm text-muted-foreground">
      <Link href="/" className="flex items-center hover:text-foreground" aria-label="Home">
        <Home className="h-3.5 w-3.5" />
      </Link>
      {items.map((item, i) => (
        <span key={i} className="flex items-center">
          <ChevronRight className="mx-2 h-3.5 w-3.5" />
          {item.href ? (
            <Link href={item.href} className="hover:text-foreground">
              {item.title}
            </Link>
          ) : (
            <span className="text-foreground">{item.title}</span>
          )}
        </span>
      ))}
    </nav>
  );
}
