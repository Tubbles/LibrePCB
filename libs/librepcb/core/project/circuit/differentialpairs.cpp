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
#include "differentialpairs.h"

#include "circuit.h"
#include "netsignal.h"

#include <QtCore>

#include <algorithm>

/*******************************************************************************
 *  Namespace
 ******************************************************************************/
namespace librepcb {

/*******************************************************************************
 *  Non-Member Functions
 ******************************************************************************/

/**
 * @brief The polarity character found at the end of a net signal name
 */
struct PolaritySuffix {
  int polarity = 0;  ///< 1, -1 or 0 if there is no polarity character.
  int index = -1;  ///< Position of the polarity character within the name.
  QChar complement;  ///< The character the complementary name carries there.
};

static PolaritySuffix findPolaritySuffix(const QString& name) noexcept {
  for (int index = name.length() - 1; index >= 0; --index) {
    const QChar character = name.at(index);
    if (((character >= QLatin1Char('0')) && (character <= QLatin1Char('9'))) ||
        (character == QLatin1Char('_'))) {
      continue;  // Lane index, the polarity character may still follow.
    } else if (character == QLatin1Char('+')) {
      return PolaritySuffix{1, index, QLatin1Char('-')};
    } else if (character == QLatin1Char('-')) {
      return PolaritySuffix{-1, index, QLatin1Char('+')};
    } else if (character == QLatin1Char('P')) {
      return PolaritySuffix{1, index, QLatin1Char('N')};
    } else if (character == QLatin1Char('N')) {
      return PolaritySuffix{-1, index, QLatin1Char('P')};
    } else {
      break;  // Any other character ends the suffix, this is no pair.
    }
  }
  return PolaritySuffix();
}

/*******************************************************************************
 *  Static Methods
 ******************************************************************************/

int DifferentialPairs::polarityOf(const QString& name) noexcept {
  return findPolaritySuffix(name).polarity;
}

std::optional<QString> DifferentialPairs::complementNameOf(
    const QString& name) noexcept {
  const PolaritySuffix suffix = findPolaritySuffix(name);
  if (suffix.polarity == 0) {
    return std::nullopt;
  }
  QString complement = name;
  complement[suffix.index] = suffix.complement;
  return complement;
}

NetSignal* DifferentialPairs::partnerOf(const NetSignal& netSignal) noexcept {
  const std::optional<QString> complement =
      complementNameOf(*netSignal.getName());
  if (!complement) {
    return nullptr;
  }
  return netSignal.getCircuit().getNetSignalByName(*complement);
}

std::optional<DifferentialPairs::Pair> DifferentialPairs::pairOf(
    const NetSignal& netSignal) noexcept {
  const int polarity = polarityOf(*netSignal.getName());
  if (polarity == 0) {
    return std::nullopt;
  }
  // Look the passed net signal up by name as well. Net signal names are unique
  // within a circuit, so this yields the very same object - but as a mutable
  // pointer. The identity check rejects a net signal which is not part of the
  // circuit (and thus forms no pair), even if some other net signal happens to
  // carry its name.
  NetSignal* self =
      netSignal.getCircuit().getNetSignalByName(*netSignal.getName());
  NetSignal* partner = partnerOf(netSignal);
  if ((self != &netSignal) || (!partner)) {
    return std::nullopt;
  }
  return (polarity > 0) ? Pair{self, partner} : Pair{partner, self};
}

QVector<DifferentialPairs::Pair> DifferentialPairs::allPairs(
    const Circuit& circuit) noexcept {
  // Anchoring on the positive half lists every pair exactly once, since the
  // complement of a positive name is always a negative one.
  QVector<Pair> pairs;
  foreach (NetSignal* netSignal, circuit.getNetSignals()) {
    if (polarityOf(*netSignal->getName()) > 0) {
      if (const std::optional<Pair> pair = pairOf(*netSignal)) {
        pairs.append(*pair);
      }
    }
  }
  std::sort(pairs.begin(), pairs.end(),
            [](const Pair& lhs, const Pair& rhs) noexcept {
              return *lhs.positive->getName() < *rhs.positive->getName();
            });
  return pairs;
}

/*******************************************************************************
 *  End of File
 ******************************************************************************/

}  // namespace librepcb
