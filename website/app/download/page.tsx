import type { Metadata } from "next";
import Link from "next/link";
import { ArrowRight } from "lucide-react";
import { Badge } from "@/components/ui/badge";
import { Button } from "@/components/ui/button";
import { Table, TableBody, TableCell, TableHead, TableHeader, TableRow } from "@/components/ui/table";
import { Breadcrumbs } from "@/components/shared/breadcrumbs";
import { SectionHeading } from "@/components/shared/section-heading";
import { DownloadCard } from "@/components/shared/download-card";
import { CodeBlock } from "@/components/shared/code-block";
import { releaseChannels, versionHistory, downloadPlatforms, checksumSample } from "@/lib/data/downloads";

export const metadata: Metadata = {
  title: "Download Center",
  description: "Download LocalHost stable, beta, and nightly builds. Checksums, version history, and installation guides.",
};

const channelVariant: Record<string, "success" | "secondary" | "outline"> = {
  Stable: "success",
  Beta: "secondary",
  Nightly: "outline",
};

export default function DownloadPage() {
  return (
    <div className="pb-24">
      <section className="border-b border-border bg-muted/20">
        <div className="container py-16">
          <Breadcrumbs items={[{ title: "Download Center" }]} />
          <h1 className="mt-6 max-w-2xl text-balance text-4xl font-semibold tracking-tight sm:text-5xl">
            Download Center
          </h1>
          <p className="mt-4 max-w-2xl text-lg text-muted-foreground">
            Stable, beta, and nightly builds for every supported platform, with checksums and full
            version history.
          </p>
        </div>
      </section>

      <section className="container py-16">
        <SectionHeading eyebrow="Release channels" title="Choose your channel" className="mb-8" />
        <div className="grid grid-cols-1 gap-4 sm:grid-cols-3">
          {releaseChannels.map((release) => (
            <div key={release.channel} className="rounded-lg border border-border p-6">
              <div className="flex items-center justify-between">
                <Badge variant={channelVariant[release.channel]}>{release.channel}</Badge>
                <span className="text-xs text-muted-foreground">{release.date}</span>
              </div>
              <p className="mt-4 font-mono text-lg font-semibold">{release.version}</p>
              <p className="mt-2 text-sm text-muted-foreground">{release.description}</p>
            </div>
          ))}
        </div>
      </section>

      <section className="container py-8">
        <SectionHeading eyebrow="Platforms" title="Download LocalHost Workstation" className="mb-8" />
        <div className="grid grid-cols-1 gap-3 sm:grid-cols-2">
          {downloadPlatforms.map((platform) => (
            <DownloadCard
              key={platform.name}
              platform={platform.name}
              format={platform.format}
              requirement={platform.requirement}
            />
          ))}
        </div>
        <p className="mt-4 text-sm text-muted-foreground">
          Looking for Enterprise, Hypervisor, or Education downloads? Visit the individual{" "}
          <Link href="/products" className="text-primary hover:underline">
            product pages
          </Link>
          .
        </p>
      </section>

      <section className="container py-16">
        <SectionHeading eyebrow="Verify" title="Checksums" className="mb-6" />
        <CodeBlock label="SHA-256 checksums" code={checksumSample} />
      </section>

      <section className="container py-8">
        <SectionHeading eyebrow="History" title="Version history" className="mb-8" />
        <div className="overflow-hidden rounded-lg border border-border">
          <Table>
            <TableHeader>
              <TableRow>
                <TableHead>Version</TableHead>
                <TableHead>Date</TableHead>
                <TableHead>Type</TableHead>
                <TableHead>Summary</TableHead>
              </TableRow>
            </TableHeader>
            <TableBody>
              {versionHistory.map((entry) => (
                <TableRow key={entry.version}>
                  <TableCell className="font-mono font-medium">{entry.version}</TableCell>
                  <TableCell className="text-muted-foreground">{entry.date}</TableCell>
                  <TableCell>
                    <Badge variant={entry.type === "Security" ? "success" : "outline"}>{entry.type}</Badge>
                  </TableCell>
                  <TableCell className="text-muted-foreground">{entry.summary}</TableCell>
                </TableRow>
              ))}
            </TableBody>
          </Table>
        </div>
      </section>

      <section className="container py-16 text-center">
        <h2 className="text-2xl font-semibold">Need step-by-step installation help?</h2>
        <Button className="mt-5" variant="outline" asChild>
          <Link href="/documentation/installation">
            Read the Installation Guide
            <ArrowRight className="h-4 w-4" />
          </Link>
        </Button>
      </section>
    </div>
  );
}
