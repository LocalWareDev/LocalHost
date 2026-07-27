import Link from "next/link";
import { ArrowRight } from "lucide-react";
import { Button } from "@/components/ui/button";

export function CtaSection() {
  return (
    <section className="border-t border-border bg-foreground text-background">
      <div className="container flex flex-col items-center gap-6 py-20 text-center">
        <h2 className="text-balance text-3xl font-semibold tracking-tight sm:text-4xl">
          Ready to standardize on LocalHost?
        </h2>
        <p className="max-w-xl text-background/70">
          Start free with LocalHost Community, or talk to our team about a fleet-wide Enterprise
          deployment.
        </p>
        <div className="flex flex-wrap items-center justify-center gap-3">
          <Button size="lg" variant="secondary" asChild>
            <Link href="/download">
              Download LocalHost
              <ArrowRight className="h-4 w-4" />
            </Link>
          </Button>
          <Button
            size="lg"
            variant="outline"
            className="border-background/30 bg-transparent text-background hover:bg-background/10 hover:text-background"
            asChild
          >
            <Link href="/contact">Contact Sales</Link>
          </Button>
        </div>
      </div>
    </section>
  );
}
