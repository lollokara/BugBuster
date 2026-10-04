import SwiftUI
import UIKit
import CoreTransferable
import UniformTypeIdentifiers

enum ScriptLogStyle {
    static func color(for level: ScriptLogLevel) -> Color {
        switch level {
        case .error: return .red
        case .warning: return .orange
        case .info: return .primary
        case .debug: return .secondary
        }
    }

    /// Device uptime as `s.mmm`.
    static func timestamp(_ ms: UInt32?) -> String {
        guard let ms else { return "" }
        return String(format: "%u.%03u", ms / 1000, ms % 1000)
    }

    static func shareFileName(device: String, date: Date, timeZone: TimeZone = .current) -> String {
        let f = DateFormatter()
        f.locale = Locale(identifier: "en_US_POSIX")
        f.timeZone = timeZone
        f.dateFormat = "yyyyMMdd-HHmmss"
        return "\(device)-script-log-\(f.string(from: date)).txt"
    }
}

/// Transferable model for lazy log export, avoiding string materialization on render (Finding #17).
struct ScriptLogExport: Transferable, Equatable {
    let fileName: String
    let textProvider: @Sendable () -> String

    static func == (lhs: ScriptLogExport, rhs: ScriptLogExport) -> Bool {
        lhs.fileName == rhs.fileName
    }

    static var transferRepresentation: some TransferRepresentation {
        DataRepresentation(exportedContentType: .plainText) { export in
            Data(export.textProvider().utf8)
        }
        .suggestedFileName { export in
            export.fileName
        }
    }
}

/// Live device log (spec §4): level colouring, filter, pause-scroll, copy/share.
/// Hosted by the iPad trailing column and the iPhone sheet (Task 14). Polling is
/// the manager's job; this view only renders `store`.
struct ScriptLogConsoleView: View {
    @ObservedObject var store: ScriptLogStore
    var title = "Script log"
    var subtitle: String?
    var onClose: (() -> Void)?

    @State private var filter = ScriptLogFilter()
    @State private var showSearch = false
    @State private var copied = false

    private static let bottomID = "log-bottom"

    var body: some View {
        VStack(spacing: 0) {
            header
            if showSearch {
                TextField("Filter text", text: $filter.query)
                    .textFieldStyle(.roundedBorder)
                    .font(.subheadline.monospaced())
                    .autocorrectionDisabled()
                    .textInputAutocapitalization(.never)
                    .padding(.horizontal, 10)
                    .padding(.bottom, 6)
            }
            Divider().opacity(0.3)
            lines
        }
        .background(Color.black.opacity(0.25))
    }

