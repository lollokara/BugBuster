import SwiftUI
import UIKit

/// Searchable reference for the on-device MicroPython API (spec §4 "Docs browser").
/// Presented as a sheet from the editor. "Insert" hands the snippet and its
/// required import to the editor, then dismisses.
struct ScriptDocsBrowser: View {
    let catalogue: FirmwareAPICatalogue?
    var onInsert: ((_ snippet: String, _ requiredImport: String?) -> Void)?

    @Environment(\.dismiss) private var dismiss
    @State private var query = ""
    @State private var entries: [FirmwareDocEntry] = []

    var body: some View {
        NavigationStack {
            Group {
                if catalogue == nil {
                    ContentUnavailableView("API docs unavailable", systemImage: "book.closed",
                                           description: Text("firmware_api.json is missing from this build."))
                } else {
                    let sections = FirmwareDocs.sections(entries, query: query)
                    List {
                        ForEach(sections) { section in
                            Section(section.title) {
                                ForEach(section.entries) { entry in
                                    NavigationLink(value: entry) { ScriptDocRow(entry: entry) }
                                        .accessibilityIdentifier("doc_row_\(entry.title)")
                                }
                            }
                        }
                    }
                    .accessibilityIdentifier("docs_browser_list")
                    .listStyle(.insetGrouped)
                    .overlay {
                        if sections.isEmpty { ContentUnavailableView.search(text: query) }
                    }
                    .searchable(text: $query, placement: .navigationBarDrawer(displayMode: .always),
                                prompt: "Functions, classes, examples")
                    .navigationDestination(for: FirmwareDocEntry.self) { entry in
                        ScriptDocDetail(entry: entry, onInsert: onInsert.map { insert -> () -> Void in
                            { insert(entry.snippet, entry.requiredImport); dismiss() }
                        })
                    }
                }
            }
            .navigationTitle("Scripting API")
            .navigationBarTitleDisplayMode(.inline)
            .toolbar {
                ToolbarItem(placement: .topBarTrailing) {
                    Button("Done") { dismiss() }
                        .accessibilityIdentifier("docs_done_button")
                }
            }
        }
        .onAppear { if entries.isEmpty, let catalogue { entries = FirmwareDocs.entries(catalogue) } }
    }
}

private struct ScriptDocRow: View {
    let entry: FirmwareDocEntry

    var body: some View {
        VStack(alignment: .leading, spacing: 3) {
            HStack(spacing: 6) {
                Image(systemName: ScriptDocDetail.symbol(for: entry.kind))
                    .foregroundStyle(.cyan)
                    .frame(width: 18)
                Text(entry.title)
                    .font(.system(.subheadline, design: .monospaced).weight(.semibold))
                    .lineLimit(1)
            }
            if let firstLine = entry.doc.split(separator: "\n").first {
                Text(firstLine)
                    .font(.caption)
                    .foregroundStyle(.secondary)
                    .lineLimit(2)
            }
        }
        .padding(.vertical, 2)
    }
}

struct ScriptDocDetail: View {
    let entry: FirmwareDocEntry
    let onInsert: (() -> Void)?

    static func symbol(for kind: FirmwareDocEntry.Kind) -> String {
        switch kind {
        case .module: return "shippingbox"
        case .type: return "cube"
        case .namespace: return "folder"
        case .function, .method: return "function"
        case .constant: return "number"
        case .example: return "doc.text"
        }
    }

    var body: some View {
        ScrollView {
            VStack(alignment: .leading, spacing: 14) {
                Text(entry.title)
                    .font(.system(.title3, design: .monospaced).weight(.bold))
                    .textSelection(.enabled)
                if !entry.signature.isEmpty && entry.kind != .example {
                    codeBlock(entry.signature)
                }
                if !entry.doc.isEmpty {
                    Text(entry.doc)
                        .font(.callout)
                        .textSelection(.enabled)
                }
                Text(entry.kind == .example ? "Example" : "Snippet")
                    .font(.caption.weight(.bold))
                    .foregroundStyle(.secondary)
                codeBlock(entry.snippet)
                HStack(spacing: 12) {
                    if let onInsert {
                        Button(action: onInsert) {
                            Label(entry.kind == .example ? "Insert example" : "Insert", systemImage: "text.insert")
                        }
                        .buttonStyle(.borderedProminent)
                        .accessibilityIdentifier("doc_insert_button")
                    }
                    Button {
                        UIPasteboard.general.string = entry.snippet
                    } label: {
                        Label("Copy", systemImage: "doc.on.doc")
                    }
                    .buttonStyle(.bordered)
                }
            }
            .padding()
            .frame(maxWidth: .infinity, alignment: .leading)
        }
        .navigationTitle(entry.kind.rawValue.capitalized)
        .navigationBarTitleDisplayMode(.inline)
    }

    private func codeBlock(_ text: String) -> some View {
        Text(text)
            .font(.system(.footnote, design: .monospaced))
            .textSelection(.enabled)
            .padding(10)
            .frame(maxWidth: .infinity, alignment: .leading)
            .background(RoundedRectangle(cornerRadius: 10, style: .continuous).fill(Color.white.opacity(0.06)))
    }
}

#Preview {
    ScriptDocsBrowser(catalogue: FirmwareAPICatalogue.bundled, onInsert: { _, _ in })
        .preferredColorScheme(.dark)
}
