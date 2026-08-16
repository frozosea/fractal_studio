"use client";

import ReactMarkdown from "react-markdown";
import remarkGfm from "remark-gfm";

/**
 * Renders assistant replies as GitHub-flavored Markdown.
 *
 * react-markdown does not render raw HTML by default and applies a URL
 * transform that rejects dangerous schemes (javascript:, data:, ...), so
 * model output can never inject markup or scripts. Links are forced to open
 * in a new tab with noopener.
 */
export function MarkdownMessage({ content }: { content: string }) {
  return (
    <div className="md-message break-words text-sm leading-6">
      <ReactMarkdown
        remarkPlugins={[remarkGfm]}
        components={{
          a: ({ children, ...props }) => (
            <a {...props} target="_blank" rel="noopener noreferrer" className="text-brand underline decoration-brand/40 underline-offset-2 hover:text-brand/90">
              {children}
            </a>
          ),
          p: ({ children }) => <p className="mb-2 last:mb-0">{children}</p>,
          ul: ({ children }) => <ul className="mb-2 list-disc space-y-1 pl-5 last:mb-0">{children}</ul>,
          ol: ({ children }) => <ol className="mb-2 list-decimal space-y-1 pl-5 last:mb-0">{children}</ol>,
          li: ({ children }) => <li className="leading-6">{children}</li>,
          h1: ({ children }) => <h1 className="mb-2 mt-3 text-base font-medium text-ink/90 first:mt-0 last:mb-0">{children}</h1>,
          h2: ({ children }) => <h2 className="mb-2 mt-3 text-[15px] font-medium text-ink/90 first:mt-0 last:mb-0">{children}</h2>,
          h3: ({ children }) => <h3 className="mb-2 mt-3 text-sm font-medium text-ink/90 first:mt-0 last:mb-0">{children}</h3>,
          h4: ({ children }) => <h4 className="mb-2 mt-3 text-sm font-medium first:mt-0 last:mb-0">{children}</h4>,
          h5: ({ children }) => <h5 className="mb-2 mt-3 text-sm font-medium first:mt-0 last:mb-0">{children}</h5>,
          h6: ({ children }) => <h6 className="mb-2 mt-3 text-sm font-medium first:mt-0 last:mb-0">{children}</h6>,
          strong: ({ children }) => <strong className="font-semibold text-ink/90">{children}</strong>,
          em: ({ children }) => <em className="italic">{children}</em>,
          blockquote: ({ children }) => (
            <blockquote className="mb-2 border-l-2 border-brand/30 pl-3 text-ink/60 last:mb-0">{children}</blockquote>
          ),
          hr: () => <hr className="mb-2 mt-3 border-instrument-rule last:mb-0" />,
          table: ({ children }) => (
            <div className="mb-2 overflow-x-auto last:mb-0">
              <table className="w-full border-collapse text-xs">{children}</table>
            </div>
          ),
          thead: ({ children }) => <thead className="border-b border-instrument-rule">{children}</thead>,
          th: ({ children }) => <th className="border border-instrument-rule px-2 py-1 text-left font-medium">{children}</th>,
          td: ({ children }) => <td className="border border-instrument-rule px-2 py-1">{children}</td>,
          code: ({ className, children }) => {
            const inline = !className;
            return inline ? (
              <code className="rounded-sm border border-instrument-rule bg-black/25 px-1 py-0.5 font-mono text-[0.85em] text-ink/85">
                {children}
              </code>
            ) : (
              <code className={className}>{children}</code>
            );
          },
          pre: ({ children }) => (
            <pre className="mb-2 overflow-x-auto rounded-sm border border-instrument-rule bg-black/30 p-3 font-mono text-xs leading-5 last:mb-0">
              {children}
            </pre>
          ),
        }}
      >
        {content}
      </ReactMarkdown>
    </div>
  );
}
