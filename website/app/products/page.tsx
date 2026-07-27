import Link from "next/link";
import { ArrowRight } from "lucide-react";
import { Button } from "@/components/ui/button";
import { Card, CardContent } from "@/components/ui/card";
import { Table, TableBody, TableCell, TableHead, TableHeader, TableRow } from "@/components/ui/table";
import { Breadcrumbs } from "@/components/shared/breadcrumbs";
import { SectionHeading } from "@/components/shared/section-heading";
import { products } from "@/lib/data/products";
import { buildMetadata } from "@/lib/seo";

export const metadata = buildMetadata({
  title: "Products",
  description: "LocalHost Workstation, Enterprise, Hypervisor, and Education — one virtualization engine, four deployment models.",
  path: "/products",
});

export default function ProductsPage() {
  return (
    <div className="pb-24">
      <section className="border-b border-border bg-muted/20">
        <div className="container py-16">
          <Breadcrumbs items={[{ title: "Products" }]} />
          <h1 className="mt-6 max-w-2xl text-balance text-4xl font-semibold tracking-tight sm:text-5xl">
            One platform. Four ways to deploy it.
          </h1>
          <p className="mt-4 max-w-2xl text-lg text-muted-foreground">
            Every LocalHost product shares the same virtualization engine, so a VM behaves identically
            whether it's running on a laptop or across a fleet of production hosts.
          </p>
        </div>
      </section>

      <section className="container py-16">
        <div className="grid grid-cols-1 gap-6 lg:grid-cols-2">
          {products.map((product) => (
            <Card key={product.slug} className="flex h-full flex-col">
              <CardContent className="flex h-full flex-col p-8">
                <h2 className="text-xl font-semibold">{product.name}</h2>
                <p className="mt-1 text-sm font-medium text-primary">{product.tagline}</p>
                <p className="mt-4 flex-1 text-sm text-muted-foreground">{product.description}</p>
                <p className="mt-4 text-xs uppercase tracking-wide text-muted-foreground">
                  {product.audience}
                </p>
                <Button className="mt-6 self-start" variant="outline" asChild>
                  <Link href={`/products/${product.slug}`}>
                    View {product.shortName}
                    <ArrowRight className="h-4 w-4" />
                  </Link>
                </Button>
              </CardContent>
            </Card>
          ))}
        </div>
      </section>

      <section className="container py-8">
        <SectionHeading eyebrow="At a glance" title="Comparing the product line" className="mb-8" />
        <div className="overflow-hidden rounded-lg border border-border">
          <Table>
            <TableHeader>
              <TableRow>
                <TableHead>Product</TableHead>
                <TableHead>Deployment</TableHead>
                <TableHead>Licensing model</TableHead>
              </TableRow>
            </TableHeader>
            <TableBody>
              {products.map((product) => (
                <TableRow key={product.slug}>
                  <TableCell className="font-medium">
                    <Link href={`/products/${product.slug}`} className="hover:underline">
                      {product.name}
                    </Link>
                  </TableCell>
                  <TableCell className="text-muted-foreground">{product.audience}</TableCell>
                  <TableCell className="text-muted-foreground">{product.licensing.model}</TableCell>
                </TableRow>
              ))}
            </TableBody>
          </Table>
        </div>
      </section>
    </div>
  );
}
