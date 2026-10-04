import UIKit

/// The editor's keyboard accessory: a keyboard-styled bar that sits flush on top of the
/// keyboard at full width. 44 pt buttons in groups — indent / outdent / Tab, code symbols,
/// undo / redo — scrolling sideways when they do not fit, with next-placeholder and
/// dismiss-keyboard pinned at the trailing edge.
final class ScriptKeyBar: UIInputView {
    static let height: CGFloat = 48
    static let buttonSize: CGFloat = 44
    static let symbols = ["(", ")", "[", "]", ":", "=", "\"", "'", "#"]

    struct Actions {
        var outdent: () -> Void = {}
        var indent: () -> Void = {}
        var tab: () -> Void = {}
        var insert: (String) -> Void = { _ in }
        var undo: () -> Void = {}
        var redo: () -> Void = {}
        var nextPlaceholder: () -> Void = {}
        var dismiss: () -> Void = {}
    }

    var actions = Actions()

    private let undoButton = ScriptKeyBar.iconButton("arrow.uturn.backward", label: "Undo")
    private let redoButton = ScriptKeyBar.iconButton("arrow.uturn.forward", label: "Redo")
    private let nextButton = ScriptKeyBar.iconButton("chevron.forward.2", label: "Next placeholder")

    init() {
        super.init(frame: CGRect(x: 0, y: 0, width: 0, height: Self.height), inputViewStyle: .keyboard)
        allowsSelfSizing = true
        autoresizingMask = [.flexibleWidth]
        build()
    }

    @available(*, unavailable) required init?(coder: NSCoder) { fatalError() }

    override var intrinsicContentSize: CGSize { CGSize(width: UIView.noIntrinsicMetric, height: Self.height) }

    /// Enable or dim the buttons whose action is currently possible.
    func refresh(canUndo: Bool, canRedo: Bool, hasPlaceholder: Bool) {
        undoButton.isEnabled = canUndo
        redoButton.isEnabled = canRedo
        nextButton.isEnabled = hasPlaceholder
    }

    // MARK: layout

    private func build() {
        let outdent = Self.iconButton("decrease.indent", label: "Outdent")
        let indent = Self.iconButton("increase.indent", label: "Indent")
        let tab = Self.textButton("Tab", label: "Tab", bold: true)
        let dismiss = Self.iconButton("keyboard.chevron.compact.down", label: "Dismiss keyboard")
        bind(outdent) { $0.outdent() }
        bind(indent) { $0.indent() }
        bind(tab) { $0.tab() }
        bind(undoButton) { $0.undo() }
        bind(redoButton) { $0.redo() }
        bind(nextButton) { $0.nextPlaceholder() }
        bind(dismiss) { $0.dismiss() }
        nextButton.tintColor = .cyan

        let symbolButtons = Self.symbols.map { symbol -> UIButton in
            let button = Self.textButton(symbol, label: Self.spoken(symbol), bold: false)
            bind(button) { $0.insert(symbol) }
            return button
        }

        let groups = [[outdent, indent, tab], symbolButtons, [undoButton, redoButton]]
        let strip = UIStackView()
        strip.axis = .horizontal
        strip.alignment = .center
        strip.distribution = .fill
        strip.spacing = 0
        strip.translatesAutoresizingMaskIntoConstraints = false
        for (index, group) in groups.enumerated() {
            if index > 0 { strip.addArrangedSubview(Self.divider()) }
            let row = UIStackView(arrangedSubviews: group)
            row.axis = .horizontal
            row.spacing = 0
            strip.addArrangedSubview(row)
        }
        let spacer = UIView()                      // soaks up the extra width on a wide bar
        spacer.setContentHuggingPriority(.defaultLow - 1, for: .horizontal)
        strip.addArrangedSubview(spacer)

        let scroll = UIScrollView()
        scroll.showsHorizontalScrollIndicator = false
        scroll.alwaysBounceHorizontal = false
        scroll.translatesAutoresizingMaskIntoConstraints = false
        scroll.addSubview(strip)

        let pinned = UIStackView(arrangedSubviews: [Self.divider(), nextButton, dismiss])
        pinned.axis = .horizontal
        pinned.alignment = .center
        pinned.spacing = 0
        pinned.translatesAutoresizingMaskIntoConstraints = false

        let hairline = UIView()
        hairline.backgroundColor = UIColor(white: 1, alpha: 0.10)
        hairline.translatesAutoresizingMaskIntoConstraints = false

        addSubview(scroll)
        addSubview(pinned)
        addSubview(hairline)
        NSLayoutConstraint.activate([
            hairline.topAnchor.constraint(equalTo: topAnchor),
            hairline.leadingAnchor.constraint(equalTo: leadingAnchor),
            hairline.trailingAnchor.constraint(equalTo: trailingAnchor),
            hairline.heightAnchor.constraint(equalToConstant: 1 / UIScreen.main.scale),

            pinned.trailingAnchor.constraint(equalTo: safeAreaLayoutGuide.trailingAnchor, constant: -6),
            pinned.centerYAnchor.constraint(equalTo: centerYAnchor),

            scroll.leadingAnchor.constraint(equalTo: safeAreaLayoutGuide.leadingAnchor),
            scroll.trailingAnchor.constraint(equalTo: pinned.leadingAnchor),
            scroll.topAnchor.constraint(equalTo: topAnchor),
            scroll.bottomAnchor.constraint(equalTo: bottomAnchor),

            strip.leadingAnchor.constraint(equalTo: scroll.contentLayoutGuide.leadingAnchor, constant: 6),
            strip.trailingAnchor.constraint(equalTo: scroll.contentLayoutGuide.trailingAnchor, constant: -6),
            strip.centerYAnchor.constraint(equalTo: scroll.frameLayoutGuide.centerYAnchor),
            // At least the visible width, so a wide bar (iPad, landscape) spreads the groups across it.
            strip.widthAnchor.constraint(greaterThanOrEqualTo: scroll.frameLayoutGuide.widthAnchor, constant: -12),
            scroll.contentLayoutGuide.heightAnchor.constraint(equalTo: scroll.frameLayoutGuide.heightAnchor)
        ])
        refresh(canUndo: false, canRedo: false, hasPlaceholder: false)
    }

