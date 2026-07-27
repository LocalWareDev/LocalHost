import Link from "next/link";
import { ArrowRight, SearchX } from "lucide-react";
import { Button } from "@/components/ui/button";

export default function NotFound() {
  return (
    <div className="container flex min-h-[70vh] flex-col items-center justify-center py-24 text-center">
      <div className="flex h-14 w-14 items-center justify-center rounded-full bg-primary/10 text-primary">
        <SearchX className="h-6 w-6" />
      </div>
      <p className="mt-6 font-mono text-sm text-muted-foreground">Error 404</p>
      <h1 className="mt-2 text-balance text-3xl font-semibold tracking-tight sm:text-4xl">
        This page doesn't exist in the current build
      </h1>
      <p className="mt-3 max-w-md text-muted-foreground">
        The page you're looking for may have moved, been renamed, or never existed. Check the URL or
        head back to the homepage.
      </p>
      <div className="mt-8 flex flex-wrap items-center justify-center gap-3">
        <Button asChild>
          <Link href="/">
            Back to homepage
            <ArrowRight className="h-4 w-4" />
          </Link>
        </Button>
        <Button variant="outline" asChild>
          <Link href="/documentation">Browse documentation</Link>
        </Button>
      </div>
    </div>
  );
}
