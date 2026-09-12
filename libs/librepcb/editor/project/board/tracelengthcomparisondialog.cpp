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
#include "tracelengthcomparisondialog.h"

#include "../../widgets/unsignedlengthedit.h"
#include "ui_tracelengthcomparisondialog.h"

#include <librepcb/core/types/lengthunit.h>
#include <librepcb/core/utils/toolbox.h>

#include <QtCore>
#include <QtWidgets>

/*******************************************************************************
 *  Namespace
 ******************************************************************************/
namespace librepcb {
namespace editor {

/*******************************************************************************
 *  Constructors / Destructor
 ******************************************************************************/

TraceLengthComparisonDialog::TraceLengthComparisonDialog(
    const QVector<TraceLengthSegment>& segments,
    const QVector<TraceLengthBus>& buses, const LengthUnit& lengthUnit,
    const QString& settingsPrefix, QWidget* parent) noexcept
  : QDialog(parent),
    mSegments(segments),
    mUi(new Ui::TraceLengthComparisonDialog),
    mSettingsPrefix(settingsPrefix),
    mBusDefaultApplied(false) {
  mUi->setupUi(this);
  mUi->cbxRateUnit->addItem("Gbps", static_cast<int>(RateUnit::Gbps));
  mUi->cbxRateUnit->addItem("MHz", static_cast<int>(RateUnit::Megahertz));
  mUi->cbxSkewMode->addItem(tr("% of Unit Interval"),
                            static_cast<int>(SkewMode::PercentOfUnitInterval));
  mUi->cbxSkewMode->addItem(tr("Picoseconds"),
                            static_cast<int>(SkewMode::Picoseconds));
  mUi->cbxSkewMode->addItem(tr("Length"), static_cast<int>(SkewMode::Length));
  mUi->edtSkewLength->configure(lengthUnit, LengthEditBase::Steps::generic(),
                                settingsPrefix % "/skew_length");
  mUi->lblFootnote->setText("ⓘ " % mUi->lblFootnote->text());

  // Setup the table.
  mUi->tableWidget->setColumnCount(7);
  mUi->tableWidget->setHorizontalHeaderLabels({
      tr("Net"),
      tr("Length [mm]"),
      tr("Length [in]"),
      tr("Delay [ps]"),
      tr("Difference [mm]"),
      tr("Difference [ps]"),
      tr("Status"),
  });
  mUi->tableWidget->setWordWrap(false);
  mUi->tableWidget->horizontalHeader()->setSectionResizeMode(
      QHeaderView::Interactive);
  mUi->tableWidget->verticalHeader()->setMinimumSectionSize(10);
  mUi->tableWidget->verticalHeader()->setVisible(false);
  mUi->tableWidget->setEditTriggers(QAbstractItemView::NoEditTriggers);
  mUi->tableWidget->setSelectionBehavior(QAbstractItemView::SelectRows);

  loadClientSettings();
  applyBusDefault(buses);
  updateSkewInputVisibility();

  // Recompute the table whenever an input changes.
  connect(mUi->spbxRate,
          static_cast<void (QDoubleSpinBox::*)(double)>(
              &QDoubleSpinBox::valueChanged),
          this, &TraceLengthComparisonDialog::updateTable);
  connect(mUi->cbxRateUnit,
          static_cast<void (QComboBox::*)(int)>(
              &QComboBox::currentIndexChanged),
          this, &TraceLengthComparisonDialog::updateTable);
  connect(mUi->cbxSkewMode,
          static_cast<void (QComboBox::*)(int)>(
              &QComboBox::currentIndexChanged),
          this, [this]() {
            updateSkewInputVisibility();
            updateTable();
          });
  connect(mUi->spbxSkewPercent,
          static_cast<void (QDoubleSpinBox::*)(double)>(
              &QDoubleSpinBox::valueChanged),
          this, &TraceLengthComparisonDialog::updateTable);
  connect(mUi->spbxSkewPs,
          static_cast<void (QDoubleSpinBox::*)(double)>(
              &QDoubleSpinBox::valueChanged),
          this, &TraceLengthComparisonDialog::updateTable);
  connect(mUi->edtSkewLength, &UnsignedLengthEdit::valueChanged, this,
          &TraceLengthComparisonDialog::updateTable);
  connect(mUi->spbxDielectricConstant,
          static_cast<void (QDoubleSpinBox::*)(double)>(
              &QDoubleSpinBox::valueChanged),
          this, &TraceLengthComparisonDialog::updateTable);
  connect(mUi->buttonBox, &QDialogButtonBox::rejected, this, &QDialog::reject);

  updateTable();
}

TraceLengthComparisonDialog::~TraceLengthComparisonDialog() noexcept {
  saveClientSettings();
}

/*******************************************************************************
 *  Private Methods
 ******************************************************************************/

void TraceLengthComparisonDialog::loadClientSettings() noexcept {
  try {
    QSettings cs;
    mUi->spbxRate->setValue(
        cs.value(mSettingsPrefix % "/rate", 1.0).toDouble());
    mUi->cbxRateUnit->setCurrentIndex(
        cs.value(mSettingsPrefix % "/rate_unit", 0).toInt());
    mUi->cbxSkewMode->setCurrentIndex(
        cs.value(mSettingsPrefix % "/skew_mode", 0).toInt());
    mUi->spbxSkewPercent->setValue(
        cs.value(mSettingsPrefix % "/skew_percent", 10.0).toDouble());
    mUi->spbxSkewPs->setValue(
        cs.value(mSettingsPrefix % "/skew_ps", 100.0).toDouble());
    mUi->edtSkewLength->setValue(UnsignedLength(Length::fromMm(
        cs.value(mSettingsPrefix % "/skew_length_value", "1.0").toString())));
    mUi->spbxDielectricConstant->setValue(
        cs.value(mSettingsPrefix % "/dielectric_constant", 4.0).toDouble());
    const QSize windowSize =
        cs.value(mSettingsPrefix % "/window_size").toSize();
    if (!windowSize.isEmpty()) {
      resize(windowSize);
    }
  } catch (const Exception& e) {
    qCritical() << "Failed to initialize trace length comparison dialog:"
                << e.getMsg();
  }
}

void TraceLengthComparisonDialog::saveClientSettings() noexcept {
  QSettings cs;
  cs.setValue(mSettingsPrefix % "/rate", mUi->spbxRate->value());
  cs.setValue(mSettingsPrefix % "/rate_unit",
              mUi->cbxRateUnit->currentIndex());
  cs.setValue(mSettingsPrefix % "/skew_percent",
              mUi->spbxSkewPercent->value());
  cs.setValue(mSettingsPrefix % "/skew_ps", mUi->spbxSkewPs->value());
  cs.setValue(mSettingsPrefix % "/dielectric_constant",
              mUi->spbxDielectricConstant->value());
  cs.setValue(mSettingsPrefix % "/window_size", size());
  // Don't remember a skew which came from a bus, it belongs to that bus and
  // not to the user's preferences.
  if (!mBusDefaultApplied) {
    cs.setValue(mSettingsPrefix % "/skew_mode",
                mUi->cbxSkewMode->currentIndex());
    cs.setValue(mSettingsPrefix % "/skew_length_value",
                mUi->edtSkewLength->getValue()->toMmString());
  }
}

void TraceLengthComparisonDialog::applyBusDefault(
    const QVector<TraceLengthBus>& buses) noexcept {
  const std::optional<TraceLengthBus> bus =
      findCommonTraceLengthBus(buses, getTraceLengthNetNames(mSegments));
  if (bus && bus->maxTraceLengthDifference) {
    mBusDefaultApplied = true;
    mUi->cbxSkewMode->setCurrentIndex(static_cast<int>(SkewMode::Length));
    mUi->edtSkewLength->setValue(*bus->maxTraceLengthDifference);
    mUi->lblSkewSource->setText(
        tr("Taken from the maximum trace length difference of bus \"%1\".")
            .arg(bus->name));
  }
  mUi->lblSkewSource->setVisible(mBusDefaultApplied);
}

void TraceLengthComparisonDialog::updateSkewInputVisibility() noexcept {
  const SkewMode mode =
      static_cast<SkewMode>(mUi->cbxSkewMode->currentIndex());
  mUi->spbxSkewPercent->setVisible(mode == SkewMode::PercentOfUnitInterval);
  mUi->spbxSkewPs->setVisible(mode == SkewMode::Picoseconds);
  mUi->edtSkewLength->setVisible(mode == SkewMode::Length);
}

TraceLengthComparisonSettings TraceLengthComparisonDialog::getSettings()
    const noexcept {
  TraceLengthComparisonSettings settings;
  settings.effectiveDielectricConstant =
      mUi->spbxDielectricConstant->value();
  const RateUnit rateUnit =
      static_cast<RateUnit>(mUi->cbxRateUnit->currentIndex());
  const qreal rate = mUi->spbxRate->value();
  if (rateUnit == RateUnit::Megahertz) {
    settings.unitIntervalPs = unitIntervalPsOfFrequency(rate);
  } else {
    settings.unitIntervalPs = unitIntervalPsOfDataRate(rate);
  }
  const SkewMode skewMode =
      static_cast<SkewMode>(mUi->cbxSkewMode->currentIndex());
  if (skewMode == SkewMode::Picoseconds) {
    settings.allowedSkewPs = mUi->spbxSkewPs->value();
  } else if (skewMode == SkewMode::Length) {
    settings.allowedSkewPs = traceDelayPs(*mUi->edtSkewLength->getValue(),
                                          settings.effectiveDielectricConstant);
  } else {
    settings.allowedSkewPs =
        settings.unitIntervalPs * mUi->spbxSkewPercent->value() / 100;
  }
  return settings;
}

void TraceLengthComparisonDialog::updateTable() noexcept {
  const QLocale locale;
  const TraceLengthComparisonSettings settings = getSettings();
  const QVector<TraceLengthComparisonRow> rows =
      compareTraceLengths(mSegments, settings);

  // Show the resolved inputs, they are what the status column is based on.
  const qreal allowedSkewMm = settings.allowedSkewPs *
      traceVelocityMmPerPs(settings.effectiveDielectricConstant);
  mUi->lblUnitInterval->setText(
      tr("%1 ps, allowed skew %2 ps (%3 mm)")
          .arg(Toolbox::floatToString(settings.unitIntervalPs, 1, locale),
               Toolbox::floatToString(settings.allowedSkewPs, 1, locale),
               Toolbox::floatToString(allowedSkewMm, 3, locale)));

  mUi->tableWidget->setRowCount(rows.count());
  for (int row = 0; row < rows.count(); ++row) {
    const TraceLengthComparisonRow& data = rows.at(row);
    QStringList texts{data.netName};
    if (data.warning == TraceLengthWarning::None) {
      texts.append(Toolbox::floatToString(data.length->toMm(), 6, locale));
      texts.append(Toolbox::floatToString(data.length->toInch(), 6, locale));
      texts.append(Toolbox::floatToString(data.delayPs, 1, locale));
      texts.append(
          Toolbox::floatToString(data.differenceToLongest.toMm(), 6, locale));
      texts.append(
          Toolbox::floatToString(data.differenceToLongestPs, 1, locale));
      texts.append(data.inSpec ? tr("In Spec") : tr("Out of Spec"));
    } else {
      texts.append({QString(), QString(), QString(), QString(), QString(),
                    traceLengthWarningText(data.warning)});
    }
    const bool highlight =
        (data.warning != TraceLengthWarning::None) || (!data.inSpec);
    for (int column = 0; column < texts.count(); ++column) {
      QTableWidgetItem* item = new QTableWidgetItem(texts.at(column));
      if (column > 0) {
        item->setTextAlignment(Qt::AlignRight | Qt::AlignVCenter);
      }
      if (highlight && (column == texts.count() - 1)) {
        item->setForeground(QBrush(Qt::red));
      }
      mUi->tableWidget->setItem(row, column, item);
    }
  }
  mUi->tableWidget->resizeColumnsToContents();
}

/*******************************************************************************
 *  End of File
 ******************************************************************************/

}  // namespace editor
}  // namespace librepcb
