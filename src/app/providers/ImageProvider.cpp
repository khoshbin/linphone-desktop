/*
 * ImageProvider.cpp
 * Copyright (C) 2017  Belledonne Communications, Grenoble, France
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301, USA.
 *
 *  Created on: June 19, 2017
 *      Author: Ronan Abhamon
 */

#include <algorithm>

#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QImage>
#include <QPainter>
#include <QSvgRenderer>
#include <QtDebug>
#include <QXmlStreamReader>

#include "../App.hpp"

#include "ImageProvider.hpp"

// Max image size in bytes. (100Kb)
#define MAX_IMAGE_SIZE 102400

using namespace std;

// =============================================================================

static void removeAttribute (QXmlStreamAttributes &readerAttributes, const QString &name) {
  auto it = find_if(readerAttributes.cbegin(), readerAttributes.cend(), [&name](const QXmlStreamAttribute &attribute) {
        return name == attribute.name() && !attribute.prefix().length();
      });
  if (it != readerAttributes.cend())
    readerAttributes.remove(static_cast<int>(distance(readerAttributes.cbegin(), it)));
}

static QByteArray buildByteArrayAttribute (const QByteArray &name, const QByteArray &value) {
  QByteArray attribute = name;
  attribute.append("=\"");
  attribute.append(value);
  attribute.append("\" ");
  return attribute;
}

static QByteArray parseFillAndStroke (
  QXmlStreamAttributes &readerAttributes,
  const Colors &colors
) {
  static QRegExp regex("^color-([^-]+)-(fill|stroke)$");

  QByteArray attributes;

  const QByteArray classAttr = readerAttributes.value("class").toLatin1();
  if (!classAttr.length())
    return attributes;

  for (const auto &classValue : classAttr.split(' ')) {
    regex.indexIn(classValue.trimmed());
    const QStringList list = regex.capturedTexts();
    if (list.length() != 3)
      continue;

    const QVariant colorValue = colors.property(list[1].toStdString().c_str());
    if (!colorValue.isValid()) {
      qWarning() << QStringLiteral("Color name `%1` does not exist.").arg(list[1]);
      continue;
    }

    removeAttribute(readerAttributes, list[2]);
    attributes.append(buildByteArrayAttribute(list[2].toLatin1(), colorValue.value<QColor>().name().toLatin1()));
  }

  return attributes;
}

static QByteArray parseStyle (
  const QXmlStreamAttributes &readerAttributes,
  const Colors &colors
) {
  return QByteArray();
  // TODO.

  static QRegExp regex("^color-([^-]+)-style-(fill|stroke)$");

  QByteArray attribute = "style=\"";

  QByteArray fill;
  QByteArray stroke;

  const QByteArray classAttr = readerAttributes.value("class").toLatin1();
  for (const auto &classValue : classAttr.split(' ')) {
    regex.indexIn(classValue.trimmed());
    const QStringList list = regex.capturedTexts();
    if (list.length() != 3)
      continue;
  }

  attribute.append("\" ");

  return attribute;
}

static QByteArray parseAttributes (const QXmlStreamReader &reader, const Colors &colors) {
  QXmlStreamAttributes readerAttributes = reader.attributes();

  QByteArray attributes = parseFillAndStroke(readerAttributes, colors);
  attributes.append(parseStyle(readerAttributes, colors));

  for (const auto &attribute : readerAttributes) {
    const QByteArray prefix = attribute.prefix().toLatin1();
    const QByteArray name = attribute.name().toLatin1();
    const QByteArray value = attribute.value().toLatin1();

    if (prefix.length() > 0) {
      attributes.append(prefix);
      attributes.append(":");
    }

    attributes.append(buildByteArrayAttribute(name, value));
  }

  return attributes;
}

static QByteArray parseDeclarations (const QXmlStreamReader &reader) {
  QByteArray declarations;
  for (const auto &declaration : reader.namespaceDeclarations()) {
    const QByteArray prefix = declaration.prefix().toLatin1();
    if (prefix.length() > 0) {
      declarations.append("xmlns:");
      declarations.append(prefix);
    } else
      declarations.append("xmlns");

    declarations.append("=\"");
    declarations.append(declaration.namespaceUri().toLatin1());
    declarations.append("\" ");
  }

  return declarations;
}

