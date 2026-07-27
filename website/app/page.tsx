import { buildMetadata } from "@/lib/seo";
import { Hero } from "@/components/sections/hero";
import { ProductOverview } from "@/components/sections/product-overview";
import { FeatureHighlights } from "@/components/sections/feature-highlights";
import { EnterpriseTeaser } from "@/components/sections/enterprise-teaser";
import { PerformanceSection } from "@/components/sections/performance-section";
import { SecuritySection } from "@/components/sections/security-section";
import { ArchitectureOverview } from "@/components/sections/architecture-overview";
import { OsCompatibility } from "@/components/sections/os-compatibility";
import { CustomerLogos } from "@/components/sections/customer-logos";
import { Testimonials } from "@/components/sections/testimonials";
import { CtaSection } from "@/components/sections/cta-section";

export const metadata = buildMetadata({
  title: "Enterprise Virtualization Platform",
  description:
    "LocalHost is an enterprise virtualization platform for organizations that require reliability, security, and performance at scale.",
  path: "/",
});

export default function HomePage() {
  return (
    <>
      <Hero />
      <CustomerLogos />
      <ProductOverview />
      <FeatureHighlights />
      <EnterpriseTeaser />
      <PerformanceSection />
      <SecuritySection />
      <ArchitectureOverview />
      <OsCompatibility />
      <Testimonials />
      <CtaSection />
    </>
  );
}
