/*
 * LibrePCB - Professional EDA for everyone!
 * Copyright (C) 2013 LibrePCB Developers, see AUTHORS.md for contributors.
 * https://librepcb.org/
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

/*******************************************************************************
 *  Includes
 ******************************************************************************/
#include "tracelengthcomparison.h"

#include <QtCore>

#include <algorithm>
#include <cmath>

/*******************************************************************************
 *  Namespace
 ******************************************************************************/
namespace librepcb {
namespace editor {

/*******************************************************************************
 *  Static Helpers
 ******************************************************************************/

/**
 * @brief Check whether any anchor of a net joins more than two segments
 *
 * An anchor with three or more segments is exactly the situation the chain
 * traversal of the select tool reports as a branch, but detecting it up front
 * makes the result independent of which segment the traversal starts at.
 *
 * @param segments  All segments of the net.
 * @return True if the segments branch somewhere.
 */
static bool traceLengthSegmentsBranch(
    const QVector<TraceLengthSegment>& segments) noexcept {
  QHash<TraceLengthAnchorId, int> count;
  foreach (const TraceLengthSegment& segment, segments) {
    if ((++count[segment.startAnchor]) > 2) {
      return true;
    }
    if ((++count[segment.endAnchor]) > 2) {
      return true;
    }
  }
  return false;
}

/**
 * @brief Walk the chain of segments starting at one anchor
 *
 * This is the same traversal as
 * ::librepcb::editor::BoardEditorState_Select::measureLengthInDirection(),
 * just on the reduced segment representation. The segments must not branch.
 *
 * @param segments  All segments of the net.
 * @param anchor    The anchor to start at.
 * @param visited   Indices of the already visited segments, extended in
 *                  place.
 * @param length    The total length, extended in place.
 */
static void walkTraceLengthChain(const QVector<TraceLengthSegment>& segments,
                                 TraceLengthAnchorId anchor,
                                 QSet<int>& visited,
                                 UnsignedLength& length) noexcept {
  for (;;) {
    int next = -1;
    for (int i = 0; (i < segments.count()) && (next < 0); ++i) {
      const TraceLengthSegment& segment = segments.at(i);
      if ((!visited.contains(i)) &&
          ((segment.startAnchor == anchor) || (segment.endAnchor == anchor))) {
        next = i;
      }
    }
    if (next < 0) {
      return;
    }
    const TraceLengthSegment& segment = segments.at(next);
    length += segment.length;
    visited.insert(next);
    if (segment.startAnchor == anchor) {
      anchor = segment.endAnchor;
    } else {
      anchor = segment.startAnchor;
    }
  }
}

/*******************************************************************************
 *  Free Functions
 ******************************************************************************/

qreal traceVelocityMmPerPs(qreal effectiveDielectricConstant) noexcept {
  return sSpeedOfLightMmPerPs /
      std::sqrt(std::max(effectiveDielectricConstant, qreal(1)));
}

qreal traceDelayPs(const Length& length,
                   qreal effectiveDielectricConstant) noexcept {
  return length.toMm() / traceVelocityMmPerPs(effectiveDielectricConstant);
}

qreal unitIntervalPsOfDataRate(qreal gigabitsPerSecond) noexcept {
  // One bit at 1 Gbit/s lasts 1 ns, i.e. 1000 ps.
  return (gigabitsPerSecond > 0) ? (1000 / gigabitsPerSecond) : qreal(0);
}

qreal unitIntervalPsOfFrequency(qreal megahertz) noexcept {
  // One period at 1 MHz lasts 1 us, i.e. 1e6 ps.
  return (megahertz > 0) ? (1e6 / megahertz) : qreal(0);
}

QSet<QString> getTraceLengthNetNames(
    const QVector<TraceLengthSegment>& segments) noexcept {
  QSet<QString> names;
  foreach (const TraceLengthSegment& segment, segments) {
    names.insert(segment.netName);
  }
  return names;
}

TraceLengthChain measureTraceLengthChain(
    const QVector<TraceLengthSegment>& segments) noexcept {
  TraceLengthChain result;
  if (segments.isEmpty()) {
    return result;
  }

  if (traceLengthSegmentsBranch(segments)) {
    result.warning = TraceLengthWarning::Branched;
    return result;
  }

  // Take the first segment, then traverse the chain first in one direction,
  // then in the other direction.
  QSet<int> visited{0};
  result.length = segments.first().length;
  walkTraceLengthChain(segments, segments.first().endAnchor, visited,
                       result.length);
  walkTraceLengthChain(segments, segments.first().startAnchor, visited,
                       result.length);
  if (visited.count() != segments.count()) {
    result.warning = TraceLengthWarning::Disconnected;
  }
  return result;
}

QVector<TraceLengthComparisonRow> compareTraceLengths(
    const QVector<TraceLengthSegment>& segments,
    const TraceLengthComparisonSettings& settings) noexcept {
  // Group the segments by net, keeping the nets sorted by name.
  QMap<QString, QVector<TraceLengthSegment>> segmentsByNet;
  foreach (const TraceLengthSegment& segment, segments) {
    segmentsByNet[segment.netName].append(segment);
  }

  // Measure each net on its own.
  QVector<TraceLengthComparisonRow> rows;
  UnsignedLength longest(0);
  for (auto it = segmentsByNet.begin(); it != segmentsByNet.end(); ++it) {
    const TraceLengthChain chain = measureTraceLengthChain(it.value());
    TraceLengthComparisonRow row;
    row.netName = it.key();
    row.warning = chain.warning;
    if (chain.warning == TraceLengthWarning::None) {
      row.length = chain.length;
      row.delayPs =
          traceDelayPs(*chain.length, settings.effectiveDielectricConstant);
      longest = std::max(longest, chain.length);
    }
    rows.append(row);
  }

  // Compare against the longest net.
  const qreal longestDelayPs =
      traceDelayPs(*longest, settings.effectiveDielectricConstant);
  for (TraceLengthComparisonRow& row : rows) {
    if (row.warning != TraceLengthWarning::None) {
      continue;
    }
    row.differenceToLongest = *row.length - *longest;
    row.differenceToLongestPs = row.delayPs - longestDelayPs;
    row.inSpec =
        (std::abs(row.differenceToLongestPs) <= settings.allowedSkewPs);
  }
  return rows;
}

QString traceLengthWarningText(TraceLengthWarning warning) noexcept {
  switch (warning) {
    case TraceLengthWarning::Branched:
      return QCoreApplication::translate(
          "TraceLengthComparison", "Selected trace segments may not branch!");
    case TraceLengthWarning::Disconnected:
      return QCoreApplication::translate(
          "TraceLengthComparison",
          "Not all selected trace segments are connected!");
    default:
      return QString();
  }
}

std::optional<TraceLengthBus> findCommonTraceLengthBus(
    const QVector<TraceLengthBus>& buses,
    const QSet<QString>& netNames) noexcept {
  if (netNames.isEmpty()) {
    return std::nullopt;
  }
  std::optional<TraceLengthBus> result;
  foreach (const TraceLengthBus& bus, buses) {
    if ((!bus.maxTraceLengthDifference) || (!bus.netNames.contains(netNames))) {
      continue;
    }
    if (result) {
      return std::nullopt;  // Ambiguous, don't suggest anything.
    }
    result = bus;
  }
  return result;
}

/*******************************************************************************
 *  End of File
 ******************************************************************************/

}  // namespace editor
}  // namespace librepcb
