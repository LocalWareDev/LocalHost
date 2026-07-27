export interface TimelineStep {
  phase: string;
  description: string;
}

export function Timeline({ steps }: { steps: TimelineStep[] }) {
  return (
    <ol className="relative space-y-10 border-l border-border pl-8">
      {steps.map((step, i) => (
        <li key={step.phase} className="relative">
          <span className="absolute -left-[calc(2rem+1px)] flex h-6 w-6 items-center justify-center rounded-full border border-primary bg-background text-xs font-semibold text-primary">
            {i + 1}
          </span>
          <h3 className="font-semibold">{step.phase}</h3>
          <p className="mt-1.5 text-sm text-muted-foreground">{step.description}</p>
        </li>
      ))}
    </ol>
  );
}
