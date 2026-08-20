#include <catch2/catch_test_macros.hpp>

#include "zaro/core/model/Project.h"

using namespace zaro;

namespace {

model::MediaRef withProxy(model::Project& project, const std::string& path,
                          const std::string& proxy) {
    model::MediaRef ref;
    ref.id = project.ids().next<model::MediaRefTag>();
    ref.path = path;
    ref.proxyPath = proxy;
    ref.name = "clip";
    return ref;
}

}  // namespace

TEST_CASE("A proxy is used only when there is one and it is switched on", "[io][proxy]") {
    model::Project project;
    const model::MediaRef proxied = withProxy(project, "/media/a.mov", "/proxies/a.mov");
    const model::MediaRef plain = withProxy(project, "/media/b.mov", "");

    // Off by default: the originals are what somebody shot.
    CHECK_FALSE(project.usingProxies());
    CHECK(project.resolvedPath(proxied) == "/media/a.mov");

    project.setUsingProxies(true);
    CHECK(project.resolvedPath(proxied) == "/proxies/a.mov");
    // Media with no proxy carries on from the original rather than failing to
    // resolve: a project part-way through being proxied still has to play.
    CHECK(project.resolvedPath(plain) == "/media/b.mov");

    project.setUsingProxies(false);
    CHECK(project.resolvedPath(proxied) == "/media/a.mov");
}

TEST_CASE("The toggle is a property of the project, not of a clip", "[io][proxy]") {
    // Nobody wants some shots on proxies and some not, and the whole reason to
    // be on proxies is that the machine cannot keep up with the originals.
    model::Project project;
    const model::MediaRef first = withProxy(project, "/media/a.mov", "/proxies/a.mov");
    const model::MediaRef second = withProxy(project, "/media/b.mov", "/proxies/b.mov");

    project.setUsingProxies(true);
    CHECK(project.resolvedPath(first) == "/proxies/a.mov");
    CHECK(project.resolvedPath(second) == "/proxies/b.mov");
}