    private func bind(_ button: UIButton, _ call: @escaping (Actions) -> Void) {
        button.addAction(UIAction { [weak self] _ in
            guard let self else { return }
            UIDevice.current.playInputClick()
            call(self.actions)
        }, for: .touchUpInside)
    }

    // MARK: pieces

    static func iconButton(_ symbol: String, label: String) -> UIButton {
        let button = baseButton(label: label)
        button.configuration?.image = UIImage(systemName: symbol,
                                              withConfiguration: UIImage.SymbolConfiguration(pointSize: 17, weight: .medium))
        return button
    }

    static func textButton(_ title: String, label: String, bold: Bool) -> UIButton {
        let button = baseButton(label: label)
        var attributes = AttributeContainer()
        attributes.font = .monospacedSystemFont(ofSize: bold ? 14 : 20, weight: bold ? .bold : .medium)
        button.configuration?.attributedTitle = AttributedString(title, attributes: attributes)
        return button
    }

    private static func baseButton(label: String) -> UIButton {
        let button = UIButton(configuration: .plain())
        button.configuration?.baseForegroundColor = UIColor(white: 0.95, alpha: 1)
        button.configuration?.contentInsets = .zero
        button.configuration?.background.cornerRadius = 10
        button.configurationUpdateHandler = { b in
            b.configuration?.background.backgroundColor = b.isHighlighted ? UIColor(white: 1, alpha: 0.22) : .clear
            b.alpha = b.isEnabled ? 1 : 0.35
        }
        button.accessibilityLabel = label
        button.translatesAutoresizingMaskIntoConstraints = false
        NSLayoutConstraint.activate([
            button.widthAnchor.constraint(equalToConstant: buttonSize),
            button.heightAnchor.constraint(equalToConstant: buttonSize)
        ])
        return button
    }

    private static func divider() -> UIView {
        let line = UIView()
        line.backgroundColor = UIColor(white: 1, alpha: 0.18)
        line.translatesAutoresizingMaskIntoConstraints = false
        NSLayoutConstraint.activate([
            line.widthAnchor.constraint(equalToConstant: 1),
            line.heightAnchor.constraint(equalToConstant: 24)
        ])
        let holder = UIView()
        holder.translatesAutoresizingMaskIntoConstraints = false
        holder.addSubview(line)
        NSLayoutConstraint.activate([
            holder.widthAnchor.constraint(equalToConstant: 9),
            line.centerXAnchor.constraint(equalTo: holder.centerXAnchor),
            line.centerYAnchor.constraint(equalTo: holder.centerYAnchor),
            holder.heightAnchor.constraint(equalToConstant: buttonSize)
        ])
        return holder
    }

    static func spoken(_ symbol: String) -> String {
        switch symbol {
        case "(": return "Open parenthesis"
        case ")": return "Close parenthesis"
        case "[": return "Open bracket"
        case "]": return "Close bracket"
        case ":": return "Colon"
        case "=": return "Equals"
        case "\"": return "Double quote"
        case "'": return "Single quote"
        case "#": return "Hash"
        default: return symbol
        }
    }
}
