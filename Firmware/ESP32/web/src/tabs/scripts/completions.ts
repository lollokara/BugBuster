import { Completion, CompletionSource, snippetCompletion } from "@codemirror/autocomplete";
import { generatedCompletions, type GeneratedCompletion } from "./completions.generated";

/**
 * Editor completions for the on-device scripting API. The table is generated from the
 * firmware's .pyi stubs (python/tools/stubs_to_json.py), so names, signatures and
 * docs cannot drift from the firmware: edit the stubs, not this file.
 */
export function toCompletion(entry: GeneratedCompletion): Completion {
  const options = { label: entry.label, detail: entry.detail, type: entry.type, info: entry.info || undefined };
  return entry.snippet === entry.label ? options : snippetCompletion(entry.snippet, options);
}

export const bugbusterCompletionOptions: Completion[] = generatedCompletions.map(toCompletion);

export const bugbusterCompletions: CompletionSource = (context) => {
  // Match the word being typed, including dots for module access
  const word = context.matchBefore(/[\w.]*/);
  if (!word || (word.from === word.to && !context.explicit)) return null;

  return {
    from: word.from,
    options: bugbusterCompletionOptions,
    validFor: /^[\w.]*$/,
  };
};
