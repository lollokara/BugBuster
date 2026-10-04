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
                                }
                            }
                        }
                    }
                    .listStyle(.insetGrouped)
                    .overlay {
                        if sections.isEmpty { ContentUnavailableView.search(text: query) }
                    }
                    .searchable(text: $query, placement: .navigationBarDrawer(displayMode: .always),
                                prompt: "Functions, classes, examples")
                    .navigationDestination(for: FirmwareDocEntry.self) { entry in
                        ScriptDocDetail(entry: entry, onInsert: onInsert.map { insert -> (String) -> Void in
                            { code in insert(code, entry.requiredImport); dismiss() }
                        })
                    }
                }
            }
            .navigationTitle("Scripting API")
            .navigationBarTitleDisplayMode(.inline)
            .toolbar {
                ToolbarItem(placement: .topBarTrailing) { Button("Done") { dismiss() } }
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
            if !entry.summary.isEmpty {
                Text(entry.summary)
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
    /// Insert `code` at the caret (nil when the browser is read-only).
    let onInsert: ((String) -> Void)?

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
            VStack(alignment: .leading, spacing: 16) {
                Text(entry.title)
                    .font(.system(.title3, design: .monospaced).weight(.bold))
                    .textSelection(.enabled)
                if !entry.signature.isEmpty && entry.kind != .example {
                    codeBlock(entry.signature)
                }
                if !entry.detail.summary.isEmpty {
                    Text(entry.detail.summary)
                        .font(.headline)
                        .textSelection(.enabled)
                }
                if !entry.detail.description.isEmpty {
                    Text(entry.detail.description)
                        .font(.callout)
                        .textSelection(.enabled)
                } else if entry.detail.summary.isEmpty && !entry.doc.isEmpty {
                    Text(entry.doc).font(.callout).textSelection(.enabled)
                }
                if let value = entry.value, entry.kind == .constant {
                    labeled("Value") { codeBlock(value) }
                }
                if !entry.params.isEmpty {
                    labeled("Parameters") { parameterTable }
                }
                if entry.returnType.map({ $0 != "None" }) ?? false || !entry.detail.returnsDoc.isEmpty {
                    labeled("Returns") { returnsBlock }
                }
                if !entry.detail.raises.isEmpty {
                    labeled("Raises") { raisesList }
                }
                if !entry.detail.notes.isEmpty {
                    safetyNote(entry.detail.notes)
                }
                if entry.kind == .example {
                    labeled("Example") { codeCard(entry.snippet, insertLabel: "Insert example") }
                } else {
                    ForEach(Array(entry.detail.examples.enumerated()), id: \.offset) { index, code in
                        labeled(entry.detail.examples.count > 1 ? "Example \(index + 1)" : "Example") {
                            codeCard(code, insertLabel: "Insert")
                        }
                    }
                    labeled("Call") { codeCard(entry.snippet, insertLabel: "Insert") }
                }
            }
            .padding()
            .frame(maxWidth: .infinity, alignment: .leading)
        }
        .navigationTitle(entry.kind.rawValue.capitalized)
        .navigationBarTitleDisplayMode(.inline)
    }

    // MARK: sections

    private func labeled<Content: View>(_ title: String, @ViewBuilder content: () -> Content) -> some View {
        VStack(alignment: .leading, spacing: 6) {
            Text(title.uppercased())
                .font(.caption.weight(.bold))
                .foregroundStyle(.secondary)
                .accessibilityAddTraits(.isHeader)
            content()
        }
    }

    private var parameterTable: some View {
        VStack(alignment: .leading, spacing: 0) {
            ForEach(Array(entry.params.enumerated()), id: \.offset) { index, param in
                if index > 0 { Divider().opacity(0.4) }
                VStack(alignment: .leading, spacing: 3) {
                    HStack(alignment: .firstTextBaseline, spacing: 6) {
                        Text(Self.parameterName(param))
                            .font(.system(.subheadline, design: .monospaced).weight(.semibold))
                        if let type = param.annotation {
                            Text(type)
                                .font(.system(.caption, design: .monospaced))
                                .foregroundStyle(.cyan)
                        }
                        Spacer(minLength: 4)
                        Text(Self.requirement(param))
                            .font(.caption2.weight(.semibold))
                            .foregroundStyle(param.isRequired ? .orange : .secondary)
                    }
                    if !param.doc.isEmpty {
                        Text(param.doc).font(.footnote).foregroundStyle(.secondary).textSelection(.enabled)
                    }
                }
                .padding(.vertical, 8)
                .accessibilityElement(children: .combine)
            }
        }
        .padding(.horizontal, 10)
        .background(RoundedRectangle(cornerRadius: 10, style: .continuous).fill(Color.white.opacity(0.06)))
    }

    private var returnsBlock: some View {
        VStack(alignment: .leading, spacing: 8) {
            if let type = entry.returnType {
                Text("-> \(type)")
                    .font(.system(.footnote, design: .monospaced).weight(.semibold))
                    .foregroundStyle(.cyan)
            }
            if !entry.detail.returnsDoc.isEmpty {
                Text(entry.detail.returnsDoc).font(.callout).textSelection(.enabled)
            }
            if !entry.detail.returnKeys.isEmpty {
                VStack(alignment: .leading, spacing: 0) {
                    ForEach(Array(entry.detail.returnKeys.enumerated()), id: \.offset) { index, key in
                        if index > 0 { Divider().opacity(0.4) }
                        VStack(alignment: .leading, spacing: 2) {
                            Text("\"\(key.name)\"")
                                .font(.system(.footnote, design: .monospaced).weight(.semibold))
                            Text(key.doc).font(.footnote).foregroundStyle(.secondary).textSelection(.enabled)
                        }
                        .padding(.vertical, 6)
                        .accessibilityElement(children: .combine)
                    }
                }
                .padding(.horizontal, 10)
                .background(RoundedRectangle(cornerRadius: 10, style: .continuous).fill(Color.white.opacity(0.06)))
            }
        }
    }

    private var raisesList: some View {
        VStack(alignment: .leading, spacing: 6) {
            ForEach(Array(entry.detail.raises.enumerated()), id: \.offset) { _, item in
                VStack(alignment: .leading, spacing: 2) {
                    Text(item.type).font(.system(.footnote, design: .monospaced).weight(.semibold))
                    Text(item.doc).font(.footnote).foregroundStyle(.secondary).textSelection(.enabled)
                }
                .accessibilityElement(children: .combine)
            }
        }
    }

    private func safetyNote(_ text: String) -> some View {
        HStack(alignment: .top, spacing: 8) {
            Image(systemName: "exclamationmark.triangle.fill")
                .foregroundStyle(.orange)
                .accessibilityHidden(true)
            VStack(alignment: .leading, spacing: 2) {
                Text("Safety").font(.caption.weight(.bold)).foregroundStyle(.orange)
                Text(text).font(.footnote).textSelection(.enabled)
            }
        }
        .padding(10)
        .frame(maxWidth: .infinity, alignment: .leading)
        .background(RoundedRectangle(cornerRadius: 10, style: .continuous).fill(Color.orange.opacity(0.12)))
        .accessibilityElement(children: .combine)
    }

    /// Code with its own Insert and Copy buttons.
    private func codeCard(_ code: String, insertLabel: String) -> some View {
        VStack(alignment: .leading, spacing: 8) {
            codeBlock(code)
            HStack(spacing: 12) {
                if let onInsert {
                    Button { onInsert(code) } label: { Label(insertLabel, systemImage: "text.insert") }
                        .buttonStyle(.borderedProminent)
                }
                Button { UIPasteboard.general.string = code } label: { Label("Copy", systemImage: "doc.on.doc") }
                    .buttonStyle(.bordered)
            }
            .controlSize(.regular)
        }
    }

    private func codeBlock(_ text: String) -> some View {
        ScrollView(.horizontal, showsIndicators: false) {
            Text(text)
                .font(.system(.footnote, design: .monospaced))
                .textSelection(.enabled)
                .fixedSize(horizontal: true, vertical: true)
                .padding(10)
        }
        .frame(maxWidth: .infinity, alignment: .leading)
        .background(RoundedRectangle(cornerRadius: 10, style: .continuous).fill(Color.white.opacity(0.06)))
    }

    // MARK: formatting (internal for tests)

    /// `name`, `*name`, `**name`, keyword-only marked with a trailing `=`.
    static func parameterName(_ p: FirmwareParam) -> String {
        switch p.kind {
        case .varPositional: return "*\(p.name)"
        case .varKeyword: return "**\(p.name)"
        case .keywordOnly: return "\(p.name)="
        case .positional: return p.name
        }
    }

    /// "required", or "default 400000", or "optional" for `*args`/`**kwargs`.
    static func requirement(_ p: FirmwareParam) -> String {
        if let value = p.defaultValue { return "default \(value)" }
        return p.acceptsKeyword ? "required" : "optional"
    }
}

#Preview {
    ScriptDocsBrowser(catalogue: FirmwareAPICatalogue.bundled, onInsert: { _, _ in })
        .preferredColorScheme(.dark)
}
