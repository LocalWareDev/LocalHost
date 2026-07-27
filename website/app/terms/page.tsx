import type { Metadata } from "next";
import { Breadcrumbs } from "@/components/shared/breadcrumbs";

export const metadata: Metadata = {
  title: "Terms of Service",
  description: "Terms governing use of LocalHost and LocalWare Corporation services.",
};

const sections = [
  {
    title: "1. Acceptance of terms",
    body: "By downloading, installing, or using LocalHost, you agree to be bound by these Terms of Service and any applicable license agreement for your edition of the product.",
  },
  {
    title: "2. License grant",
    body: "Subject to payment of applicable fees, LocalWare Corporation grants you a non-exclusive, non-transferable license to use LocalHost in accordance with the licensing model described on the Pricing page for your selected plan.",
  },
  {
    title: "3. Acceptable use",
    body: "You may not reverse engineer, sublicense, or use LocalHost to build a competing virtualization product. Community edition usage is limited to the seat and VM limits described in its license terms.",
  },
  {
    title: "4. Service availability",
    body: "For hosted management plane services, LocalWare Corporation targets the uptime commitments specified in your Enterprise agreement. Scheduled maintenance will be communicated in advance where practical.",
  },
  {
    title: "5. Warranties and disclaimers",
    body: "LocalHost is provided on an 'as is' basis except as expressly warranted in a signed Enterprise agreement. LocalWare Corporation disclaims implied warranties to the maximum extent permitted by law.",
  },
  {
    title: "6. Limitation of liability",
    body: "To the extent permitted by law, LocalWare Corporation's aggregate liability arising from these terms will not exceed the fees paid by you in the twelve months preceding the claim.",
  },
  {
    title: "7. Termination",
    body: "We may suspend or terminate access for material breach of these terms. Upon termination, license rights to LocalHost cease, subject to any data export provisions in your agreement.",
  },
  {
    title: "8. Governing law",
    body: "These terms are governed by the laws of the State of Delaware, without regard to conflict of law principles, unless superseded by a signed enterprise agreement.",
  },
  {
    title: "9. Contact",
    body: "Questions about these terms can be directed to legal@localware.com.",
  },
];

export default function TermsPage() {
  return (
    <div className="container-prose py-16">
      <Breadcrumbs items={[{ title: "Terms of Service" }]} />
      <h1 className="mt-6 text-4xl font-semibold tracking-tight">Terms of Service</h1>
      <p className="mt-3 text-sm text-muted-foreground">Last updated: July 1, 2026</p>

      <div className="mt-10 space-y-8">
        {sections.map((section) => (
          <section key={section.title}>
            <h2 className="text-lg font-semibold">{section.title}</h2>
            <p className="mt-2 leading-relaxed text-muted-foreground">{section.body}</p>
          </section>
        ))}
      </div>
    </div>
  );
}
