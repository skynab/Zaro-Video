#include "Xml.h"

#include <algorithm>
#include <cctype>
#include <charconv>
#include <string>

namespace zaro::io::xml {
namespace {

bool isSpace(char c) noexcept {
    return c == ' ' || c == '\t' || c == '\n' || c == '\r';
}

/// The characters a name may contain. Deliberately permissive about ':': this
/// does not implement namespaces, so a prefixed name is simply a name with a
/// colon in it, which is the reading that lets a namespaced document through
/// instead of refusing it.
bool isNameChar(char c) noexcept {
    const auto u = static_cast<unsigned char>(c);
    return std::isalnum(u) != 0 || c == '_' || c == '-' || c == '.' || c == ':' ||
           u >= 0x80;  // any UTF-8 continuation or lead byte
}

void appendUtf8(std::string& out, std::uint32_t codepoint) {
    if (codepoint < 0x80) {
        out += static_cast<char>(codepoint);
    } else if (codepoint < 0x800) {
        out += static_cast<char>(0xC0U | (codepoint >> 6));
        out += static_cast<char>(0x80U | (codepoint & 0x3FU));
    } else if (codepoint < 0x10000) {
        out += static_cast<char>(0xE0U | (codepoint >> 12));
        out += static_cast<char>(0x80U | ((codepoint >> 6) & 0x3FU));
        out += static_cast<char>(0x80U | (codepoint & 0x3FU));
    } else {
        out += static_cast<char>(0xF0U | (codepoint >> 18));
        out += static_cast<char>(0x80U | ((codepoint >> 12) & 0x3FU));
        out += static_cast<char>(0x80U | ((codepoint >> 6) & 0x3FU));
        out += static_cast<char>(0x80U | (codepoint & 0x3FU));
    }
}

/// Expand entity references in a run of character data.
///
/// An `&` that does not begin a reference this understands is kept verbatim
/// rather than rejected. That is not laxity for its own sake: files exported by
/// other programs contain bare ampersands in clip names, and refusing a
/// two-hour cut over one of them helps nobody.
std::string unescape(std::string_view raw) {
    std::string out;
    out.reserve(raw.size());
    for (std::size_t i = 0; i < raw.size(); ++i) {
        if (raw[i] != '&') {
            out += raw[i];
            continue;
        }
        const std::size_t end = raw.find(';', i + 1);
        if (end == std::string_view::npos || end - i > 12) {
            out += raw[i];
            continue;
        }
        const std::string_view name = raw.substr(i + 1, end - i - 1);
        if (name == "amp") {
            out += '&';
        } else if (name == "lt") {
            out += '<';
        } else if (name == "gt") {
            out += '>';
        } else if (name == "quot") {
            out += '"';
        } else if (name == "apos") {
            out += '\'';
        } else if (name.size() > 1 && name.front() == '#') {
            const bool hex = name[1] == 'x' || name[1] == 'X';
            const std::string_view digits = name.substr(hex ? 2 : 1);
            std::uint32_t codepoint = 0;
            const char* first = digits.data();
            const char* last = first + digits.size();
            const auto parsed = std::from_chars(first, last, codepoint, hex ? 16 : 10);
            if (parsed.ec != std::errc{} || parsed.ptr != last || codepoint > 0x10FFFF) {
                out += raw[i];
                continue;
            }
            appendUtf8(out, codepoint);
        } else {
            out += raw[i];
            continue;
        }
        i = end;
    }
    return out;
}

std::string_view trimmed(std::string_view text) {
    std::size_t begin = 0;
    while (begin < text.size() && isSpace(text[begin])) {
        ++begin;
    }
    std::size_t end = text.size();
    while (end > begin && isSpace(text[end - 1])) {
        --end;
    }
    return text.substr(begin, end - begin);
}

void escapeInto(std::string& out, std::string_view text, bool inAttribute) {
    for (const char c : text) {
        switch (c) {
            case '&':
                out += "&amp;";
                break;
            case '<':
                out += "&lt;";
                break;
            case '>':
                out += "&gt;";
                break;
            case '"':
                // Only inside an attribute, where it would end the value. In
                // text it is an ordinary character, and escaping it there makes
                // every clip name with a quote in it harder to read for nothing.
                out += inAttribute ? "&quot;" : "\"";
                break;
            default:
                out += c;
                break;
        }
    }
}

void writeNode(std::string& out, const Node& node, int depth) {
    out.append(static_cast<std::size_t>(depth), '\t');
    out += '<';
    out += node.name;
    for (const auto& [key, value] : node.attributes) {
        out += ' ';
        out += key;
        out += "=\"";
        escapeInto(out, value, true);
        out += '"';
    }
    if (node.children.empty() && node.text.empty()) {
        out += "/>\n";
        return;
    }
    out += '>';
    if (node.children.empty()) {
        escapeInto(out, node.text, false);
    } else {
        out += '\n';
        for (const Node& child : node.children) {
            writeNode(out, child, depth + 1);
        }
        out.append(static_cast<std::size_t>(depth), '\t');
    }
    out += "</";
    out += node.name;
    out += ">\n";
}

class Parser {
public:
    explicit Parser(std::string_view text) : in_{text} {}

