import Link from "next/link";
import { ArrowRight, CheckCircle2 } from "lucide-react";
import { Button } from "@/components/ui/button";
import { Card, CardContent } from "@/components/ui/card";
import { Badge } from "@/components/ui/badge";
import { Table, TableBody, TableCell, TableHead, TableHeader, TableRow } from "@/components/ui/table";
import { Breadcrumbs } from "@/components/shared/breadcrumbs";
import { SectionHeading } from "@/components/shared/section-heading";
import { FeatureGrid } from "@/components/shared/feature-grid";
import { ScreenshotPlaceholder } from "@/components/shared/screenshot-placeholder";
import { DownloadCard } from "@/components/shared/download-card";
import type { Product } from "@/lib/data/products";
import { products } from "@/lib/data/products";

export function ProductPageTemplate({ product }: { product: Product }) {
  const otherProducts = products.filter((p) => p.slug !== product.slug);

  return (
    <div className="pb-24">
      {/* Hero */}
      <section className="border-b border-border bg-muted/20">
        <div className="container py-16 sm:py-20">
          <Breadcrumbs items={[{ title: "Products", href: "/products" }, { title: product.shortName }]} />
          <div className="mt-6 max-w-3xl">
            <Badge variant="outline">{product.shortName}</Badge>
            <h1 className="mt-4 text-balance text-4xl font-semibold tracking-tight sm:text-5xl">
              {product.name}
            </h1>
            <p className="mt-3 text-xl text-muted-foreground">{product.tagline}</p>
            <p className="mt-5 text-base leading-relaxed text-muted-foreground">{product.longDescription}</p>
            <div className="mt-8 flex flex-wrap gap-3">
              <Button size="lg" asChild>
                <Link href="/download">
                  Download {product.shortName}
                  <ArrowRight className="h-4 w-4" />
                </Link>
              </Button>
              <Button size="lg" variant="outline" asChild>
                <Link href="/contact">Talk to sales</Link>
              </Button>
            </div>
            <p className="mt-4 text-sm text-muted-foreground">Built for: {product.audience}</p>
          </div>
        </div>
      </section>

      {/* Screenshot */}
      <section className="container py-16">
        <ScreenshotPlaceholder label={`${product.name} — console`} />
      </section>

      {/* Features */}
      <section className="container py-8">
        <SectionHeading eyebrow="Capabilities" title="What's included" className="mb-10" />
        <FeatureGrid items={product.features} />
      </section>

      {/* Specs */}
      <section className="container py-16">
        <SectionHeading eyebrow="Specifications" title="Technical specifications" className="mb-8" />
        <Card>
          <CardContent className="p-0">
            <Table>
              <TableBody>
                {product.specs.map((spec) => (
                  <TableRow key={spec.label}>
                    <TableCell className="w-1/2 font-medium">{spec.label}</TableCell>
                    <TableCell className="text-muted-foreground">{spec.value}</TableCell>
                  </TableRow>
                ))}
              </TableBody>
            </Table>
          </CardContent>
        </Card>
      </section>

      {/* Comparison */}
      <section className="container py-8">
        <SectionHeading eyebrow="Compare" title="How it fits alongside other LocalHost products" className="mb-8" />
        <div className="overflow-hidden rounded-lg border border-border">
          <Table>
            <TableHeader>
              <TableRow>
                <TableHead>Product</TableHead>
                <TableHead>Best for</TableHead>
                <TableHead>Licensing model</TableHead>
              </TableRow>
            </TableHeader>
            <TableBody>
              <TableRow className="bg-primary/5">
                <TableCell className="font-medium">{product.name} (this page)</TableCell>
                <TableCell className="text-muted-foreground">{product.audience}</TableCell>
                <TableCell className="text-muted-foreground">{product.licensing.model}</TableCell>
              </TableRow>
              {otherProducts.map((p) => (
                <TableRow key={p.slug}>
                  <TableCell className="font-medium">
                    <Link href={`/products/${p.slug}`} className="hover:underline">
                      {p.name}
                    </Link>
                  </TableCell>
                  <TableCell className="text-muted-foreground">{p.audience}</TableCell>
                  <TableCell className="text-muted-foreground">{p.licensing.model}</TableCell>
                </TableRow>
              ))}
            </TableBody>
          </Table>
        </div>
      </section>

      {/* Licensing */}
      <section className="container py-8">
        <SectionHeading eyebrow="Licensing" title={product.licensing.model} className="mb-6" />
        <ul className="space-y-3">
          {product.licensing.details.map((detail) => (
            <li key={detail} className="flex items-start gap-2 text-sm text-muted-foreground">
              <CheckCircle2 className="mt-0.5 h-4 w-4 shrink-0 text-primary" />
              {detail}
            </li>
          ))}
        </ul>
      </section>

      {/* Requirements */}
      <section className="container py-16">
        <SectionHeading eyebrow="System requirements" title="What you'll need" className="mb-8" />
        <div className="overflow-hidden rounded-lg border border-border">
          <Table>
            <TableHeader>
              <TableRow>
                <TableHead>Platform</TableHead>
                <TableHead>CPU</TableHead>
                <TableHead>Memory</TableHead>
                <TableHead>Storage</TableHead>
                <TableHead>Notes</TableHead>
              </TableRow>
            </TableHeader>
            <TableBody>
              {product.requirements.map((req) => (
                <TableRow key={req.platform}>
                  <TableCell className="font-medium">{req.platform}</TableCell>
                  <TableCell className="text-muted-foreground">{req.cpu}</TableCell>
                  <TableCell className="text-muted-foreground">{req.memory}</TableCell>
                  <TableCell className="text-muted-foreground">{req.storage}</TableCell>
                  <TableCell className="text-muted-foreground">{req.notes}</TableCell>
                </TableRow>
              ))}
            </TableBody>
          </Table>
        </div>
      </section>

      {/* Downloads */}
      <section className="container py-8">
        <SectionHeading eyebrow="Download" title={`Get ${product.name}`} className="mb-8" />
        <div className="grid grid-cols-1 gap-3 sm:grid-cols-2">
          {product.downloads.map((dl) => (
            <DownloadCard key={dl.platform} platform={dl.platform} format={dl.format} size={dl.size} />
          ))}
        </div>
        <p className="mt-4 text-sm text-muted-foreground">
          Need checksums or older versions? Visit the{" "}
          <Link href="/download" className="text-primary hover:underline">
            Download Center
          </Link>
          .
        </p>
      </section>
    </div>
  );
}
