import { Accordion, AccordionContent, AccordionItem, AccordionTrigger } from "@/components/ui/accordion";

export interface FaqItem {
  question: string;
  answer: string;
}

export function FaqAccordion({ items }: { items: FaqItem[] }) {
  return (
    <Accordion type="single" collapsible className="w-full">
      {items.map((item, i) => (
        <AccordionItem key={i} value={`item-${i}`}>
          <AccordionTrigger className="text-base">{item.question}</AccordionTrigger>
          <AccordionContent className="text-base">{item.answer}</AccordionContent>
        </AccordionItem>
      ))}
    </Accordion>
  );
}