    Result<Node> document() {
        skipMisc();
        if (!looking("<")) {
            return fail("no root element");
        }
        auto root = element();
        if (!root) {
            return root;
        }
        skipMisc();
        return root;
    }

private:
    [[nodiscard]] bool atEnd() const noexcept { return i_ >= in_.size(); }
    [[nodiscard]] bool looking(std::string_view what) const noexcept {
        return in_.compare(i_, what.size(), what) == 0;
    }

    [[nodiscard]] Error fail(const std::string& what) const {
        return Error{ErrorCode::InvalidData, what + ", at byte " + std::to_string(i_)};
    }

    void skipSpace() {
        while (!atEnd() && isSpace(in_[i_])) {
            ++i_;
        }
    }

    /// Comments, processing instructions, the declaration and the doctype:
    /// everything that may sit between elements and carries nothing we keep.
    void skipMisc() {
        for (;;) {
            skipSpace();
            if (looking("<!--")) {
                const std::size_t end = in_.find("-->", i_ + 4);
                i_ = end == std::string_view::npos ? in_.size() : end + 3;
            } else if (looking("<?")) {
                const std::size_t end = in_.find("?>", i_ + 2);
                i_ = end == std::string_view::npos ? in_.size() : end + 2;
            } else if (looking("<!")) {
                // A doctype, possibly with an internal subset in brackets. The
                // subset may contain '>', so bracket depth is tracked rather
                // than scanning for the first one.
                i_ += 2;
                int brackets = 0;
                while (!atEnd()) {
                    if (in_[i_] == '[') {
                        ++brackets;
                    } else if (in_[i_] == ']') {
                        --brackets;
                    } else if (in_[i_] == '>' && brackets <= 0) {
                        ++i_;
                        break;
                    }
                    ++i_;
                }
            } else {
                return;
            }
        }
    }

    std::string_view name() {
        const std::size_t begin = i_;
        while (!atEnd() && isNameChar(in_[i_])) {
            ++i_;
        }
        return in_.substr(begin, i_ - begin);
    }

    Result<Node> element() {
        ++i_;  // '<'
        Node node;
        node.name = std::string{name()};
        if (node.name.empty()) {
            return fail("an element with no name");
        }

        for (;;) {
            skipSpace();
            if (atEnd()) {
                return fail("the file ends inside <" + node.name + ">");
            }
            if (looking("/>")) {
                i_ += 2;
                return node;
            }
            if (looking(">")) {
                ++i_;
                break;
            }
            const std::string_view key = name();
            if (key.empty()) {
                return fail("an attribute with no name in <" + node.name + ">");
            }
            skipSpace();
            if (!looking("=")) {
                // Valueless, which XML does not allow and HTML-ish exports
                // occasionally contain. Kept as empty rather than refused.
                node.attributes.emplace_back(key, std::string{});
                continue;
            }
            ++i_;
            skipSpace();
            if (atEnd() || (in_[i_] != '"' && in_[i_] != '\'')) {
                return fail("an unquoted attribute value in <" + node.name + ">");
            }
            const char quote = in_[i_++];
            const std::size_t begin = i_;
            while (!atEnd() && in_[i_] != quote) {
                ++i_;
            }
            if (atEnd()) {
                return fail("an unterminated attribute value in <" + node.name + ">");
            }
            node.attributes.emplace_back(key, unescape(in_.substr(begin, i_ - begin)));
            ++i_;
        }

        return content(std::move(node));
    }

