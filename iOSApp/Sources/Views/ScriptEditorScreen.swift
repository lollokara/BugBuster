import SwiftUI

struct ScriptEditorScreen: View {
    let name: String
    @ObservedObject var model: ScriptsTabModel
    @ObservedObject var editor: ScriptEditorModel
    let onBack: () -> Void
    let onRun: (_ background: Bool) -> Void

    @State private var showingDocs = false

    init(name: String, model: ScriptsTabModel, onBack: @escaping () -> Void, onRun: @escaping (_ background: Bool) -> Void) {
        self.name = name
        self.model = model
        self.editor = model.editor
        self.onBack = onBack
        self.onRun = onRun
    }

    var body: some View {
        VStack(spacing: 8) {
            HStack(spacing: 14) {
                Button(action: onBack) {
                    Label("Back", systemImage: "chevron.left")
                        .lineLimit(1).fixedSize()
                        .frame(minHeight: ScriptViewMetrics.minTouchTarget)
                }
                .accessibilityIdentifier("editor_back_button")
                Spacer()
                Text(name)
                    .font(.subheadline.monospaced().weight(.bold))
                    .lineLimit(1)
                    .minimumScaleFactor(0.85)
                if editor.isDirty {
                    Circle().fill(.orange).frame(width: 6, height: 6).accessibilityLabel("Unsaved changes")
                }
                Spacer()
                Button { showingDocs = true } label: {
                    Image(systemName: "book")
                        .frame(minWidth: ScriptViewMetrics.minTouchTarget, minHeight: ScriptViewMetrics.minTouchTarget)
                        .contentShape(Rectangle())
                }
                .accessibilityLabel("Scripting API docs")
                .accessibilityIdentifier("editor_docs_button")

                Button("Save") { Task { await model.save() } }
                    .lineLimit(1).fixedSize()
                    .fontWeight(.bold)
                    .disabled(model.uploadProgress != nil)
                    .frame(minHeight: ScriptViewMetrics.minTouchTarget)
                    .accessibilityIdentifier("editor_save_button")

                ScriptRunMenu(onRun: onRun) {
                    Image(systemName: "play.fill")
                        .foregroundStyle(.green)
                        .frame(width: ScriptViewMetrics.minTouchTarget, height: ScriptViewMetrics.minTouchTarget)
                        .contentShape(Rectangle())
                }
                .accessibilityLabel("Run \(name)")
                .accessibilityIdentifier("editor_run_menu")
            }
            .padding(.horizontal)
            .padding(.vertical, 4)
            .glassEffect(.regular, in: RoundedRectangle(cornerRadius: 16, style: .continuous))

            if let progress = model.uploadProgress {
                ProgressView(value: progress).tint(.cyan)
            }
            if let lint = model.lint {
                ScriptLintBannerView(lint: lint)
                    .accessibilityElement(children: .contain)
                    .accessibilityIdentifier("script_lint_banner")
            }
            ScriptEditorView(model: editor)
                .accessibilityIdentifier("script_editor_view")
                .padding(4)
                .glassEffect(.regular, in: RoundedRectangle(cornerRadius: 16, style: .continuous))
        }
        .padding(.horizontal)
        .padding(.bottom, 8)
        .sheet(isPresented: $showingDocs) {
            ScriptDocsBrowser(catalogue: FirmwareAPICatalogue.bundled) { snippet, module in
                editor.insertAtCaret(snippet, ensuringImport: module)
            }
        }
    }
}

private struct ScriptLintBannerView: View {
    let lint: ScriptsTabModel.LintBanner

    private var icon: String {
        switch lint {
        case .ok: return "checkmark.circle.fill"
        case .failed: return "exclamationmark.triangle.fill"
        case .unavailable: return "info.circle.fill"
        }
    }

    private var text: String {
        switch lint {
        case .ok: return "Saved · syntax OK"
        case .failed(let message), .unavailable(let message): return message
        }
    }

    private var tint: Color {
        switch lint {
        case .ok: return .green
        case .failed: return .red
        case .unavailable: return .orange
        }
    }

    var body: some View {
        HStack(alignment: .top, spacing: 8) {
            Image(systemName: icon)
                .accessibilityHidden(true)
            Text(text).font(.caption.weight(.semibold)).lineLimit(3)
            Spacer(minLength: 0)
        }
        .foregroundStyle(tint)
        .padding(.horizontal, 12)
        .padding(.vertical, 8)
        .glassEffect(.regular.tint(tint), in: RoundedRectangle(cornerRadius: 12, style: .continuous))
    }
}

