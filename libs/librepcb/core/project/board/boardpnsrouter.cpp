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
#include "boardpnsrouter.h"

#include "../../exceptions.h"
#include "../../types/layer.h"
#include "board.h"
#include "items/bi_device.h"
#include "items/bi_pad.h"

#include <librepcb/rust-core/ffi.h>

#include <QtCore>

#include <algorithm>
#include <vector>

/*******************************************************************************
 *  Namespace
 ******************************************************************************/
namespace librepcb {

/*******************************************************************************
 *  Helpers
 ******************************************************************************/

static Point toPoint(const rs::PnsPoint& p) noexcept {
  return Point(Length(p.x), Length(p.y));
}

static rs::PnsPoint toFfi(const Point& p) noexcept {
  return rs::PnsPoint{p.getX().toNm(), p.getY().toNm()};
}

/**
 * Widen a length the router answered into a PositiveLength.
 *
 * The router never places zero width copper, but a degenerate answer must
 * not throw out of a getter, so it is widened to the smallest positive
 * value instead.
 */
static PositiveLength toPositiveLength(qint64 nm) noexcept {
  return PositiveLength(Length(std::max<qint64>(nm, 1)));
}

/**
 * Turn the router's negative-for-none clearance into an optional.
 */
static std::optional<Length> toClearance(qint64 nm) noexcept {
  if (nm < 0) {
    return std::nullopt;
  }
  return Length(nm);
}

static BoardPnsPreviewStyle toStyle(rs::PnsPreviewStyle style) noexcept {
  switch (style) {
    case rs::PnsPreviewStyle::Tail:
      return BoardPnsPreviewStyle::Tail;
    case rs::PnsPreviewStyle::Hover:
      return BoardPnsPreviewStyle::Hover;
    case rs::PnsPreviewStyle::SemiSolid:
      return BoardPnsPreviewStyle::SemiSolid;
    case rs::PnsPreviewStyle::Collision:
      return BoardPnsPreviewStyle::Collision;
    case rs::PnsPreviewStyle::Head:
    default:
      return BoardPnsPreviewStyle::Head;
  }
}

static BoardPnsRouter::StartResult toStartResult(
    rs::PnsStartResult result) noexcept {
  switch (result) {
    case rs::PnsStartResult::AlreadyRouting:
      return BoardPnsRouter::StartResult::AlreadyRouting;
    case rs::PnsStartResult::UnknownStartItem:
      return BoardPnsRouter::StartResult::UnknownStartItem;
    case rs::PnsStartResult::NotRoutable:
      return BoardPnsRouter::StartResult::NotRoutable;
    case rs::PnsStartResult::StartPointViolatesRules:
      return BoardPnsRouter::StartResult::StartPointViolatesRules;
    case rs::PnsStartResult::PlacerRefused:
      return BoardPnsRouter::StartResult::PlacerRefused;
    case rs::PnsStartResult::NothingToDrag:
      return BoardPnsRouter::StartResult::NothingToDrag;
    case rs::PnsStartResult::IncompleteDeviceDrag:
      return BoardPnsRouter::StartResult::IncompleteDeviceDrag;
    case rs::PnsStartResult::NotDraggable:
      return BoardPnsRouter::StartResult::NotDraggable;
    case rs::PnsStartResult::PairNeedsStartItem:
      return BoardPnsRouter::StartResult::PairNeedsStartItem;
    case rs::PnsStartResult::NotADiffPair:
      return BoardPnsRouter::StartResult::NotADiffPair;
    case rs::PnsStartResult::NoDanglingAnchor:
      return BoardPnsRouter::StartResult::NoDanglingAnchor;
    case rs::PnsStartResult::NoCoupledStartItem:
      return BoardPnsRouter::StartResult::NoCoupledStartItem;
    case rs::PnsStartResult::PairGapBelowMinClearance:
      return BoardPnsRouter::StartResult::PairGapBelowMinClearance;
    case rs::PnsStartResult::PairGapMismatch:
      return BoardPnsRouter::StartResult::PairGapMismatch;
    case rs::PnsStartResult::TuningRefused:
      // Nothing here ever starts a length tuning session, so this is
      // unreachable; it keeps the mapping total.
      return BoardPnsRouter::StartResult::PlacerRefused;
    case rs::PnsStartResult::Ok:
    default:
      return BoardPnsRouter::StartResult::Ok;
  }
}

static BoardPnsRouter::FixOutcome toFixOutcome(
    rs::PnsFixOutcome outcome) noexcept {
  switch (outcome) {
    case rs::PnsFixOutcome::Finished:
      return BoardPnsRouter::FixOutcome::Finished;
    case rs::PnsFixOutcome::NotRouting:
      return BoardPnsRouter::FixOutcome::NotRouting;
    case rs::PnsFixOutcome::Continue:
    default:
      return BoardPnsRouter::FixOutcome::Continue;
  }
}

/**
 * Count the pads of a device the snapshot gave a host ID to.
 *
 * A pad with no copper on any copper layer is not synced, so it can never
 * be part of a drag and must not make a whole footprint look incomplete.
 */
static int countSyncedPads(const BoardPnsSnapshot& snapshot,
                           const BI_Device& device) noexcept {
  int count = 0;
  foreach (const BI_Pad* pad, device.getPads()) {
    if (snapshot.getHostId(*pad) != 0) {
      ++count;
    }
  }
  return count;
}

static rs::PnsRouterSettings toFfi(
    const BoardPnsRouter::Settings& settings) noexcept {
  quint8 mode = 2;  // pnsrouter::settings::RouterMode::Walkaround
  if (settings.mode == BoardPnsRouter::Mode::MarkObstacles) {
    mode = 0;
  } else if (settings.mode == BoardPnsRouter::Mode::Shove) {
    mode = 1;
  }
  return rs::PnsRouterSettings{
      mode,
      (*settings.traceWidth).toNm(),
      (*settings.viaDiameter).toNm(),
      (*settings.viaDrill).toNm(),
      settings.shoveIterationLimit,
      settings.allowDrcViolations,
      settings.cornerMode90,
      settings.recordSession,
      (*settings.diffPairWidth).toNm(),
      (*settings.diffPairGap).toNm(),
      // Zero is how the router spells "the via gap follows the trace gap".
      settings.diffPairViaGap ? (**settings.diffPairViaGap).toNm() : 0,
  };
}

/*******************************************************************************
 *  Constructors / Destructor
 ******************************************************************************/

static RustHandle<rs::PnsRouter> construct(
    BoardPnsSnapshot& snapshot, const BoardPnsRouter::Settings& settings) {
  const rs::PnsRouterSettings ffiSettings = toFfi(settings);
  // The session consumes the snapshot, so this must be the last thing that
  // can fail before the handle owns it.
  if (auto obj = rs::ffi_pnsrouter_new(snapshot.release(), &ffiSettings)) {
    return RustHandle<rs::PnsRouter>(*obj, &rs::ffi_pnsrouter_delete);
  } else {
    throw RuntimeError(__FILE__, __LINE__,
                       QString("Failed to create PnsRouter"));
  }
}

BoardPnsRouter::BoardPnsRouter(const Board& board, const Settings& settings)
  : mSnapshot(new BoardPnsSnapshot(board)),  // can throw
    mInnerLayerCount(board.getInnerLayerCount()),
    mHandle(construct(*mSnapshot, settings)),  // can throw
    mPreview(),
    mCommit() {
}

BoardPnsRouter::~BoardPnsRouter() noexcept {
}

/*******************************************************************************
 *  Getters
 ******************************************************************************/

int BoardPnsRouter::getCopperLayerCount() const noexcept {
  return rs::ffi_pnsrouter_copper_layer_count(*mHandle);
}

bool BoardPnsRouter::isRoutingInProgress() const noexcept {
  return rs::ffi_pnsrouter_routing_in_progress(*mHandle);
}

const Layer* BoardPnsRouter::getCurrentLayer() const noexcept {
  return toLayer(rs::ffi_pnsrouter_current_layer(*mHandle));
}

bool BoardPnsRouter::isDragging() const noexcept {
  return rs::ffi_pnsrouter_is_dragging(*mHandle);
}

bool BoardPnsRouter::isPlacingVia() const noexcept {
  return rs::ffi_pnsrouter_placing_via(*mHandle);
}

const QVector<BoardPnsHostRef>& BoardPnsRouter::getHostRefs() const noexcept {
  return mSnapshot->getHostRefs();
}

BoardPnsHostRef BoardPnsRouter::getHostRef(quint64 hostId) const noexcept {
  const QVector<BoardPnsHostRef>& refs = mSnapshot->getHostRefs();
  if (hostId < static_cast<quint64>(refs.count())) {
    return refs.at(static_cast<int>(hostId));
  }
  // The router invented this item and the board has no object for it yet.
  return BoardPnsHostRef();
}

/*******************************************************************************
 *  General Methods
 ******************************************************************************/

QVector<quint64> BoardPnsRouter::hover(const Point& pos,
                                       const Layer* layer) noexcept {
  const int denseLayer = layer
      ? BoardPnsSnapshot::toDenseLayerIndex(*layer, mInnerLayerCount)
      : -1;
  const std::size_t count =
      rs::ffi_pnsrouter_hover(*mHandle, toFfi(pos), denseLayer);

  QVector<quint64> result;
  result.reserve(static_cast<int>(count));
  for (std::size_t i = 0; i < count; ++i) {
    result.append(rs::ffi_pnsrouter_hover_at(*mHandle, i));
  }
  return result;
}

BoardPnsRouter::StartResult BoardPnsRouter::isStartingPointRoutable(
    const Point& pos, quint64 startItem, const Layer& layer) const noexcept {
  const int denseLayer =
      BoardPnsSnapshot::toDenseLayerIndex(layer, mInnerLayerCount);
  return toStartResult(rs::ffi_pnsrouter_is_starting_point_routable(
      *mHandle, toFfi(pos), startItem, denseLayer));
}

BoardPnsRouter::StartResult BoardPnsRouter::startRouting(
    const Point& pos, quint64 startItem, const Layer& layer) noexcept {
  const int denseLayer =
      BoardPnsSnapshot::toDenseLayerIndex(layer, mInnerLayerCount);
  const rs::PnsStartResult result = rs::ffi_pnsrouter_start_routing(
      *mHandle, toFfi(pos), startItem, denseLayer);
  updatePreview();
  return toStartResult(result);
}

BoardPnsRouter::StartResult BoardPnsRouter::isStartingPointRoutableDiffPair(
    const Point& pos, quint64 startItem, const Layer& layer) const noexcept {
  const int denseLayer =
      BoardPnsSnapshot::toDenseLayerIndex(layer, mInnerLayerCount);
  return toStartResult(rs::ffi_pnsrouter_is_starting_point_routable_diff_pair(
      *mHandle, toFfi(pos), startItem, denseLayer));
}

BoardPnsRouter::StartResult BoardPnsRouter::startRoutingDiffPair(
    const Point& pos, quint64 startItem, const Layer& layer) noexcept {
  const int denseLayer =
      BoardPnsSnapshot::toDenseLayerIndex(layer, mInnerLayerCount);
  const rs::PnsStartResult result = rs::ffi_pnsrouter_start_routing_diff_pair(
      *mHandle, toFfi(pos), startItem, denseLayer);
  updatePreview();
  return toStartResult(result);
}

QVector<NetSignal*> BoardPnsRouter::getCurrentNets() const noexcept {
  uint32_t netP = 0;
  uint32_t netN = 0;
  const uint32_t count = rs::ffi_pnsrouter_current_nets(*mHandle, &netP, &netN);

  QVector<NetSignal*> nets;
  if (count > 0) {
    nets.append(toNetSignal(netP));
  }
  if (count > 1) {
    nets.append(toNetSignal(netN));
  }
  return nets;
}

BoardPnsRouter::StartResult BoardPnsRouter::startDragging(
    const Point& pos, const QVector<quint64>& items, bool freeAngle) noexcept {
  const StartResult check = checkDraggableItems(items);
  if (check != StartResult::Ok) {
    return check;
  }

  // The router takes the IDs as a pointer and a count. They are copied
  // rather than handed over in place because quint64 and uint64_t are two
  // distinct 64 bit types on this platform, which converts value by value
  // and not pointer by pointer. An empty vector may answer a null pointer,
  // which the router side reads as the empty set and refuses.
  std::vector<uint64_t> ids;
  ids.reserve(static_cast<std::size_t>(items.count()));
  foreach (const quint64 hostId, items) {
    ids.push_back(hostId);
  }
  const rs::PnsStartResult result = rs::ffi_pnsrouter_start_dragging(
      *mHandle, toFfi(pos), ids.data(), ids.size(), freeAngle);
  updatePreview();
  return toStartResult(result);
}

BoardPnsRouter::StartResult BoardPnsRouter::startDragging(
    const Point& pos, quint64 hostId, bool freeAngle) noexcept {
  return startDragging(pos, QVector<quint64>{hostId}, freeAngle);
}

void BoardPnsRouter::moveTo(const Point& pos, quint64 endItem) noexcept {
  rs::ffi_pnsrouter_move_to(*mHandle, toFfi(pos), endItem);
  updatePreview();
}

BoardPnsRouter::FixOutcome BoardPnsRouter::fixRoute(
    const Point& pos, quint64 endItem, bool forceFinish) noexcept {
  const rs::PnsFixOutcome outcome =
      rs::ffi_pnsrouter_fix_route(*mHandle, toFfi(pos), endItem, forceFinish);
  updatePreview();
  if (outcome == rs::PnsFixOutcome::Finished) {
    updateCommit();
  }
  return toFixOutcome(outcome);
}

BoardPnsRouter::FixOutcome BoardPnsRouter::finish() noexcept {
  const rs::PnsFixOutcome outcome = rs::ffi_pnsrouter_finish(*mHandle);
  updatePreview();
  if (outcome == rs::PnsFixOutcome::Finished) {
    updateCommit();
  }
  return toFixOutcome(outcome);
}

std::optional<Point> BoardPnsRouter::undoLastSegment() noexcept {
  rs::PnsPoint at = {};
  if (!rs::ffi_pnsrouter_undo_last_segment(*mHandle, &at)) {
    return std::nullopt;
  }
  return toPoint(at);
}

bool BoardPnsRouter::switchLayer(const Layer& layer) noexcept {
  const int denseLayer =
      BoardPnsSnapshot::toDenseLayerIndex(layer, mInnerLayerCount);
  if (denseLayer < 0) {
    return false;  // Not a copper layer of this board.
  }
  return rs::ffi_pnsrouter_switch_layer(*mHandle, denseLayer);
}

bool BoardPnsRouter::toggleViaPlacement() noexcept {
  return rs::ffi_pnsrouter_toggle_via_placement(*mHandle);
}

void BoardPnsRouter::flipPosture() noexcept {
  rs::ffi_pnsrouter_flip_posture(*mHandle);
}

BoardPnsCommit BoardPnsRouter::stopRouting() noexcept {
  rs::ffi_pnsrouter_stop_routing(*mHandle);
  updatePreview();
  updateCommit();
  return mCommit;
}

void BoardPnsRouter::abortRouting() noexcept {
  rs::ffi_pnsrouter_abort_routing(*mHandle);
  updatePreview();
  updateCommit();
}

void BoardPnsRouter::setSettings(const Settings& settings) noexcept {
  const rs::PnsRouterSettings ffiSettings = toFfi(settings);
  rs::ffi_pnsrouter_set_settings(*mHandle, &ffiSettings);
}

QString BoardPnsRouter::takeRecording() noexcept {
  QString text;
  if (!rs::ffi_pnsrouter_take_recording(*mHandle, &text)) {
    return QString();
  }
  return text;
}

/*******************************************************************************
 *  Private Methods
 ******************************************************************************/

void BoardPnsRouter::updatePreview() noexcept {
  mPreview = BoardPnsPreview();

  const std::size_t itemCount = rs::ffi_pnsrouter_preview_item_count(*mHandle);
  mPreview.items.reserve(static_cast<int>(itemCount));
  for (std::size_t i = 0; i < itemCount; ++i) {
    rs::PnsPreviewItem raw = {};
    rs::ffi_pnsrouter_preview_item(*mHandle, i, &raw);

    BoardPnsPreviewItem item;
    item.path.reserve(static_cast<int>(raw.point_count));
    for (std::size_t p = 0; p < raw.point_count; ++p) {
      item.path.append(
          toPoint(rs::ffi_pnsrouter_preview_item_point(*mHandle, i, p)));
    }
    item.width = toPositiveLength(raw.width);
    item.layer = toLayer(raw.layer);
    item.net = toNetSignal(raw.net);
    item.style = toStyle(raw.style);
    item.clearance = toClearance(raw.clearance);
    mPreview.items.append(item);
  }

  if (rs::ffi_pnsrouter_preview_has_via(*mHandle)) {
    rs::PnsPreviewVia raw = {};
    rs::ffi_pnsrouter_preview_via(*mHandle, &raw);
    mPreview.via = toPreviewVia(raw);
  }

  if (rs::ffi_pnsrouter_preview_has_via_n(*mHandle)) {
    rs::PnsPreviewVia raw = {};
    rs::ffi_pnsrouter_preview_via_n(*mHandle, &raw);
    mPreview.viaN = toPreviewVia(raw);
  }

  const std::size_t viaCount =
      rs::ffi_pnsrouter_preview_fixed_via_count(*mHandle);
  mPreview.fixedVias.reserve(static_cast<int>(viaCount));
  for (std::size_t i = 0; i < viaCount; ++i) {
    rs::PnsPreviewVia raw = {};
    rs::ffi_pnsrouter_preview_fixed_via(*mHandle, i, &raw);
    mPreview.fixedVias.append(toPreviewVia(raw));
  }

  const std::size_t ratlineCount =
      rs::ffi_pnsrouter_preview_ratline_point_count(*mHandle);
  mPreview.ratline.reserve(static_cast<int>(ratlineCount));
  for (std::size_t i = 0; i < ratlineCount; ++i) {
    mPreview.ratline.append(
        toPoint(rs::ffi_pnsrouter_preview_ratline_point(*mHandle, i)));
  }

  const std::size_t ratlineNCount =
      rs::ffi_pnsrouter_preview_ratline_n_point_count(*mHandle);
  mPreview.ratlineN.reserve(static_cast<int>(ratlineNCount));
  for (std::size_t i = 0; i < ratlineNCount; ++i) {
    mPreview.ratlineN.append(
        toPoint(rs::ffi_pnsrouter_preview_ratline_n_point(*mHandle, i)));
  }

  const std::size_t violationCount =
      rs::ffi_pnsrouter_preview_violation_count(*mHandle);
  mPreview.violations.reserve(static_cast<int>(violationCount));
  for (std::size_t i = 0; i < violationCount; ++i) {
    rs::PnsViolationMarker raw = {};
    rs::ffi_pnsrouter_preview_violation(*mHandle, i, &raw);

    BoardPnsViolation violation;
    violation.host = getHostRef(raw.host_id);
    violation.clearance = Length(raw.clearance);
    violation.forcedLayer = toLayer(raw.forced_layer);
    violation.hideOriginal = raw.hide_original;
    mPreview.violations.append(violation);
  }

  const std::size_t hiddenCount =
      rs::ffi_pnsrouter_preview_hidden_count(*mHandle);
  mPreview.hidden.reserve(static_cast<int>(hiddenCount));
  for (std::size_t i = 0; i < hiddenCount; ++i) {
    mPreview.hidden.append(
        getHostRef(rs::ffi_pnsrouter_preview_hidden_at(*mHandle, i)));
  }

  const std::size_t movedCount =
      rs::ffi_pnsrouter_preview_moved_solid_count(*mHandle);
  for (std::size_t i = 0; i < movedCount; ++i) {
    uint64_t hostId = 0;
    rs::PnsPoint offset = {};
    rs::ffi_pnsrouter_preview_moved_solid_at(*mHandle, i, &hostId, &offset);
    appendMovedDevice(mPreview.movedDevices, getHostRef(hostId),
                      toPoint(offset));
  }
}

void BoardPnsRouter::updateCommit() noexcept {
  mCommit = BoardPnsCommit();

  const std::size_t removedCount =
      rs::ffi_pnsrouter_commit_removed_count(*mHandle);
  mCommit.removed.reserve(static_cast<int>(removedCount));
  for (std::size_t i = 0; i < removedCount; ++i) {
    mCommit.removed.append(
        getHostRef(rs::ffi_pnsrouter_commit_removed_at(*mHandle, i)));
  }

  const std::size_t addedCount = rs::ffi_pnsrouter_commit_added_count(*mHandle);
  mCommit.added.reserve(static_cast<int>(addedCount));
  for (std::size_t i = 0; i < addedCount; ++i) {
    rs::PnsNewItem raw = {};
    rs::ffi_pnsrouter_commit_added_at(*mHandle, i, &raw);
    mCommit.added.append(toNewItem(raw));
  }

  const std::size_t updatedCount =
      rs::ffi_pnsrouter_commit_updated_count(*mHandle);
  mCommit.updated.reserve(static_cast<int>(updatedCount));
  for (std::size_t i = 0; i < updatedCount; ++i) {
    uint64_t hostId = 0;
    rs::PnsNewItem raw = {};
    rs::ffi_pnsrouter_commit_updated_at(*mHandle, i, &hostId, &raw);
    mCommit.updated.append(qMakePair(getHostRef(hostId), toNewItem(raw)));
  }

  const std::size_t movedCount =
      rs::ffi_pnsrouter_commit_moved_solid_count(*mHandle);
  for (std::size_t i = 0; i < movedCount; ++i) {
    uint64_t hostId = 0;
    rs::PnsPoint offset = {};
    rs::ffi_pnsrouter_commit_moved_solid_at(*mHandle, i, &hostId, &offset);
    appendMovedDevice(mCommit.movedDevices, getHostRef(hostId),
                      toPoint(offset));
  }
}

BoardPnsRouter::StartResult BoardPnsRouter::checkDraggableItems(
    const QVector<quint64>& items) const noexcept {
  // The router moves pads, the host moves the devices which own them, so a
  // set of pads is only a drag this host can apply if it holds every pad
  // of every device it names and nothing else. Holes, copper graphics and
  // keepout zones are obstacles the router never moves at all.
  QHash<BI_Device*, QSet<const BI_Pad*>> padsByDevice;
  int others = 0;
  foreach (const quint64 hostId, items) {
    if (hostId == 0) {
      continue;  // Dropped on the router side too.
    }
    const BoardPnsHostRef ref = getHostRef(hostId);
    if (ref.hole || ref.polygon || ref.zone) {
      return StartResult::NotDraggable;
    } else if (ref.pad) {
      BI_Device* device = ref.pad->getDevice();
      if (!device) {
        return StartResult::NotDraggable;  // A board pad moves with nothing.
      }
      padsByDevice[device].insert(ref.pad);
    } else {
      ++others;
    }
  }

  if (padsByDevice.isEmpty()) {
    return StartResult::Ok;  // A trace or via drag, which needs no gate.
  } else if (others > 0) {
    return StartResult::IncompleteDeviceDrag;  // A pad among traces.
  }
  for (auto it = padsByDevice.begin(); it != padsByDevice.end(); ++it) {
    if (it.value().count() != countSyncedPads(*mSnapshot, *it.key())) {
      return StartResult::IncompleteDeviceDrag;
    }
  }
  return StartResult::Ok;
}

void BoardPnsRouter::appendMovedDevice(QVector<BoardPnsMovedDevice>& devices,
                                       const BoardPnsHostRef& ref,
                                       const Point& offset) noexcept {
  BI_Device* device = ref.pad ? ref.pad->getDevice() : nullptr;
  if (!device) {
    return;  // Not a pad of a device, so there is nothing to move.
  }

  for (BoardPnsMovedDevice& moved : devices) {
    if (moved.device == device) {
      // Every pad of one device is moved by the same vector, so a second
      // answer which disagrees is a router bug rather than a second move.
      // The first one is kept, because dropping the drag is worse.
      if (moved.offset != offset) {
        qWarning() << "The router moved the pads of one device by different "
                      "offsets:"
                   << moved.offset << "and" << offset;
      }
      return;
    }
  }
  devices.append(BoardPnsMovedDevice{device, offset});
}

BoardPnsPreviewVia BoardPnsRouter::toPreviewVia(
    const rs::PnsPreviewVia& via) const noexcept {
  BoardPnsPreviewVia result;
  result.position = toPoint(via.pos);
  result.diameter = toPositiveLength(via.diameter);
  result.drill = toPositiveLength(via.drill);
  result.startLayer = toLayer(via.layer_start);
  result.endLayer = toLayer(via.layer_end);
  result.net = toNetSignal(via.net);
  result.style = toStyle(via.style);
  result.clearance = toClearance(via.clearance);
  return result;
}

BoardPnsNewItem BoardPnsRouter::toNewItem(
    const rs::PnsNewItem& item) const noexcept {
  BoardPnsNewItem result;
  result.net = toNetSignal(item.net);
  result.source = getHostRef(item.source);
  if (item.kind == rs::PnsNewGeometryKind::Via) {
    BoardPnsNewVia via;
    via.position = toPoint(item.pos);
    via.diameter = toPositiveLength(item.diameter);
    via.drill = toPositiveLength(item.drill);
    via.startLayer = toLayer(item.layer_start);
    via.endLayer = toLayer(item.layer_end);
    result.geometry = via;
  } else {
    BoardPnsNewSegment segment;
    segment.start = toPoint(item.p1);
    segment.end = toPoint(item.p2);
    segment.width = toPositiveLength(item.width);
    // A trace occupies exactly one layer, so both ends of the router's
    // layer range are the same.
    segment.layer = toLayer(item.layer_start);
    result.geometry = segment;
  }
  return result;
}

const Layer* BoardPnsRouter::toLayer(int denseIndex) const noexcept {
  return BoardPnsSnapshot::fromDenseLayerIndex(denseIndex, mInnerLayerCount);
}

NetSignal* BoardPnsRouter::toNetSignal(quint32 netNumber) const noexcept {
  return mSnapshot->getNetSignal(netNumber);
}

/*******************************************************************************
 *  End of File
 ******************************************************************************/

}  // namespace librepcb