    Result<Node> content(Node node) {
        std::string text;
        for (;;) {
            if (atEnd()) {
                return fail("the file ends inside <" + node.name + ">");
            }
            if (in_[i_] != '<') {
                const std::size_t begin = i_;
                while (!atEnd() && in_[i_] != '<') {
                    ++i_;
                }
                text += unescape(in_.substr(begin, i_ - begin));
                continue;
            }
            if (looking("</")) {
                i_ += 2;
                const std::string_view closing = name();
                if (closing != node.name) {
                    return fail("</" + std::string{closing} + "> closes <" + node.name + ">");
                }
                skipSpace();
                if (!looking(">")) {
                    return fail("a malformed closing tag for <" + node.name + ">");
                }
                ++i_;
                node.text = std::string{trimmed(text)};
                return node;
            }
            if (looking("<![CDATA[")) {
                const std::size_t begin = i_ + 9;
                const std::size_t end = in_.find("]]>", begin);
                if (end == std::string_view::npos) {
                    return fail("an unterminated CDATA section");
                }
                text += in_.substr(begin, end - begin);
                i_ = end + 3;
                continue;
            }
            if (looking("<!--") || looking("<?") || looking("<!")) {
                skipMisc();
                continue;
            }
            auto child = element();
            if (!child) {
                return child;
            }
            node.children.push_back(std::move(*child));
        }
    }

    std::string_view in_;
    std::size_t i_{0};
};

}  // namespace

const Node* Node::child(std::string_view childName) const {
    const auto found = std::find_if(children.begin(), children.end(),
                                    [&](const Node& n) { return n.name == childName; });
    return found == children.end() ? nullptr : &*found;
}

std::vector<const Node*> Node::childrenNamed(std::string_view childName) const {
    std::vector<const Node*> out;
    for (const Node& n : children) {
        if (n.name == childName) {
            out.push_back(&n);
        }
    }
    return out;
}

std::string Node::textOf(std::string_view childName, std::string_view fallback) const {
    const Node* found = child(childName);
    return found == nullptr || found->text.empty() ? std::string{fallback} : found->text;
}

std::int64_t Node::intOf(std::string_view childName, std::int64_t fallback) const {
    const Node* found = child(childName);
    if (found == nullptr) {
        return fallback;
    }
    const std::string_view digits = trimmed(found->text);
    std::int64_t value = 0;
    const char* first = digits.data();
    const char* last = first + digits.size();
    const auto parsed = std::from_chars(first, last, value);
    return parsed.ec == std::errc{} ? value : fallback;
}

bool Node::boolOf(std::string_view childName, bool fallback) const {
    const Node* found = child(childName);
    if (found == nullptr || found->text.empty()) {
        return fallback;
    }
    std::string lowered = found->text;
    std::transform(lowered.begin(), lowered.end(), lowered.begin(), [](char c) {
        return static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    });
    if (lowered == "true" || lowered == "1" || lowered == "yes") {
        return true;
    }
    if (lowered == "false" || lowered == "0" || lowered == "no") {
        return false;
    }
    return fallback;
}

std::string Node::attribute(std::string_view attributeName) const {
    const auto found = std::find_if(attributes.begin(), attributes.end(),
                                    [&](const auto& pair) { return pair.first == attributeName; });
    return found == attributes.end() ? std::string{} : found->second;
}

Node& Node::add(std::string childName) {
    Node child;
    child.name = std::move(childName);
    children.push_back(std::move(child));
    return children.back();
}

Node& Node::add(std::string childName, std::string childText) {
    Node& child = add(std::move(childName));
    child.text = std::move(childText);
    return child;
}

Node& Node::add(std::string childName, std::int64_t childValue) {
    return add(std::move(childName), std::to_string(childValue));
}

Node& Node::addBool(std::string childName, bool value) {
    return add(std::move(childName), std::string{value ? "TRUE" : "FALSE"});
}

void Node::setAttribute(std::string attributeName, std::string value) {
    attributes.emplace_back(std::move(attributeName), std::move(value));
}

std::string write(const Node& root, std::string_view prologue) {
    std::string out;
    out += prologue;
    writeNode(out, root, 0);
    return out;
}

Result<Node> parse(std::string_view text) {
    Parser parser{text};
    return parser.document();
}

}  // namespace zaro::io::xml
