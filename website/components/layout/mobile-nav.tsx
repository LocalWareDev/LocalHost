"use client";

import * as React from "react";
import Link from "next/link";
import { Menu } from "lucide-react";
import { Button } from "@/components/ui/button";
import { Sheet, SheetContent, SheetHeader, SheetTitle, SheetTrigger, SheetClose } from "@/components/ui/sheet";
import { mainNav } from "@/lib/data/nav";
import { Logo } from "@/components/shared/logo";

export function MobileNav() {
  const [open, setOpen] = React.useState(false);

  return (
    <Sheet open={open} onOpenChange={setOpen}>
      <SheetTrigger asChild>
        <Button variant="ghost" size="icon" aria-label="Open menu" className="lg:hidden">
          <Menu className="h-5 w-5" />
        </Button>
      </SheetTrigger>
      <SheetContent side="right" className="w-full sm:max-w-sm overflow-y-auto">
        <SheetHeader>
          <SheetTitle>
            <Logo />
          </SheetTitle>
        </SheetHeader>
        <nav className="mt-6 flex flex-col gap-1">
          {mainNav.map((group) => (
            <div key={group.title} className="mb-3">
              {group.href && !group.items ? (
                <SheetClose asChild>
                  <Link href={group.href} className="block rounded-md px-3 py-2 text-base font-medium hover:bg-accent">
                    {group.title}
                  </Link>
                </SheetClose>
              ) : (
                <>
                  <div className="px-3 py-1 text-xs font-semibold uppercase tracking-wide text-muted-foreground">
                    {group.title}
                  </div>
                  {group.items?.map((item) => (
                    <SheetClose asChild key={item.href}>
                      <Link href={item.href} className="block rounded-md px-3 py-2 text-sm hover:bg-accent">
                        {item.title}
                      </Link>
                    </SheetClose>
                  ))}
                </>
              )}
            </div>
          ))}
        </nav>
        <div className="mt-4 flex flex-col gap-2 border-t border-border pt-4">
          <SheetClose asChild>
            <Button asChild>
              <Link href="/contact">Contact Sales</Link>
            </Button>
          </SheetClose>
          <SheetClose asChild>
            <Button asChild variant="outline">
              <Link href="/download">Download LocalHost</Link>
            </Button>
          </SheetClose>
        </div>
      </SheetContent>
    </Sheet>
  );
}
