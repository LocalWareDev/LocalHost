import { SectionHeading } from "@/components/shared/section-heading";
import { TestimonialCard } from "@/components/shared/testimonial-card";
import { testimonials } from "@/lib/data/testimonials";

export function Testimonials() {
  return (
    <section className="container py-20">
      <SectionHeading
        eyebrow="Customers"
        title="What infrastructure teams say after migrating"
        align="center"
        className="mb-12"
      />
      <div className="grid grid-cols-1 gap-6 lg:grid-cols-3">
        {testimonials.map((t) => (
          <TestimonialCard key={t.company} testimonial={t} />
        ))}
      </div>
    </section>
  );
}
