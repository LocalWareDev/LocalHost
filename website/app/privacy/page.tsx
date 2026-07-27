import type { Metadata } from "next";
import { Breadcrumbs } from "@/components/shared/breadcrumbs";

export const metadata: Metadata = {
  title: "Privacy Policy",
  description: "How LocalWare Corporation collects, uses, and protects information.",
};

const sections = [
  {
    title: "1. Information we collect",
    body: "We collect information you provide directly, such as account details, billing information, and support requests, as well as information collected automatically through product telemetry, which can be disabled in Enterprise deployments through the management console.",
  },
  {
    title: "2. How we use information",
    body: "Information is used to provide and improve LocalHost, process transactions, respond to support requests, and communicate product and security updates. We do not sell personal information to third parties.",
  },
  {
    title: "3. Data retention",
    body: "Account and billing information is retained for as long as your account is active and as required by applicable law. Enterprise customers can configure audit log retention independently, up to a maximum of seven years.",
  },
  {
    title: "4. Data security",
    body: "We apply encryption in transit and at rest for customer data, role-based access controls internally, and undergo periodic third-party security assessments aligned with SOC 2 requirements.",
  },
  {
    title: "5. Your rights",
    body: "Depending on your jurisdiction, you may have rights to access, correct, or delete personal information we hold about you. Requests can be submitted to privacy@localware.com.",
  },
  {
    title: "6. International transfers",
    body: "LocalWare Corporation operates globally. Where personal information is transferred internationally, we rely on appropriate legal safeguards such as standard contractual clauses.",
  },
  {
    title: "7. Changes to this policy",
    body: "We will post any changes to this policy on this page and, for material changes, notify account administrators directly.",
  },
  {
    title: "8. Contact",
    body: "Questions about this policy can be directed to privacy@localware.com.",
  },
];

export default function PrivacyPage() {
  return (
    <div className="container-prose py-16">
      <Breadcrumbs items={[{ title: "Privacy Policy" }]} />
      <h1 className="mt-6 text-4xl font-semibold tracking-tight">Privacy Policy</h1>
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
