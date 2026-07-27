import type { Metadata } from "next";
import Link from "next/link";
import { ArrowRight, BookOpen, Bug, CheckCircle2, MessageSquare, Phone } from "lucide-react";
import { Button } from "@/components/ui/button";
import { Card, CardContent } from "@/components/ui/card";
import { Badge } from "@/components/ui/badge";
import { Breadcrumbs } from "@/components/shared/breadcrumbs";
import { SectionHeading } from "@/components/shared/section-heading";
import { FaqAccordion } from "@/components/shared/faq-accordion";

export const metadata: Metadata = {
  title: "Support",
  description: "LocalHost knowledge base, community, bug reports, support contact, and system status.",
};

const channels = [
  {
    icon: BookOpen,
    title: "Knowledge base",
    description: "Searchable articles covering installation, licensing, and troubleshooting.",
    href: "/documentation",
    cta: "Browse articles",
  },
  {
    icon: MessageSquare,
    title: "Community forum",
    description: "Ask questions and share configurations with other LocalHost users.",
    href: "#",
    cta: "Visit the forum",
  },
  {
    icon: Bug,
    title: "Bug reports",
    description: "File a reproducible issue against a specific LocalHost version.",
    href: "#",
    cta: "Report a bug",
  },
  {
    icon: Phone,
    title: "Contact support",
    description: "Reach our support team directly for account or licensing issues.",
    href: "/contact",
    cta: "Contact support",
  },
];

const systemComponents = [
  { name: "Management Console (SaaS)", status: "Operational" },
  { name: "License & Activation Service", status: "Operational" },
  { name: "Download CDN", status: "Operational" },
  { name: "Documentation Site", status: "Operational" },
  { name: "REST API", status: "Operational" },
];

const faqs = [
  {
    question: "What are your support hours?",
    answer: "Community support is available at all times via the forum. Professional plans include priority email support during business hours; Enterprise includes 24/7/365 coverage.",
  },
  {
    question: "How do I escalate a critical production issue?",
    answer: "Enterprise customers can escalate through their dedicated technical account manager or the emergency line included in their onboarding documentation.",
  },
  {
    question: "Where do I report a security vulnerability?",
    answer: "Email security@localware.com with details. We follow a coordinated disclosure process and will acknowledge reports within two business days.",
  },
];

export default function SupportPage() {
  return (
    <div className="pb-24">
      <section className="border-b border-border bg-muted/20">
        <div className="container py-16">
          <Breadcrumbs items={[{ title: "Support" }]} />
          <h1 className="mt-6 max-w-2xl text-balance text-4xl font-semibold tracking-tight sm:text-5xl">
            Support
          </h1>
          <p className="mt-4 max-w-2xl text-lg text-muted-foreground">
            Find answers in the knowledge base, ask the community, or reach our support team directly.
          </p>
        </div>
      </section>

      <section className="container py-16">
        <div className="grid grid-cols-1 gap-5 sm:grid-cols-2 lg:grid-cols-4">
          {channels.map((channel) => (
            <Card key={channel.title} className="flex h-full flex-col">
              <CardContent className="flex h-full flex-col p-6">
                <div className="flex h-10 w-10 items-center justify-center rounded-md bg-primary/10 text-primary">
                  <channel.icon className="h-5 w-5" />
                </div>
                <h2 className="mt-4 font-semibold">{channel.title}</h2>
                <p className="mt-2 flex-1 text-sm text-muted-foreground">{channel.description}</p>
                <Button variant="outline" size="sm" className="mt-4 self-start" asChild>
                  <Link href={channel.href}>
                    {channel.cta}
                    <ArrowRight className="h-3.5 w-3.5" />
                  </Link>
                </Button>
              </CardContent>
            </Card>
          ))}
        </div>
      </section>

      <section id="status" className="container scroll-mt-24 py-8">
        <SectionHeading eyebrow="Status" title="System status" className="mb-8" />
        <Card>
          <CardContent className="p-0">
            <ul className="divide-y divide-border">
              {systemComponents.map((component) => (
                <li key={component.name} className="flex items-center justify-between px-6 py-4">
                  <span className="text-sm font-medium">{component.name}</span>
                  <Badge variant="success" className="gap-1">
                    <CheckCircle2 className="h-3 w-3" />
                    {component.status}
                  </Badge>
                </li>
              ))}
            </ul>
          </CardContent>
        </Card>
        <p className="mt-3 text-xs text-muted-foreground">
          Status shown reflects the last automated check. Historical incident reports are published on
          the status page.
        </p>
      </section>

      <section className="container py-20">
        <SectionHeading eyebrow="Common questions" title="Support FAQ" className="mb-8" />
        <div className="mx-auto max-w-2xl">
          <FaqAccordion items={faqs} />
        </div>
      </section>
    </div>
  );
}
