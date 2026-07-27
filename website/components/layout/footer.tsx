import Link from "next/link";
import { Github, Linkedin, Twitter } from "lucide-react";
import { Logo } from "@/components/shared/logo";
import { footerNav } from "@/lib/data/nav";
import { siteConfig } from "@/lib/site-config";
import { Separator } from "@/components/ui/separator";

const columns: { title: string; links: { title: string; href: string }[] }[] = [
  { title: "Product", links: footerNav.product },
  { title: "Resources", links: footerNav.resources },
  { title: "Company", links: footerNav.company },
  { title: "Legal", links: footerNav.legal },
];

export function Footer() {
  return (
    <footer className="border-t border-border bg-background">
      <div className="container py-16">
        <div className="grid grid-cols-2 gap-10 md:grid-cols-6">
          <div className="col-span-2 md:col-span-2">
            <Logo />
            <p className="mt-4 max-w-xs text-sm text-muted-foreground">{siteConfig.description}</p>
            <div className="mt-6 flex items-center gap-3 text-muted-foreground">
              <Link href={siteConfig.links.github} aria-label="GitHub" className="hover:text-foreground">
                <Github className="h-4 w-4" />
              </Link>
              <Link href={siteConfig.links.twitter} aria-label="Twitter" className="hover:text-foreground">
                <Twitter className="h-4 w-4" />
              </Link>
              <Link href={siteConfig.links.linkedin} aria-label="LinkedIn" className="hover:text-foreground">
                <Linkedin className="h-4 w-4" />
              </Link>
            </div>
          </div>

          {columns.map((col) => (
            <div key={col.title}>
              <h3 className="text-sm font-semibold">{col.title}</h3>
              <ul className="mt-4 space-y-3">
                {col.links.map((link) => (
                  <li key={link.href}>
                    <Link href={link.href} className="text-sm text-muted-foreground hover:text-foreground">
                      {link.title}
                    </Link>
                  </li>
                ))}
              </ul>
            </div>
          ))}
        </div>

        <Separator className="my-10" />

        <div className="flex flex-col items-start justify-between gap-4 text-xs text-muted-foreground md:flex-row md:items-center">
          <p>
            © {new Date().getFullYear()} {siteConfig.company}. All rights reserved. LocalHost is a trademark of{" "}
            {siteConfig.company}.
          </p>
          <div className="flex gap-4">
            <Link href="/privacy" className="hover:text-foreground">
              Privacy
            </Link>
            <Link href="/terms" className="hover:text-foreground">
              Terms
            </Link>
            <Link href="/support#status" className="hover:text-foreground">
              System Status
            </Link>
          </div>
        </div>
      </div>
    </footer>
  );
}
