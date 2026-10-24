// Every append, including untranslated text and whitespace, uses the same UTF-8 budget.
struct ReadmeTranslationOutput {
    let maximumBytes: Int
    private(set) var text = ""
    private var bytes = 0
    mutating func append(_ value: String) -> Bool {
        let size = value.utf8.count
        guard size <= maximumBytes - bytes else { return false }
        text += value
        bytes += size
        return true
    }
}
