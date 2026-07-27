import { Breadcrumbs } from "@/components/shared/breadcrumbs";
import { SectionHeading } from "@/components/shared/section-heading";
import { PricingCard } from "@/components/shared/pricing-card";
import { ComparisonTable } from "@/components/shared/comparison-table";
import { FaqAccordion } from "@/components/shared/faq-accordion";
import { pricingPlans, pricingComparison } from "@/lib/data/pricing";
import { buildMetadata } from "@/lib/seo";

export const metadata = buildMetadata({
  title: "Pricing",
  description: "Community, Professional, and Enterprise pricing for LocalHost virtualization.",
  path: "/pricing",
});

const faqs = [
  {
    question: "Can I switch plans later?",
    answer: "Yes. You can upgrade from Community to Professional at any time, and Enterprise agreements can incorporate existing Professional seats.",
  },
  {
    question: "Is there a free trial of Professional or Enterprise?",
    answer: "Yes, a 30-day evaluation license is available for both plans. Contact sales to activate a trial for your team.",
  },
  {
    question: "How is Enterprise pricing calculated?",
    answer: "Enterprise is priced per managed host rather than per seat, since fleet management scales with infrastructure, not headcount. Contact sales for a quote based on your environment.",
  },
  {
    question: "Do you offer academic or nonprofit pricing?",
    answer: "Yes. See LocalHost Education for institutional site licensing, and contact sales for nonprofit discounts.",
  },
];

export default function PricingPage() {
  return (
    <div className="pb-24">
      <section className="border-b border-border bg-muted/20">
        <div className="container py-16 text-center sm:py-20">
          <Breadcrumbs items={[{ title: "Pricing" }]} />
          <h1 className="mx-auto mt-6 max-w-2xl text-balance text-4xl font-semibold tracking-tight sm:text-5xl">
            Simple pricing that scales with your fleet
          </h1>
          <p className="mx-auto mt-4 max-w-xl text-lg text-muted-foreground">
            Start free with Community. Move to Professional for advanced features, or Enterprise for
            fleet-wide management and compliance.
          </p>
        </div>
      </section>

      <section className="container py-16">
        <div className="grid grid-cols-1 gap-6 lg:grid-cols-3">
          {pricingPlans.map((plan) => (
            <PricingCard key={plan.id} plan={plan} />
          ))}
        </div>
      </section>

      <section className="container py-8">
        <SectionHeading eyebrow="Compare plans" title="Full feature comparison" className="mb-8" />
        <ComparisonTable groups={pricingComparison} />
      </section>

      <section className="container py-20">
        <SectionHeading eyebrow="Questions" title="Pricing FAQ" className="mb-8" />
        <div className="mx-auto max-w-2xl">
          <FaqAccordion items={faqs} />
        </div>
      </section>
    </div>
  );
}
