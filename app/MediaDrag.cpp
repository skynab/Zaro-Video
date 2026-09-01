#include "MediaDrag.h"

#include <QByteArray>
#include <QDataStream>
#include <QIODevice>
#include <QMimeData>
#include <cstdint>

namespace zaro::app {
namespace {

/// Named for this application, so a drag from another program that happens to
/// carry two numbers cannot be mistaken for one of ours.
constexpr const char* kMimeType = "application/x-cutreel-media";

}  // namespace

const char* mediaDragMimeType() {
    return kMimeType;
}

QMimeData* encodeMediaDrag(const MediaDrag& dragged) {
    QByteArray payload;
    QDataStream out{&payload, QIODevice::WriteOnly};
    out << static_cast<quint64>(dragged.media.value())
        << static_cast<quint64>(dragged.subclip.value());

    auto* mime = new QMimeData;
    mime->setData(kMimeType, payload);
    return mime;
}

std::optional<MediaDrag> decodeMediaDrag(const QMimeData* mime) {
    if (mime == nullptr || !mime->hasFormat(kMimeType)) {
        return std::nullopt;
    }
    QByteArray payload = mime->data(kMimeType);
    QDataStream in{&payload, QIODevice::ReadOnly};
    quint64 media = 0;
    quint64 subclip = 0;
    in >> media >> subclip;
    if (in.status() != QDataStream::Ok) {
        return std::nullopt;
    }
    MediaDrag dragged;
    dragged.media = model::MediaRefId{static_cast<std::uint64_t>(media)};
    dragged.subclip = model::SubclipId{static_cast<std::uint64_t>(subclip)};
    if (!dragged.media.isValid()) {
        return std::nullopt;  // a heading, or a row with nothing behind it
    }
    return dragged;
}

}  // namespace zaro::app
