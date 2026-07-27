"use client";

import * as React from "react";
import { useRouter } from "next/navigation";
import { FileText, Search } from "lucide-react";
import {
  CommandDialog,
  CommandEmpty,
  CommandGroup,
  CommandInput,
  CommandItem,
  CommandList,
} from "@/components/ui/command";
import { Button } from "@/components/ui/button";
import { commandItems } from "@/lib/data/nav";

export function CommandPalette() {
  const [open, setOpen] = React.useState(false);
  const router = useRouter();

  React.useEffect(() => {
    function onKeyDown(e: KeyboardEvent) {
      if ((e.key === "k" && (e.metaKey || e.ctrlKey)) || e.key === "/") {
        if (e.key === "/" && (e.target as HTMLElement)?.tagName === "INPUT") return;
        e.preventDefault();
        setOpen((prev) => !prev);
      }
    }
    document.addEventListener("keydown", onKeyDown);
    return () => document.removeEventListener("keydown", onKeyDown);
  }, []);

  const groups = React.useMemo(() => {
    const map = new Map<string, typeof commandItems>();
    for (const item of commandItems) {
      const list = map.get(item.group) ?? [];
      list.push(item);
      map.set(item.group, list);
    }
    return Array.from(map.entries());
  }, []);

  const go = (href: string) => {
    setOpen(false);
    router.push(href);
  };

  return (
    <>
      <Button
        variant="outline"
        size="sm"
        onClick={() => setOpen(true)}
        className="hidden h-9 items-center gap-2 text-muted-foreground md:inline-flex"
      >
        <Search className="h-3.5 w-3.5" />
        Search
        <kbd className="ml-2 rounded border border-border bg-muted px-1.5 py-0.5 text-[10px] font-medium">
          Ctrl K
        </kbd>
      </Button>
      <Button
        variant="ghost"
        size="icon"
        onClick={() => setOpen(true)}
        aria-label="Search"
        className="md:hidden"
      >
        <Search className="h-4 w-4" />
      </Button>
      <CommandDialog open={open} onOpenChange={setOpen}>
        <CommandInput placeholder="Search pages, products, and documentation…" />
        <CommandList>
          <CommandEmpty>No results found.</CommandEmpty>
          {groups.map(([group, items]) => (
            <CommandGroup key={group} heading={group}>
              {items.map((item) => (
                <CommandItem key={item.href} value={item.title} onSelect={() => go(item.href)}>
                  <FileText className="mr-2 h-4 w-4 text-muted-foreground" />
                  {item.title}
                </CommandItem>
              ))}
            </CommandGroup>
          ))}
        </CommandList>
      </CommandDialog>
    </>
  );
}
