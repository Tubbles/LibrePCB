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

#ifndef LIBREPCB_CORE_DIFFERENTIALPAIRS_H
#define LIBREPCB_CORE_DIFFERENTIALPAIRS_H

/*******************************************************************************
 *  Includes
 ******************************************************************************/
#include <QtCore>

#include <optional>

/*******************************************************************************
 *  Namespace / Forward Declarations
 ******************************************************************************/
namespace librepcb {

class Circuit;
class NetSignal;

/*******************************************************************************
 *  Class DifferentialPairs
 ******************************************************************************/

/**
 * @brief Detection of differential pairs by net signal naming convention
 *
 * LibrePCB has no stored differential pair object: the project file format
 * carries no such element, so pairs are *derived* from the net signal names
 * instead. This mirrors the convention KiCad uses in `BOARD::MatchDpSuffix()`,
 * which makes the very same schematics work in both tools without any extra
 * annotation.
 *
 * Two net signals form a differential pair when their names differ in exactly
 * one polarity character. To find that character, the name is scanned from its
 * *end* towards the front:
 *
 *   - Digits (`0`..`9`) and underscores are skipped, they may trail the
 *     polarity character as a lane index (`LVDS_P0`, `TX+_1`).
 *   - The first character which is not skipped decides:
 *     - `+` is positive, its complement is `-`
 *     - `-` is negative, its complement is `+`
 *     - `P` (uppercase only) is positive, its complement is `N`
 *     - `N` (uppercase only) is negative, its complement is `P`
 *     - anything else means the name is not part of a pair.
 *
 * The complement name is the name with that single character replaced, so
 * `USB_DP` <-> `USB_DN`, `CLK+` <-> `CLK-`, `LVDS_P0` <-> `LVDS_N0` and
 * `TX+_1` <-> `TX-_1` are pairs, while `clk_p` (lowercase), `DATA0` (no
 * polarity character at all) and `VCC` are not.
 *
 * A pair only *exists* when both halves exist as net signals in the circuit.
 * A name alone never makes a pair, ::librepcb::DifferentialPairs::polarityOf()
 * and ::librepcb::DifferentialPairs::complementNameOf() answer questions about
 * names, everything else answers questions about a concrete circuit.
 *
 * Because the pairs are derived, they are read only. Editing them means
 * renaming the net signals. A stored pair concept (with an explicit,
 * user-editable assignment and per-pair design rules) has to wait for the next
 * file format window, ::librepcb::Project's format is stable at the moment.
 */
class DifferentialPairs final {
public:
  /**
   * @brief Both halves of one differential pair
   *
   * Both members are always non-null.
   */
  struct Pair {
    NetSignal* positive;  ///< The `+` / `P` half of the pair.
    NetSignal* negative;  ///< The `-` / `N` half of the pair.
  };

  // Constructors / Destructor
  DifferentialPairs() = delete;
  DifferentialPairs(const DifferentialPairs& other) = delete;
  ~DifferentialPairs() = delete;

  // Operator Overloadings
  DifferentialPairs& operator=(const DifferentialPairs& rhs) = delete;

  // Static Methods

  /**
   * @brief Determine the polarity a net signal name expresses
   *
   * @param name  Any net signal name.
   *
   * @retval 1    The name is the positive half of a pair (`+` or `P`).
   * @retval -1   The name is the negative half of a pair (`-` or `N`).
   * @retval 0    The name carries no polarity character.
   */
  static int polarityOf(const QString& name) noexcept;

  /**
   * @brief Determine the name of the complementary net signal
   *
   * @param name  Any net signal name.
   *
   * @return The name with its polarity character replaced by the complementary
   *         one, or `std::nullopt` if the name carries no polarity character.
   *         The returned name is not looked up anywhere, it may well belong to
   *         no net signal at all.
   */
  static std::optional<QString> complementNameOf(const QString& name) noexcept;

  /**
   * @brief Get the net signal on the other half of a differential pair
   *
   * @param netSignal   The net signal to get the partner of.
   *
   * @return The complementary net signal, or `nullptr` if the name carries no
   *         polarity character or the complement does not exist in the
   *         circuit.
   */
  static NetSignal* partnerOf(const NetSignal& netSignal) noexcept;

  /**
   * @brief Get the differential pair a net signal belongs to
   *
   * @param netSignal   One half of the pair to get.
   *
   * @return The pair with both halves sorted by polarity, or `std::nullopt` if
   *         the net signal is not part of a pair. Note that the passed net
   *         signal must be added to its circuit, an orphaned net signal never
   *         forms a pair.
   */
  static std::optional<Pair> pairOf(const NetSignal& netSignal) noexcept;

  /**
   * @brief Get all differential pairs of a circuit
   *
   * @param circuit   The circuit to scan.
   *
   * @return Every pair exactly once (not once per half), ordered by the name
   *         of the positive net signal to keep the result deterministic.
   */
  static QVector<Pair> allPairs(const Circuit& circuit) noexcept;
};

/*******************************************************************************
 *  End of File
 ******************************************************************************/

}  // namespace librepcb

#endif
