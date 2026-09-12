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

#ifndef LIBREPCB_EDITOR_TRACELENGTHCOMPARISONDIALOG_H
#define LIBREPCB_EDITOR_TRACELENGTHCOMPARISONDIALOG_H

/*******************************************************************************
 *  Includes
 ******************************************************************************/
#include "tracelengthcomparison.h"

#include <QtCore>
#include <QtWidgets>

/*******************************************************************************
 *  Namespace / Forward Declarations
 ******************************************************************************/
namespace librepcb {

class LengthUnit;

namespace editor {

namespace Ui {
class TraceLengthComparisonDialog;
}

/*******************************************************************************
 *  Class TraceLengthComparisonDialog
 ******************************************************************************/

/**
 * @brief Dialog comparing the lengths of the selected traces of several nets
 *
 * The dialog is read-only, it just shows how the selected traces of the
 * involved nets compare to each other. The data rate, the allowed skew and
 * the effective dielectric constant are remembered in the client settings.
 *
 * If exactly one bus contains all compared nets and defines a maximum trace
 * length difference, that difference is preselected as the allowed skew
 * (overriding the remembered one) because it is the value the schematic
 * actually asks for.
 */
class TraceLengthComparisonDialog final : public QDialog {
  Q_OBJECT

  /// The unit the data rate input is interpreted in
  enum class RateUnit {
    Gbps = 0,  ///< Data rate in gigabits per second
    Megahertz = 1,  ///< Clock frequency in megahertz
  };

  /// How the allowed skew is entered
  enum class SkewMode {
    PercentOfUnitInterval = 0,
    Picoseconds = 1,
    Length = 2,
  };

public:
  // Constructors / Destructor
  TraceLengthComparisonDialog() = delete;
  TraceLengthComparisonDialog(const TraceLengthComparisonDialog& other) =
      delete;
  TraceLengthComparisonDialog(const QVector<TraceLengthSegment>& segments,
                              const QVector<TraceLengthBus>& buses,
                              const LengthUnit& lengthUnit,
                              const QString& settingsPrefix,
                              QWidget* parent) noexcept;
  ~TraceLengthComparisonDialog() noexcept override;

  // Operator Overloadings
  TraceLengthComparisonDialog& operator=(
      const TraceLengthComparisonDialog& rhs) = delete;

private:  // Methods
  void loadClientSettings() noexcept;
  void saveClientSettings() noexcept;
  void applyBusDefault(const QVector<TraceLengthBus>& buses) noexcept;
  void updateSkewInputVisibility() noexcept;
  TraceLengthComparisonSettings getSettings() const noexcept;
  void updateTable() noexcept;

private:  // Data
  const QVector<TraceLengthSegment> mSegments;
  QScopedPointer<Ui::TraceLengthComparisonDialog> mUi;
  QString mSettingsPrefix;
  /// True if the allowed skew was preset from a bus instead of the settings
  bool mBusDefaultApplied;
};

/*******************************************************************************
 *  End of File
 ******************************************************************************/

}  // namespace editor
}  // namespace librepcb

#endif
