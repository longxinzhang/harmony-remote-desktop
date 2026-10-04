import AppKit
import UniformTypeIdentifiers

final class AppKitClipboardAdapter: ClipboardAdapter {
    static let ownerType = NSPasteboard.PasteboardType("com.longxin.harmonyremote.clipboard-owner")
    private let board: NSPasteboard
    private var attemptedAutomaticRead = false
    // Injectable named pasteboards allow platform tests without touching the user's General board.
    init(board: NSPasteboard = .general) { self.board = board }
    var revision: Int { board.changeCount }
    func allowOneAutomaticReadAttempt() { attemptedAutomaticRead = false }
    func read(explicit: Bool) throws -> ClipboardSnapshot {
        if #available(macOS 15.4, *), !explicit {
            switch board.accessBehavior {
            case .alwaysAllow: break
            case .alwaysDeny: throw ClipboardFailure.denied
            default:
                // First programmatic access makes the app visible in the system
                // permission settings. Never repeat that prompt from a poll loop.
                guard !attemptedAutomaticRead else { throw ClipboardFailure.denied }
                attemptedAutomaticRead = true
            }
        }
        let before = board.changeCount
        guard let items = board.pasteboardItems, items.count == 1, let item = items.first else { throw ClipboardFailure.unsupported }
        let forbidden = items.flatMap(\.types).contains { type in
            let name = type.rawValue.lowercased()
            return type == .fileURL || UTType(type.rawValue)?.conforms(to: .image) == true ||
                name.contains("filepromise") || name.contains("file-url") ||
                name.contains("promised-file") || name == "nsfilenamespboardtype" || name == "nsfilespromisepboardtype"
        }
        guard !forbidden, item.types.contains(.string), let string = item.string(forType: .string) else { throw ClipboardFailure.unsupported }
        let data = Data(string.utf8)
        _ = try ClipboardWire.text(data)
        let owner = item.string(forType: Self.ownerType)
        guard before == board.changeCount else { throw ClipboardFailure.changed }
        return ClipboardSnapshot(revision: before, text: data, owner: owner)
    }
    func write(_ text: Data, owner: String, expectedRevision: Int) throws -> Int {
        let string = try ClipboardWire.text(text)
        let item = NSPasteboardItem()
        guard item.setString(string, forType: .string), item.setString(owner, forType: Self.ownerType) else { throw ClipboardFailure.write }
        guard board.changeCount == expectedRevision else { throw ClipboardFailure.changed }
        board.prepareForNewContents(with: [.currentHostOnly])
        guard board.writeObjects([item]) else { throw ClipboardFailure.write }
        let revision = board.changeCount
        guard let current = board.pasteboardItems, current.count == 1,
              current[0].string(forType: Self.ownerType) == owner,
              current[0].string(forType: .string).map({ Data($0.utf8) }) == text,
              board.changeCount == revision else { throw ClipboardFailure.changed }
        return revision
    }
}
