import { CodeBlock } from "@/components/shared/code-block";
import type { DocPage } from "@/lib/data/docs";

export function DocContent({ page }: { page: DocPage }) {
  return (
    <article className="max-w-3xl">
      <h1 className="text-3xl font-semibold tracking-tight">{page.title}</h1>
      <p className="mt-3 text-lg text-muted-foreground">{page.description}</p>

      <div className="mt-10 space-y-10">
        {page.blocks.map((block) => (
          <section key={block.heading}>
            <h2 className="text-xl font-semibold">{block.heading}</h2>
            <div className="mt-3 space-y-3">
              {block.body.map((paragraph, i) => (
                <p key={i} className="leading-relaxed text-muted-foreground">
                  {paragraph}
                </p>
              ))}
            </div>
            {block.code && (
              <div className="mt-4">
                <CodeBlock label={block.code.label} code={block.code.content} />
              </div>
            )}
          </section>
        ))}
      </div>
    </article>
  );
}
