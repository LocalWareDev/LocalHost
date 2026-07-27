import { Quote } from "lucide-react";
import { Card, CardContent } from "@/components/ui/card";
import type { Testimonial } from "@/lib/data/testimonials";

export function TestimonialCard({ testimonial }: { testimonial: Testimonial }) {
  return (
    <Card className="flex h-full flex-col justify-between">
      <CardContent className="p-8">
        <Quote className="h-6 w-6 text-primary/40" />
        <p className="mt-4 text-balance text-base leading-relaxed text-foreground/90">
          "{testimonial.quote}"
        </p>
        <div className="mt-6 border-t border-border pt-4">
          <p className="text-sm font-semibold">{testimonial.title}</p>
          <p className="text-sm text-muted-foreground">{testimonial.company}</p>
        </div>
      </CardContent>
    </Card>
  );
}