    private var header: some View {
        HStack(spacing: 2) {
            VStack(alignment: .leading, spacing: 1) {
                Text(title).font(.headline)
                if let subtitle {
                    Text(subtitle).font(.caption2.monospaced()).foregroundStyle(.secondary).lineLimit(1)
                }
            }
            Spacer(minLength: 4)
            Menu {
                ForEach(ScriptLogLevel.allCases) { level in
                    Toggle(level.label, isOn: Binding(
                        get: { filter.levels.contains(level) },
                        set: { on in if on { filter.levels.insert(level) } else { filter.levels.remove(level) } }))
                }
            } label: {
                Image(systemName: filter.levels.count == ScriptLogLevel.allCases.count
                      ? "line.3.horizontal.decrease.circle" : "line.3.horizontal.decrease.circle.fill")
                    .frame(minWidth: 44, minHeight: 44)
                    .contentShape(Rectangle())
            }
            .accessibilityLabel("Filter levels")
            Button { showSearch.toggle(); if !showSearch { filter.query = "" } } label: {
                Image(systemName: "magnifyingglass")
                    .frame(minWidth: 44, minHeight: 44)
                    .contentShape(Rectangle())
            }
            .accessibilityLabel("Filter text")
            Button { store.isPaused.toggle() } label: {
                Image(systemName: store.isPaused ? "play.circle.fill" : "pause.circle")
                    .frame(minWidth: 44, minHeight: 44)
                    .contentShape(Rectangle())
            }
            .accessibilityLabel(store.isPaused ? "Resume auto-scroll" : "Pause auto-scroll")
            Button {
                UIPasteboard.general.string = store.plainText
                copied = true
                Task { try? await Task.sleep(nanoseconds: 1_200_000_000); copied = false }
            } label: {
                Image(systemName: copied ? "checkmark" : "doc.on.doc")
                    .frame(minWidth: 44, minHeight: 44)
                    .contentShape(Rectangle())
            }
            .accessibilityLabel("Copy log")
            ShareLink(item: ScriptLogExport(
                fileName: ScriptLogStyle.shareFileName(device: "bugbuster", date: Date()),
                textProvider: { [weak store] in store?.plainText ?? "" }
            ),
            preview: SharePreview(ScriptLogStyle.shareFileName(device: "bugbuster", date: Date()))) {
                Image(systemName: "square.and.arrow.up")
                    .frame(minWidth: 44, minHeight: 44)
                    .contentShape(Rectangle())
            }
            .accessibilityLabel("Share log")
            Button { store.clear() } label: {
                Image(systemName: "trash")
                    .frame(minWidth: 44, minHeight: 44)
                    .contentShape(Rectangle())
            }
            .accessibilityLabel("Clear log")
            if let onClose {
                Button(action: onClose) {
                    Image(systemName: "xmark.circle.fill").foregroundStyle(.secondary)
                        .frame(minWidth: 44, minHeight: 44)
                        .contentShape(Rectangle())
                }
                .accessibilityLabel("Close log")
            }
        }
        .font(.body)
        .foregroundStyle(.cyan)
        .padding(.horizontal, 12)
        .padding(.vertical, 6)
    }

    private var lines: some View {
        let visible = filter.apply(store.lines)
        return ScrollViewReader { proxy in
            ScrollView {
                LazyVStack(alignment: .leading, spacing: 2) {
                    ForEach(visible) { line in
                        ScriptLogLineRow(line: line)
                    }
                    Color.clear.frame(height: 1).id(Self.bottomID)
                }
                .padding(.horizontal, 10)
                .padding(.vertical, 6)
                .textSelection(.enabled)
            }
            .overlay(alignment: .bottomTrailing) {
                if store.isPaused {
                    Button {
                        store.isPaused = false
                        proxy.scrollTo(Self.bottomID, anchor: .bottom)
                    } label: {
                        Label("Latest", systemImage: "arrow.down.to.line")
                            .font(.caption.weight(.semibold))
                            .padding(.horizontal, 10).padding(.vertical, 6)
                            .frame(minHeight: 44)
                    }
                    .glassEffect(.regular.tint(.cyan), in: Capsule())
                    .padding(10)
                }
            }
            .overlay {
                if visible.isEmpty {
                    Text(store.lines.isEmpty ? "No output yet" : "No lines match the filter")
                        .font(.subheadline)
                        .foregroundStyle(.secondary)
                }
            }
            .onChange(of: store.lines.last?.id) { _, _ in
                guard !store.isPaused else { return }
                proxy.scrollTo(Self.bottomID, anchor: .bottom)
            }
            .onAppear { proxy.scrollTo(Self.bottomID, anchor: .bottom) }
        }
    }
}

private struct ScriptLogLineRow: View {
    let line: ScriptLogLine

    var body: some View {
        if line.isMarker {
            Text("— \(line.text) —")
                .font(.caption2.monospaced().weight(.semibold))
                .foregroundStyle(.cyan.opacity(0.8))
                .frame(maxWidth: .infinity, alignment: .center)
                .padding(.vertical, 3)
        } else {
            HStack(alignment: .firstTextBaseline, spacing: 6) {
                if line.tsMs != nil {
                    Text(ScriptLogStyle.timestamp(line.tsMs))
                        .foregroundStyle(.secondary)
                    Text(line.level.rawValue)
                        .fontWeight(.bold)
                        .foregroundStyle(ScriptLogStyle.color(for: line.level))
                }
                Text(line.text)
                    .foregroundStyle(ScriptLogStyle.color(for: line.level))
                    .frame(maxWidth: .infinity, alignment: .leading)
            }
            .font(.caption.monospaced())
        }
    }
}