static QByteArray parseStartDocument (const QXmlStreamReader &reader) {
  QByteArray startDocument = "<?xml version=\"";
  startDocument.append(reader.documentVersion().toLatin1());
  startDocument.append("\" encoding=\"");
  startDocument.append(reader.documentEncoding().toLatin1());
  startDocument.append("\"?>");
  return startDocument;
}

static QByteArray parseStartElement (const QXmlStreamReader &reader, const Colors &colors) {
  QByteArray startElement = "<";
  startElement.append(reader.name().toLatin1());
  startElement.append(" ");
  startElement.append(parseAttributes(reader, colors));
  startElement.append(" ");
  startElement.append(parseDeclarations(reader));
  startElement.append(">");
  return startElement;
}

static QByteArray parseEndElement (const QXmlStreamReader &reader) {
  QByteArray endElement = "</";
  endElement.append(reader.name().toLatin1());
  endElement.append(">");
  return endElement;
}

// -----------------------------------------------------------------------------

static QByteArray computeContent (QFile &file) {
  const Colors *colors = App::getInstance()->getColors();

  QByteArray content;
  QXmlStreamReader reader(&file);
  while (!reader.atEnd())
    switch (reader.readNext()) {
      case QXmlStreamReader::Comment:
      case QXmlStreamReader::DTD:
      case QXmlStreamReader::EndDocument:
      case QXmlStreamReader::Invalid:
      case QXmlStreamReader::NoToken:
      case QXmlStreamReader::ProcessingInstruction:
        break;

      case QXmlStreamReader::StartDocument:
        content.append(parseStartDocument(reader));
        break;

      case QXmlStreamReader::StartElement:
        content.append(parseStartElement(reader, *colors));
        break;

      case QXmlStreamReader::EndElement:
        content.append(parseEndElement(reader));
        break;

      case QXmlStreamReader::Characters:
        content.append(reader.text().toLatin1());
        break;

      case QXmlStreamReader::EntityReference:
        content.append(reader.name().toLatin1());
        break;
    }

  return reader.hasError() ? QByteArray() : content;
}

// -----------------------------------------------------------------------------

const QString ImageProvider::PROVIDER_ID = "internal";

ImageProvider::ImageProvider () : QQuickImageProvider(
    QQmlImageProviderBase::Image,
    QQmlImageProviderBase::ForceAsynchronousImageLoading
  ) {}

// -----------------------------------------------------------------------------

QImage ImageProvider::requestImage (const QString &id, QSize *, const QSize &) {
  const QString path = QStringLiteral(":/assets/images/%1").arg(id);
  qInfo() << QStringLiteral("Image `%1` requested.").arg(path);

  QElapsedTimer timer;
  timer.start();

  // 1. Read and update XML content.
  QFile file(path);
  if (QFileInfo(file).size() > MAX_IMAGE_SIZE) {
    qWarning() << QStringLiteral("Unable to open large file: `%1`.").arg(path);
    return QImage();
  }

  if (!file.open(QIODevice::ReadOnly)) {
    qWarning() << QStringLiteral("Unable to open file: `%1`.").arg(path);
    return QImage();
  }

  const QByteArray content = computeContent(file);
  if (!content.length()) {
    qWarning() << QStringLiteral("Unable to parse file: `%1`.").arg(path);
    return QImage();
  }

  // 2. Build svg renderer.
  QSvgRenderer renderer(content);
  if (!renderer.isValid()) {
    qWarning() << QStringLiteral("Invalid svg file: `%1`.").arg(path);
    return QImage();
  }

  // 3. Create en empty image.
  const QRectF viewBox = renderer.viewBoxF();
  QImage image(static_cast<int>(viewBox.width()), static_cast<int>(viewBox.height()), QImage::Format_ARGB32);
  if (image.isNull()) {
    qWarning() << QStringLiteral("Unable to create image of size `(%1, %2)` from path: `%3`.")
      .arg(viewBox.width()).arg(viewBox.height()).arg(path);
    return QImage(); // Memory cannot be allocated.
  }
  image.fill(0x00000000);

  // 4. Paint!
  QPainter painter(&image);
  renderer.render(&painter);

  qInfo() << QStringLiteral("Image `%1` loaded in %2 milliseconds.").arg(path).arg(timer.elapsed());

  return image;
}