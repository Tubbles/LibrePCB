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
#include "pnsrecordingwindow.h"

#include "pnssessionrecorder.h"

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

PnsRecordingWindow::PnsRecordingWindow(const PnsSessionRecorder& recorder,
                                       QWidget* parent) noexcept
  : QDialog(parent, Qt::Tool), mSessionCountLabel(new QLabel(this)) {
  setAttribute(Qt::WA_DeleteOnClose);
  setWindowTitle(tr("Routing Session Recording"));

  QVBoxLayout* layout = new QVBoxLayout(this);
  layout->addWidget(new QLabel(
      tr("Every push and shove routing session is written to:"), this));

  QLabel* dirLabel = new QLabel(recorder.getDirectory().toNative(), this);
  dirLabel->setTextInteractionFlags(Qt::TextSelectableByMouse);
  dirLabel->setWordWrap(true);
  QFont dirFont = dirLabel->font();
  dirFont.setBold(true);
  dirLabel->setFont(dirFont);
  layout->addWidget(dirLabel);

  layout->addWidget(mSessionCountLabel);
  setSessionCount(recorder.getSessionCount());
  connect(&recorder, &PnsSessionRecorder::sessionCountChanged, this,
          &PnsRecordingWindow::setSessionCount);

  const QString hintText =
      tr("A route which is already being placed when recording starts is not "
         "recorded, the next one is. Stopping while a route is being placed "
         "keeps what was recorded up to that moment.") %
      "\n\n" % tr("Closing this window stops recording.");
  QLabel* hint = new QLabel(hintText, this);
  hint->setWordWrap(true);
  layout->addWidget(hint);

  QPushButton* btnStop = new QPushButton(tr("Stop Recording"), this);
  btnStop->setDefault(true);
  connect(btnStop, &QPushButton::clicked, this, &QDialog::close);
  layout->addWidget(btnStop);

  resize(400, sizeHint().height());
}

PnsRecordingWindow::~PnsRecordingWindow() noexcept {
}

/*******************************************************************************
 *  Private Methods
 ******************************************************************************/

void PnsRecordingWindow::setSessionCount(int count) noexcept {
  mSessionCountLabel->setText(tr("Sessions recorded: %1").arg(count));
}

/*******************************************************************************
 *  End of File
 ******************************************************************************/

}  // namespace editor
}  // namespace librepcb
