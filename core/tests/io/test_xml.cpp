// The hand-written XML reader and writer behind the Premiere interchange.
//
// Tested directly rather than only through a timeline, because a parser fails
// at its edges and a timeline exercises only its middle.

#include <cstdint>
#include <string>

#include <catch2/catch_test_macros.hpp>

#include "Xml.h"

using namespace zaro;
using zaro::io::xml::Node;

TEST_CASE("An element tree round trips", "[io][xml]") {
    Node root;
    root.name = "xmeml";
    root.setAttribute("version", "4");
    Node& sequence = root.add("sequence");
    sequence.setAttribute("id", "sequence-1");
    sequence.add("name", std::string{"A cut"});
    sequence.add("duration", std::int64_t{500});
    sequence.addBool("enabled", true);

    const std::string text = io::xml::write(root, "<?xml version=\"1.0\"?>\n");
    const auto back = io::xml::parse(text);
    REQUIRE(back);
    CHECK(back->name == "xmeml");
    CHECK(back->attribute("version") == "4");
    const Node* readSequence = back->child("sequence");
    REQUIRE(readSequence != nullptr);
    CHECK(readSequence->attribute("id") == "sequence-1");
    CHECK(readSequence->textOf("name") == "A cut");
    CHECK(readSequence->intOf("duration") == 500);
    CHECK(readSequence->boolOf("enabled", false));
}

TEST_CASE("The five predefined entities survive both directions", "[io][xml]") {
    Node root;
    root.name = "clip";
    root.setAttribute("note", R"(a "quoted" & <angled> one)");
    root.add("name", std::string{R"(Ben & Jerry's <take 2>)"});

    const std::string text = io::xml::write(root);
    // Escaped on the way out, or the document is not XML at all.
    CHECK(text.find("&amp;") != std::string::npos);
    CHECK(text.find("&lt;take 2&gt;") != std::string::npos);
    CHECK(text.find("&quot;quoted&quot;") != std::string::npos);

    const auto back = io::xml::parse(text);
    REQUIRE(back);
    CHECK(back->textOf("name") == R"(Ben & Jerry's <take 2>)");
    CHECK(back->attribute("note") == R"(a "quoted" & <angled> one)");
}

TEST_CASE("Numeric character references become UTF-8", "[io][xml]") {
    const auto back = io::xml::parse("<name>caf&#233; &#x2014; b&#xe9;ta</name>");
    REQUIRE(back);
    CHECK(back->text == "caf\xc3\xa9 \xe2\x80\x94 b\xc3\xa9ta");
}

TEST_CASE("A bare ampersand is kept rather than refused", "[io][xml]") {
    // Other programs write clip names containing one. Losing a two-hour cut
    // over a character every browser forgives would help nobody.
    const auto back = io::xml::parse("<name>Q &amp; A & more</name>");
    REQUIRE(back);
    CHECK(back->text == "Q & A & more");
}

TEST_CASE("CDATA, comments, the declaration and a doctype are all skipped over", "[io][xml]") {
    const auto back = io::xml::parse(
        "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
        "<!DOCTYPE xmeml>\n"
        "<!-- a comment with <tags> and an unclosed \" quote -->\n"
        "<xmeml version=\"4\">\n"
        "  <!-- another -->\n"
        "  <name><![CDATA[raw <not an element> & unescaped]]></name>\n"
        "</xmeml>");
    REQUIRE(back);
    CHECK(back->name == "xmeml");
    CHECK(back->textOf("name") == "raw <not an element> & unescaped");
}

TEST_CASE("A doctype with an internal subset does not end at its first '>'", "[io][xml]") {
    const auto back = io::xml::parse(
        "<!DOCTYPE xmeml [ <!ELEMENT xmeml ANY> ]>\n"
        "<xmeml><name>after</name></xmeml>");
    REQUIRE(back);
    CHECK(back->textOf("name") == "after");
}

TEST_CASE("Self-closing and empty elements are the same thing to a reader", "[io][xml]") {
    const auto back = io::xml::parse(R"(<track><file id="file-1"/><enabled></enabled></track>)");
    REQUIRE(back);
    const Node* file = back->child("file");
    REQUIRE(file != nullptr);
    CHECK(file->children.empty());
    CHECK(file->attribute("id") == "file-1");
    // A missing element and an empty one both fall through to the fallback,
    // which is what lets a reader treat every field of this format as optional.
    CHECK(back->boolOf("enabled", true));
    CHECK(back->boolOf("locked", true));
}

TEST_CASE("Malformed documents are refused, with an offset", "[io][xml]") {
    CHECK_FALSE(io::xml::parse(""));
    CHECK_FALSE(io::xml::parse("no markup at all"));
    CHECK_FALSE(io::xml::parse("<xmeml><name>truncated"));
    CHECK_FALSE(io::xml::parse("<xmeml><name>crossed</xmeml></name>"));
    CHECK_FALSE(io::xml::parse("<xmeml version=4></xmeml>"));
    // An empty root, on the other hand, is a document. Whether it says anything
    // worth reading is the timeline reader's question, not the parser's.
    CHECK(io::xml::parse("<xmeml></xmeml>"));

    const auto refused = io::xml::parse("<xmeml><name>truncated");
    REQUIRE_FALSE(refused);
    CHECK(refused.error().message().find("byte") != std::string::npos);
}

TEST_CASE("Text is trimmed and children are found in document order", "[io][xml]") {
    const auto back = io::xml::parse(
        "<track>\n"
        "  <name>\n    spaced out\n  </name>\n"
        "  <clipitem><name>first</name></clipitem>\n"
        "  <clipitem><name>second</name></clipitem>\n"
        "</track>");
    REQUIRE(back);
    CHECK(back->textOf("name") == "spaced out");
    const auto items = back->childrenNamed("clipitem");
    REQUIRE(items.size() == 2);
    CHECK(items[0]->textOf("name") == "first");
    CHECK(items[1]->textOf("name") == "second");
}

TEST_CASE("A number that is not a number falls back rather than reading as zero", "[io][xml]") {
    // Zero is a position on a timeline. Silently returning it for a field that
    // could not be read would place a clip at the head of the sequence.
    const auto back = io::xml::parse("<clipitem><start>not a number</start></clipitem>");
    REQUIRE(back);
    CHECK(back->intOf("start", -1) == -1);
    CHECK(back->intOf("end", -1) == -1);
}
