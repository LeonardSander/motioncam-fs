#pragma once

#include <QByteArray>
#include <QXmlStreamReader>
#include <QXmlStreamWriter>
#include <stdexcept>

namespace motioncam {

// Keep processing edits while removing GPS properties, including nested GPS
// directories. Reject unreadable XML rather than copying an unchecked packet.
inline QByteArray excludeSidecarGps(const QByteArray& source, bool xml) {
    if (!xml) {
        QByteArray result;
        bool gpsSection = false;
        for (const auto& line : source.split('\n')) {
            auto trimmed = line.trimmed();
            if (trimmed.startsWith("\xef\xbb\xbf")) trimmed.remove(0, 3);
            if (trimmed.startsWith('[') && trimmed.endsWith(']'))
                gpsSection = trimmed.toLower().contains("gps");
            const auto equals = trimmed.indexOf('=');
            const bool gpsProperty = equals >= 0 &&
                trimmed.left(equals).toLower().contains("gps");
            if (!gpsSection && !gpsProperty) {
                result += line;
                result += '\n';
            }
        }
        if (!source.endsWith('\n') && result.endsWith('\n')) result.chop(1);
        // split() includes an empty last line for files ending in a newline.
        if (source.endsWith('\n') && result.endsWith("\n\n")) result.chop(1);
        return result;
    }

    QXmlStreamReader reader(source);
    QByteArray result;
    QXmlStreamWriter writer(&result);
    bool changed = false;
    while (!reader.atEnd()) {
        reader.readNext();
        if (reader.isStartElement()) {
            if (reader.name().toString().contains("gps", Qt::CaseInsensitive)) {
                reader.skipCurrentElement();
                changed = true;
                continue;
            }
            writer.writeStartElement(reader.qualifiedName().toString());
            for (const auto& ns : reader.namespaceDeclarations())
                writer.writeNamespace(ns.namespaceUri().toString(), ns.prefix().toString());
            for (const auto& attribute : reader.attributes()) {
                if (attribute.name().toString().contains("gps", Qt::CaseInsensitive)) {
                    changed = true;
                    continue;
                }
                writer.writeAttribute(attribute.qualifiedName().toString(), attribute.value().toString());
            }
        } else if (!reader.hasError()) {
            writer.writeCurrentToken(reader);
        }
    }
    if (reader.hasError() || writer.hasError())
        throw std::runtime_error("Could not remove GPS metadata from XML sidecar");
    return changed ? result : source;
}

} // namespace motioncam
