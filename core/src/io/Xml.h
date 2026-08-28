// A small XML tree, and the reader and writer for it.
//
// Internal to core/src/io. It exists because core links no toolkit -- no Qt, no
// libxml -- and one interchange format in this program is XML rather than JSON.
// Adding a dependency for it would put an XML library on the include path of
// every consumer of core, to serve one file format.
//
// **Deliberately not a general XML implementation.** There are no namespaces,
// no schema validation, no DTD resolution and no mixed content: a node has
// character data or it has element children, and an interchange format that
// needed more than that would be a reason to take the dependency after all.
// What is here is what FCP7 XML uses, done exactly: elements, attributes, text,
// the five predefined entities, numeric character references, CDATA, comments,
// the XML declaration and a doctype.
#pragma once

#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "zaro/core/Error.h"

namespace zaro::io::xml {

/// One element.
struct Node {
    std::string name;
    std::vector<std::pair<std::string, std::string>> attributes;
    /// The character data directly inside, with surrounding whitespace
    /// stripped. Empty for an element that holds other elements.
    std::string text;
    std::vector<Node> children;

    /// The first child with this name, or nullptr.
    [[nodiscard]] const Node* child(std::string_view childName) const;
    /// Every child with this name, in document order.
    [[nodiscard]] std::vector<const Node*> childrenNamed(std::string_view childName) const;

    /// The text of the first child with this name, or `fallback` when there is
    /// no such child. A missing element and an empty one are the same thing to
    /// a reader, which is what makes this the right shape for reading a format
    /// whose fields are all optional in practice.
    [[nodiscard]] std::string textOf(std::string_view childName,
                                     std::string_view fallback = {}) const;
    /// The same, parsed. `fallback` stands in for both a missing element and
    /// one whose text is not a number.
    [[nodiscard]] std::int64_t intOf(std::string_view childName, std::int64_t fallback = 0) const;
    /// FCP7 XML writes booleans as the words TRUE and FALSE.
    [[nodiscard]] bool boolOf(std::string_view childName, bool fallback) const;

    [[nodiscard]] std::string attribute(std::string_view attributeName) const;

    /// Append an empty child and return it, for building a tree top down.
    ///
    /// The reference is invalidated by the next `add` on the *same* parent, as
    /// any reference into a vector is. Building depth first -- take a child,
    /// fill it in, then come back for the next sibling -- never trips over
    /// that, and is how every caller here is written.
    Node& add(std::string childName);
    /// Append a leaf carrying text.
    Node& add(std::string childName, std::string childText);
    Node& add(std::string childName, std::int64_t childValue);
    /// A leaf carrying TRUE or FALSE.
    Node& addBool(std::string childName, bool value);

    void setAttribute(std::string attributeName, std::string value);
};

/// Serialise, tab indented, with `prologue` written ahead of the root.
///
/// The prologue is the caller's because it is format specific: the XML
/// declaration is common to every XML file, and the doctype line an application
/// expects to see is not.
[[nodiscard]] std::string write(const Node& root, std::string_view prologue = {});

/// Parse a document and hand back its root element.
///
/// Errors are `InvalidData` and say where they are: a byte offset is what makes
/// a truncated 40MB export from another program findable.
[[nodiscard]] Result<Node> parse(std::string_view text);

}  // namespace zaro::io::xml
