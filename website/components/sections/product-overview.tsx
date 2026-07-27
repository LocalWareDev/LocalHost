import Link from "next/link";
import { ArrowRight } from "lucide-react";
import { Card, CardContent } from "@/components/ui/card";
import { SectionHeading } from "@/components/shared/section-heading";
import { products } from "@/lib/data/products";

export function ProductOverview() {
  return (
    <section className="container py-20">
      <SectionHeading
        eyebrow="Product line"
        title="One platform, four ways to run it"
        description="From a single engineer's laptop to a fleet of thousands of hosts, LocalHost scales down and up without changing how a virtual machine behaves."
        className="mb-12"
      />
      <div className="grid grid-cols-1 gap-5 sm:grid-cols-2 lg:grid-cols-4">
        {products.map((product) => (
          <Link key={product.slug} href={`/products/${product.slug}`}>
            <Card className="h-full transition-all hover:-translate-y-0.5 hover:border-primary/40 hover:shadow-md">
              <CardContent className="flex h-full flex-col p-6">
                <h3 className="font-semibold">{product.shortName}</h3>
                <p className="mt-2 flex-1 text-sm text-muted-foreground">{product.description}</p>
                <span className="mt-4 inline-flex items-center gap-1 text-sm font-medium text-primary">
                  Learn more <ArrowRight className="h-3.5 w-3.5" />
                </span>
              </CardContent>
            </Card>
          </Link>
        ))}
      </div>
    </section>
  );
}
