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
                Button(action: onBack) { Label("Back", systemImage: "chevron.left") }
                Spacer()
                Text(name)
                    .font(.system(size: 14, weight: .bold, design: .monospaced))
                    .lineLimit(1)
                if editor.isDirty {
                    Circle().fill(.orange).frame(width: 6, height: 6).accessibilityLabel("Unsaved changes")
                }
                Spacer()
                Button { showingDocs = true } label: { Image(systemName: "book") }
                    .accessibilityLabel("Scripting API docs")
                Button("Save") { Task { await model.save() } }
                    .fontWeight(.bold)
                    .disabled(model.uploadProgress != nil)
                ScriptRunMenu(onRun: onRun) {
                    Image(systemName: "play.fill").foregroundStyle(.green)
                }
                .accessibilityLabel("Run \(name)")
            }
            .padding()
            .glassEffect(.regular, in: RoundedRectangle(cornerRadius: 16, style: .continuous))

            if let progress = model.uploadProgress {
                ProgressView(value: progress).tint(.cyan)
            }
            if let lint = model.lint {
                ScriptLintBannerView(lint: lint)
            }
            ScriptEditorView(model: editor)
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
            Text(text).font(.system(size: 12, weight: .semibold)).lineLimit(3)
            Spacer(minLength: 0)
        }
        .foregroundStyle(tint)
        .padding(.horizontal, 12)
        .padding(.vertical, 8)
        .glassEffect(.regular.tint(tint), in: RoundedRectangle(cornerRadius: 12, style: .continuous))
    }
}
