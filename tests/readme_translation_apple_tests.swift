@main
struct ReadmeTranslationOutputTests {
    static func main() {
        var output = ReadmeTranslationOutput(maximumBytes: 8)
        precondition(output.append("한🙂"))
        precondition(!output.append("\n\n"))
        precondition(output.text == "한🙂")
        precondition(output.append("\n"))
        precondition(!output.append("a"))
    }
}
