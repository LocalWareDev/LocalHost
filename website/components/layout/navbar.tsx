"use client";

import * as React from "react";
import Link from "next/link";
import { usePathname } from "next/navigation";
import { ChevronDown, Menu } from "lucide-react";
import { cn } from "@/lib/utils";
import { mainNav } from "@/lib/data/nav";
import { Logo } from "@/components/shared/logo";
import { Button } from "@/components/ui/button";
import { ThemeSwitcher } from "@/components/layout/theme-switcher";
import { CommandPalette } from "@/components/layout/command-palette";
import { MobileNav } from "@/components/layout/mobile-nav";

export function Navbar() {
  const pathname = usePathname();
  const [openGroup, setOpenGroup] = React.useState<string | null>(null);
  const closeTimeout = React.useRef<ReturnType<typeof setTimeout>>();

  const handleEnter = (title: string) => {
    if (closeTimeout.current) clearTimeout(closeTimeout.current);
    setOpenGroup(title);
  };

  const handleLeave = () => {
    closeTimeout.current = setTimeout(() => setOpenGroup(null), 120);
  };

  React.useEffect(() => {
    setOpenGroup(null);
  }, [pathname]);

  return (
    <header className="sticky top-0 z-40 w-full border-b border-border/80 bg-background/85 backdrop-blur supports-[backdrop-filter]:bg-background/70">
      <div className="container flex h-16 items-center justify-between">
        <div className="flex items-center gap-8">
          <Link href="/" aria-label="LocalHost home">
            <Logo />
          </Link>

          <nav className="hidden items-center gap-1 lg:flex" onMouseLeave={handleLeave}>
            {mainNav.map((group) => {
              const hasMenu = !!group.items?.length;
              const active = group.href && pathname.startsWith(group.href) && group.href !== "/";
              return (
                <div key={group.title} className="relative" onMouseEnter={() => hasMenu && handleEnter(group.title)}>
                  {hasMenu ? (
                    <button
                      className={cn(
                        "flex items-center gap-1 rounded-md px-3 py-2 text-sm font-medium text-foreground/80 transition-colors hover:bg-accent hover:text-foreground",
                        active && "text-foreground"
                      )}
                      onClick={() => setOpenGroup(openGroup === group.title ? null : group.title)}
                      aria-expanded={openGroup === group.title}
                    >
                      {group.title}
                      <ChevronDown className="h-3.5 w-3.5 opacity-60" />
                    </button>
                  ) : (
                    <Link
                      href={group.href ?? "#"}
                      className={cn(
                        "block rounded-md px-3 py-2 text-sm font-medium text-foreground/80 transition-colors hover:bg-accent hover:text-foreground",
                        active && "text-foreground"
                      )}
                    >
                      {group.title}
                    </Link>
                  )}

                  {hasMenu && openGroup === group.title && (
                    <div
                      className="absolute left-0 top-full z-50 mt-2 w-[420px] rounded-lg border border-border bg-popover p-3 text-popover-foreground shadow-xl animate-in fade-in-0 zoom-in-95"
                      onMouseEnter={() => handleEnter(group.title)}
                    >
                      <div className="grid grid-cols-1 gap-1">
                        {group.items!.map((item) => (
                          <Link
                            key={item.href}
                            href={item.href}
                            className="rounded-md px-3 py-2.5 transition-colors hover:bg-accent"
                          >
                            <div className="text-sm font-medium">{item.title}</div>
                            {item.description && (
                              <div className="mt-0.5 text-xs text-muted-foreground">{item.description}</div>
                            )}
                          </Link>
                        ))}
                      </div>
                      {group.href && (
                        <Link
                          href={group.href}
                          className="mt-1 block rounded-md px-3 py-2 text-sm font-medium text-primary hover:bg-accent"
                        >
                          View all {group.title.toLowerCase()} →
                        </Link>
                      )}
                    </div>
                  )}
                </div>
              );
            })}
          </nav>
        </div>

        <div className="flex items-center gap-2">
          <CommandPalette />
          <ThemeSwitcher />
          <Button asChild size="sm" className="hidden lg:inline-flex">
            <Link href="/contact">Contact Sales</Link>
          </Button>
          <Button asChild variant="outline" size="sm" className="hidden lg:inline-flex">
            <Link href="/download">Download</Link>
          </Button>
          <MobileNav />
        </div>
      </div>
    </header>
  );
}
