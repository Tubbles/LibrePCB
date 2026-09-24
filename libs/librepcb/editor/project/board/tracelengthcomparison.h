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

#ifndef LIBREPCB_EDITOR_TRACELENGTHCOMPARISON_H
#define LIBREPCB_EDITOR_TRACELENGTHCOMPARISON_H

/*******************************************************************************
 *  Includes
 ******************************************************************************/
#include <librepcb/core/types/length.h>

#include <QtCore>

#include <optional>

/*******************************************************************************
 *  Namespace / Forward Declarations
 ******************************************************************************/
namespace librepcb {
namespace editor {

/*******************************************************************************
 *  Types
 ******************************************************************************/

/**
 * @brief Identity of a trace segment anchor
 *
 * Two trace segments are considered connected if they share an anchor ID.
 * The board editor uses the address of the corresponding
 * ::librepcb::BI_NetLineAnchor, unit tests use arbitrary distinct numbers.
 */
using TraceLengthAnchorId = quintptr;

/**
 * @brief One selected trace segment, reduced to what the comparison needs
 *
 * This keeps the comparison free of any dependency on the board and its
 * graphics scene, which makes it testable without a project.
 */
struct TraceLengthSegment {
  QString netName;  ///< Name of the net this segment belongs to
  TraceLengthAnchorId startAnchor;  ///< Anchor at one end of the segment
  TraceLengthAnchorId endAnchor;  ///< Anchor at the other end of the segment
  UnsignedLength length;  ///< Length of the segment itself
};

/**
 * @brief One bus, reduced to what the comparison needs
 */
struct TraceLengthBus {
  QString name;  ///< Name of the bus
  QSet<QString> netNames;  ///< Names of the nets connected to the bus
  /// The maximum trace length difference configured on the bus, if any
  std::optional<UnsignedLength> maxTraceLengthDifference;
};

/**
 * @brief The user supplied parameters of a trace length comparison
 */
struct TraceLengthComparisonSettings {
  /// Unit interval in picoseconds, i.e. 1/rate for a data signal or the
  /// period for a clock. Must be greater than zero.
  qreal unitIntervalPs = 1000;
  /// The tolerated delay difference between the nets, in picoseconds.
  qreal allowedSkewPs = 100;
  /// Effective relative permittivity seen by the traces. Must be >= 1.
  qreal effectiveDielectricConstant = 4.0;
};

/**
 * @brief Why the selected segments of a net could not be measured
 */
enum class TraceLengthWarning {
  None,  ///< The length could be determined
  Branched,  ///< The selected segments of the net branch
  Disconnected,  ///< Not all selected segments of the net are connected
};

/**
 * @brief The result of measuring the selected segments of a single net
 */
struct TraceLengthChain {
  UnsignedLength length{0};  ///< Only meaningful without a warning
  TraceLengthWarning warning = TraceLengthWarning::None;
};

/**
 * @brief One row of the trace length comparison, i.e. one net
 *
 * If #warning is set, none of the numeric members are meaningful.
 */
struct TraceLengthComparisonRow {
  QString netName;
  TraceLengthWarning warning = TraceLengthWarning::None;
  UnsignedLength length{0};
  qreal delayPs = 0;
  /// Length of this net minus length of the longest net, i.e. <= 0.
  Length differenceToLongest{0};
  /// Delay of this net minus delay of the longest net, i.e. <= 0.
  qreal differenceToLongestPs = 0;
  /// True if the delay difference is within the allowed skew.
  bool inSpec = true;
};

/*******************************************************************************
 *  Free Functions
 ******************************************************************************/

/**
 * @brief Speed of light in millimeters per picosecond
 */
constexpr qreal sSpeedOfLightMmPerPs = 0.299792458;

/**
 * @brief Signal propagation velocity on a trace
 *
 * The velocity is the speed of light divided by the square root of the
 * effective relative permittivity of the medium around the trace.
 *
 * @param effectiveDielectricConstant  Effective relative permittivity, values
 *                                     below 1 are clamped to 1.
 * @return Velocity in millimeters per picosecond.
 */
qreal traceVelocityMmPerPs(qreal effectiveDielectricConstant) noexcept;

/**
 * @brief Propagation delay of a trace of a given length
 *
 * @param length                       Length of the trace.
 * @param effectiveDielectricConstant  Effective relative permittivity.
 * @return Delay in picoseconds.
 */
qreal traceDelayPs(const Length& length,
                   qreal effectiveDielectricConstant) noexcept;

/**
 * @brief Unit interval of a data rate
 *
 * @param gigabitsPerSecond  Data rate in Gbit/s, values <= 0 yield 0.
 * @return The duration of one bit in picoseconds.
 */
qreal unitIntervalPsOfDataRate(qreal gigabitsPerSecond) noexcept;

/**
 * @brief Unit interval of a clock frequency
 *
 * @param megahertz  Clock frequency in MHz, values <= 0 yield 0.
 * @return The clock period in picoseconds.
 */
qreal unitIntervalPsOfFrequency(qreal megahertz) noexcept;

/**
 * @brief Get the names of all nets occurring in a set of segments
 */
QSet<QString> getTraceLengthNetNames(
    const QVector<TraceLengthSegment>& segments) noexcept;

/**
 * @brief Measure a set of trace segments as one single chain
 *
 * All passed segments must belong to the same net. Starting at the first
 * segment, the chain is traversed in both directions, exactly like the
 * single chain measurement of the select tool does. Branches are detected
 * up front, so the result does not depend on the order of the segments.
 *
 * @param segments  The segments to measure (of one net).
 * @return The total length, or a warning if the segments branch or are not
 *         all connected to each other.
 */
TraceLengthChain measureTraceLengthChain(
    const QVector<TraceLengthSegment>& segments) noexcept;

/**
 * @brief Compare the lengths of the selected traces of several nets
 *
 * The segments are grouped by net name, each group is measured as one chain
 * and then compared against the longest net. Nets which could not be
 * measured carry a warning instead and do not take part in the comparison.
 *
 * @param segments  The selected segments of all nets.
 * @param settings  The user supplied comparison parameters.
 * @return One row per net, sorted by net name.
 */
QVector<TraceLengthComparisonRow> compareTraceLengths(
    const QVector<TraceLengthSegment>& segments,
    const TraceLengthComparisonSettings& settings) noexcept;

/**
 * @brief Get the translated, human readable text of a warning
 *
 * @param warning  The warning to describe.
 * @return The text to show to the user, empty for
 *         ::librepcb::editor::TraceLengthWarning::None.
 */
QString traceLengthWarningText(TraceLengthWarning warning) noexcept;

/**
 * @brief Find the bus which suggests an allowed skew for the given nets
 *
 * Only a bus which contains *all* of the given nets and which defines a
 * maximum trace length difference qualifies. If several buses qualify, the
 * suggestion would be ambiguous and none is returned.
 *
 * @param buses     All buses of the circuit.
 * @param netNames  The nets taking part in the comparison.
 * @return The qualifying bus, if there is exactly one.
 */
std::optional<TraceLengthBus> findCommonTraceLengthBus(
    const QVector<TraceLengthBus>& buses,
    const QSet<QString>& netNames) noexcept;

/*******************************************************************************
 *  End of File
 ******************************************************************************/

}  // namespace editor
}  // namespace librepcb

#endif
