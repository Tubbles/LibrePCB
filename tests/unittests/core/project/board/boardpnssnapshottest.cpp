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
#include <gtest/gtest.h>
#include <librepcb/core/fileio/transactionalfilesystem.h>
#include <librepcb/core/geometry/path.h>
#include <librepcb/core/geometry/zone.h>
#include <librepcb/core/project/board/board.h>
#include <librepcb/core/project/board/boardpnssnapshot.h>
#include <librepcb/core/project/board/boardzonedata.h>
#include <librepcb/core/project/board/drc/boarddesignrulecheckdata.h>
#include <librepcb/core/project/board/items/bi_device.h>
#include <librepcb/core/project/board/items/bi_hole.h>
#include <librepcb/core/project/board/items/bi_netline.h>
#include <librepcb/core/project/board/items/bi_netsegment.h>
#include <librepcb/core/project/board/items/bi_pad.h>
#include <librepcb/core/project/board/items/bi_via.h>
#include <librepcb/core/project/board/items/bi_zone.h>
#include <librepcb/core/project/circuit/circuit.h>
#include <librepcb/core/project/project.h>
#include <librepcb/core/project/projectloader.h>
#include <librepcb/core/types/layer.h>
#include <librepcb/core/types/uuid.h>
#include <librepcb/core/utils/rusthandle.h>
#include <librepcb/rust-core/ffi.h>

#include <QtCore>

#include <algorithm>
#include <memory>

/*******************************************************************************
 *  Namespace
 ******************************************************************************/
namespace librepcb {
namespace tests {

/*******************************************************************************
 *  Helpers
 ******************************************************************************/

static RustHandle<rs::PnsSnapshot> makeSnapshot(int copperLayerCount) {
  rs::PnsSnapshot* obj =
      rs::ffi_pnsrouter_snapshot_new(static_cast<uint8_t>(copperLayerCount));
  EXPECT_NE(obj, nullptr);
  return RustHandle<rs::PnsSnapshot>(*obj, &rs::ffi_pnsrouter_snapshot_delete);
}

static rs::PnsItemHeader makeHeader(quint64 hostId, quint32 net, int layerStart,
                                    int layerEnd) noexcept {
  rs::PnsItemHeader header = {};
  header.host_id = hostId;
  header.net = net;
  header.layer_start = layerStart;
  header.layer_end = layerEnd;
  header.routable = true;
  header.copper_clearance = -1;
  return header;
}

static rs::PnsShape circle(qint64 x, qint64 y, qint64 radius) noexcept {
  rs::PnsShape shape = {};
  shape.kind = rs::PnsShapeKind::Circle;
  shape.center = rs::PnsPoint{x, y};
  shape.radius = radius;
  return shape;
}

/**
 * @brief Add a round pad on the top layer
 */
static void addPad(rs::PnsSnapshot* snapshot, quint64 hostId, quint32 net,
                   qint64 x, qint64 copperClearance, bool boardEdge = false) {
  rs::PnsItemHeader header = makeHeader(hostId, net, 0, 0);
  header.copper_clearance = copperClearance;
  header.board_edge = boardEdge;
  header.routable = !boardEdge;

  rs::PnsSolidGeometry geometry = {};
  geometry.shape = circle(x, 0, 500000);
  geometry.pos = rs::PnsPoint{x, 0};
  geometry.has_hole = false;
  ASSERT_EQ(rs::ffi_pnsrouter_snapshot_add_solid(snapshot, &header, &geometry),
            rs::PnsResult::Ok);
}

/**
 * @brief Add a bare drill on every copper layer
 */
static void addHole(rs::PnsSnapshot* snapshot, quint64 hostId, quint32 net,
                    qint64 x, int copperLayerCount) {
  rs::PnsItemHeader header = makeHeader(hostId, net, 0, copperLayerCount - 1);
  header.routable = false;

  const rs::PnsHoleGeometry geometry{circle(x, 0, 200000)};
  ASSERT_EQ(rs::ffi_pnsrouter_snapshot_add_hole(snapshot, &header, &geometry),
            rs::PnsResult::Ok);
}

/**
 * @brief Add a zone to a board, which takes ownership of it
 *
 * ::librepcb::Board::~Board() deletes every zone it holds, so the caller must
 * not.
 */
static const BI_Zone* addZone(Board& board, const Path& outline,
                              const QSet<const Layer*>& layers,
                              Zone::Rules rules) {
  BI_Zone* zone = new BI_Zone(
      board,
      BoardZoneData(Uuid::createRandom(), layers, rules, outline, false));
  board.addZone(*zone);
  return zone;
}

static const BI_Zone* addKeepoutZone(Board& board, const Path& outline,
                                     const QSet<const Layer*>& layers) {
  return addZone(board, outline, layers, Zone::Rules(Zone::Rule::NoCopper));
}

/**
 * @brief A five millimetre square at the origin, four corners
 */
static Path squareOutline() noexcept {
  return Path::rect(Point(0, 0),
                    Point(Length(5000000), Length(5000000)));  // 5 mm.
}

/**
 * @brief An L shaped outline at the origin, six corners, one of them reflex
 */
static Path lShapedOutline() noexcept {
  const Length outer(10000000);  // 10 mm.
  const Length inner(5000000);  // 5 mm.
  return Path({
                  Vertex(Point(Length(0), Length(0))),
                  Vertex(Point(outer, Length(0))),
                  Vertex(Point(outer, inner)),
                  Vertex(Point(inner, inner)),
                  Vertex(Point(inner, outer)),
                  Vertex(Point(Length(0), outer)),
              })
      .toClosedPath();
}

/**
 * @brief Get the host ID a zone was given, or 0 if it is not in the snapshot
 */
static quint64 findZoneHostId(const BoardPnsSnapshot& snapshot,
                              const BI_Zone* zone) noexcept {
  const QVector<BoardPnsHostRef>& refs = snapshot.getHostRefs();
  for (int i = 0; i < refs.count(); ++i) {
    if (refs.at(i).zone == zone) {
      return static_cast<quint64>(i);
    }
  }
  return 0;
}

static std::unique_ptr<Project> openGerberTestProject() {
  const FilePath fp(TEST_DATA_DIR "/projects/Gerber Test/project.lpp");
  std::shared_ptr<TransactionalFileSystem> fs =
      TransactionalFileSystem::openRO(fp.getParentDir());
  ProjectLoader loader;
  return loader.open(std::make_unique<TransactionalDirectory>(fs),
                     fp.getFilename());  // can throw
}

/*******************************************************************************
 *  Test Class
 ******************************************************************************/

class BoardPnsSnapshotTest : public ::testing::Test {};

/*******************************************************************************
 *  Step 1: the FFI handles
 ******************************************************************************/

TEST_F(BoardPnsSnapshotTest, testSnapshotHandleLifetime) {
  RustHandle<rs::PnsSnapshot> handle = makeSnapshot(2);

  rs::PnsSnapshotStats stats = {};
  rs::ffi_pnsrouter_snapshot_stats(*handle, &stats);
  EXPECT_EQ(stats.copper_layer_count, 2);
  EXPECT_EQ(stats.item_count, 0U);
}

TEST_F(BoardPnsSnapshotTest, testRouterHandleLifetime) {
  RustHandle<rs::PnsSnapshot> snapshot = makeSnapshot(4);

  const rs::PnsRouterSettings settings{
      2,  // Walkaround.
      250000,  // 0.25 mm trace.
      700000,  // 0.7 mm via.
      300000,  // 0.3 mm via drill.
      250,  // Shove iteration limit.
      false,  // Allow DRC violations.
      false,  // 90 degree corners.
      false,  // Record the session.
  };
  rs::PnsRouter* obj = rs::ffi_pnsrouter_new(snapshot.mObj, &settings);
  ASSERT_NE(obj, nullptr);
  snapshot.mObj = nullptr;  // Consumed by ffi_pnsrouter_new().

  RustHandle<rs::PnsRouter> router(*obj, &rs::ffi_pnsrouter_delete);
  EXPECT_EQ(rs::ffi_pnsrouter_copper_layer_count(*router), 4);
  EXPECT_FALSE(rs::ffi_pnsrouter_routing_in_progress(*router));
}

/*******************************************************************************
 *  Step 2: the layer mapping and the board snapshot
 ******************************************************************************/

TEST_F(BoardPnsSnapshotTest, testLayerMappingRoundTrip) {
  // Two, four and eight copper layers.
  for (int innerLayerCount : {0, 2, 6}) {
    const int copperLayerCount = innerLayerCount + 2;

    for (int index = 0; index < copperLayerCount; ++index) {
      const Layer* layer =
          BoardPnsSnapshot::fromDenseLayerIndex(index, innerLayerCount);
      ASSERT_NE(layer, nullptr) << "index=" << index;
      EXPECT_TRUE(layer->isCopper());
      EXPECT_EQ(BoardPnsSnapshot::toDenseLayerIndex(*layer, innerLayerCount),
                index);
    }

    // The bottom layer is always the last index, whatever its sparse
    // copper number is.
    EXPECT_EQ(BoardPnsSnapshot::toDenseLayerIndex(Layer::botCopper(),
                                                  innerLayerCount),
              copperLayerCount - 1);
    EXPECT_EQ(BoardPnsSnapshot::toDenseLayerIndex(Layer::topCopper(),
                                                  innerLayerCount),
              0);
    EXPECT_EQ(Layer::botCopper().getCopperNumber(),
              Layer::innerCopperCount() + 1);

    // Out of range on both ends.
    EXPECT_EQ(BoardPnsSnapshot::fromDenseLayerIndex(-1, innerLayerCount),
              nullptr);
    EXPECT_EQ(BoardPnsSnapshot::fromDenseLayerIndex(copperLayerCount,
                                                    innerLayerCount),
              nullptr);
    EXPECT_EQ(BoardPnsSnapshot::toDenseLayerIndex(Layer::topLegend(),
                                                  innerLayerCount),
              -1);
    if (innerLayerCount < Layer::innerCopperCount()) {
      EXPECT_EQ(BoardPnsSnapshot::toDenseLayerIndex(
                    *Layer::innerCopper(innerLayerCount + 1), innerLayerCount),
                -1);
    }
  }
}

TEST_F(BoardPnsSnapshotTest, testSnapshotOfBoard) {
  std::unique_ptr<Project> project = openGerberTestProject();
  ASSERT_FALSE(project->getBoards().isEmpty());
  Board& board = *project->getBoards().first();

  BoardPnsSnapshot snapshot(board);
  EXPECT_EQ(snapshot.getCopperLayerCount(), board.getInnerLayerCount() + 2);

  rs::PnsSnapshotStats stats = {};
  rs::ffi_pnsrouter_snapshot_stats(*snapshot, &stats);

  // Every trace and every via of the board must be there, one for one.
  int traces = 0;
  int vias = 0;
  foreach (const BI_NetSegment* segment, board.getNetSegments()) {
    traces += segment->getNetLines().count();
    vias += segment->getVias().count();
  }
  EXPECT_GT(traces, 0);
  EXPECT_GT(vias, 0);
  EXPECT_EQ(stats.segment_count, static_cast<std::size_t>(traces));
  EXPECT_EQ(stats.via_count, static_cast<std::size_t>(vias));

  // Every board level drill becomes a bare hole, and so does every drill
  // beyond the first of a pad, because one solid can carry only one.
  EXPECT_GE(stats.hole_count,
            static_cast<std::size_t>(board.getHoles().count()));
  foreach (const BI_Hole* hole, board.getHoles()) {
    const quint64 id = snapshot.getHostId(*hole);
    ASSERT_GT(id, 0U);
    EXPECT_EQ(snapshot.getHostRefs().at(id).hole, hole);
  }

  // Pads, copper polygons and the board outline are all solids, and every
  // pad contributes at least one.
  EXPECT_GT(stats.solid_count, 0U);
  EXPECT_EQ(stats.item_count,
            stats.segment_count + stats.via_count + stats.solid_count +
                stats.hole_count);

  // Exactly one solid per drilled pad carries the drill, never several,
  // because coincident drills would collide with each other.
  EXPECT_GT(stats.drilled_solid_count, 0U);
  EXPECT_LE(stats.drilled_solid_count, stats.solid_count);

  // Every net of the circuit is interned, and every net class.
  EXPECT_EQ(
      stats.net_count,
      static_cast<std::size_t>(project->getCircuit().getNetSignals().count()));
  EXPECT_EQ(
      stats.net_class_count,
      static_cast<std::size_t>(project->getCircuit().getNetClasses().count()));

  // The broad phase inflation radius must dominate every rule.
  EXPECT_GE(stats.max_clearance,
            (*board.getDrcSettings().getMinCopperCopperClearance()).toNm());
  EXPECT_GE(stats.max_clearance,
            (*board.getDrcSettings().getMinCopperBoardClearance()).toNm());

  // The host ID table has one entry per synced board object plus the null
  // entry at index 0.
  EXPECT_GT(snapshot.getHostRefs().count(), 1);
  foreach (const BI_NetSegment* segment, board.getNetSegments()) {
    foreach (const BI_NetLine* netLine, segment->getNetLines()) {
      const quint64 id = snapshot.getHostId(*netLine);
      ASSERT_GT(id, 0U);
      EXPECT_EQ(snapshot.getHostRefs().at(id).netLine, netLine);
    }
    foreach (const BI_Via* via, segment->getVias()) {
      const quint64 id = snapshot.getHostId(*via);
      ASSERT_GT(id, 0U);
      EXPECT_EQ(snapshot.getHostRefs().at(id).via, via);
    }
  }
}

/*******************************************************************************
 *  Step 3: the rule resolver
 ******************************************************************************/

/**
 * @brief Compare the resolver against BoardDesignRuleCheckData's helpers
 *
 * The helpers are the reference implementation of LibrePCB's `std::max`
 * combination of a net class value with the board setting. The router must
 * produce the same answers or it will produce boards that fail the DRC.
 */
TEST_F(BoardPnsSnapshotTest, testRuleResolverMatchesDrcHelpers) {
  std::unique_ptr<Project> project = openGerberTestProject();
  ASSERT_FALSE(project->getBoards().isEmpty());
  Board& board = *project->getBoards().first();
  BoardDesignRuleCheckData data(board, board.getDrcSettings(), true);

  const Uuid netClassUuid = Uuid::createRandom();
  const QVector<qint64> values = {0, 100000, 200000, 500000};

  for (qint64 boardClearance : values) {
    for (qint64 netClassClearance : values) {
      for (qint64 padOverride : values) {
        // The reference: BoardDesignRuleCheckData's own helpers.
        BoardDesignRuleCheckSettings settings;
        settings.setMinCopperCopperClearance(
            UnsignedLength(Length(boardClearance)));
        settings.setMinCopperBoardClearance(UnsignedLength(Length(300000)));
        settings.setMinCopperNpthClearance(UnsignedLength(Length(150000)));
        settings.setMinDrillDrillClearance(UnsignedLength(Length(350000)));
        settings.setMinDrillBoardClearance(UnsignedLength(Length(400000)));
        settings.setMinCopperWidth(UnsignedLength(Length(boardClearance)));
        settings.setMinPthDrillDiameter(UnsignedLength(Length(boardClearance)));
        data.settings = settings;
        data.netClasses.clear();
        data.netClasses.insert(netClassUuid,
                               BoardDesignRuleCheckData::NetClass{
                                   UnsignedLength(Length(netClassClearance)),
                                   UnsignedLength(Length(netClassClearance)),
                                   UnsignedLength(Length(netClassClearance)),
                               });

        // The same values across the FFI.
        RustHandle<rs::PnsSnapshot> snapshot = makeSnapshot(2);
        const rs::PnsBoardRules boardRules{
            boardClearance, 300000,         150000, 350000, 400000,
            boardClearance, boardClearance, 250000, 300000,
        };
        ASSERT_EQ(
            rs::ffi_pnsrouter_snapshot_set_board_rules(*snapshot, &boardRules),
            rs::PnsResult::Ok);
        const rs::PnsNetClassRules netClassRules{
            netClassClearance, netClassClearance, netClassClearance, 0, 0,
        };
        ASSERT_EQ(
            rs::ffi_pnsrouter_snapshot_add_net_class(*snapshot, &netClassRules),
            0U);
        const quint32 netA = rs::ffi_pnsrouter_snapshot_add_net(*snapshot, 0);
        const quint32 netB = rs::ffi_pnsrouter_snapshot_add_net(*snapshot, 0);
        ASSERT_EQ(netA, 1U);
        ASSERT_EQ(netB, 2U);

        // Host 1: a pad with the override. Host 2: a pad without it.
        // Host 3: a pad on the same net as host 1. Host 4 and 5: drills.
        // Host 6: a board edge.
        addPad(*snapshot, 1, netA, 0, padOverride);
        addPad(*snapshot, 2, netB, 5000000, -1);
        addPad(*snapshot, 3, netA, 10000000, -1);
        addHole(*snapshot, 4, 0, 15000000, 2);
        addHole(*snapshot, 5, 0, 20000000, 2);
        addPad(*snapshot, 6, 0, 25000000, -1, true);

        const qint32 maxClearance =
            rs::ffi_pnsrouter_snapshot_max_clearance(*snapshot);

        auto clearance = [&snapshot](quint64 a, quint64 b) {
          qint32 value = -1;
          EXPECT_EQ(rs::ffi_pnsrouter_snapshot_clearance(
                        *snapshot, a, rs::PnsItemRole::Copper, b,
                        rs::PnsItemRole::Copper, &value),
                    rs::PnsResult::Ok);
          EXPECT_LE(value, rs::ffi_pnsrouter_snapshot_max_clearance(*snapshot));
          return value;
        };

        // Copper to copper on different nets: the maximum of the board
        // setting, the net class setting and the pad override, which is
        // the DRC helper plus the override.
        const qint64 expected = std::max<qint64>(
            (*data.getMinCopperCopperClearance(netClassUuid)).toNm(),
            padOverride);
        EXPECT_EQ(clearance(1, 2), expected)
            << "board=" << boardClearance << " netclass=" << netClassClearance
            << " pad=" << padOverride;

        // The override belongs to host 1 only, so a pair without it falls
        // back to the DRC helper's answer.
        EXPECT_EQ(clearance(2, 3),
                  (*data.getMinCopperCopperClearance(netClassUuid)).toNm());

        // Same net, no rule applies at all.
        qint32 unused = -1;
        EXPECT_EQ(rs::ffi_pnsrouter_snapshot_clearance(
                      *snapshot, 1, rs::PnsItemRole::Copper, 3,
                      rs::PnsItemRole::Copper, &unused),
                  rs::PnsResult::NoClearance);

        // Hole to hole takes the drill to drill rule, whatever the nets.
        EXPECT_EQ(clearance(4, 5), 350000);
        // Hole to copper takes the copper to NPTH rule.
        EXPECT_EQ(clearance(1, 4), 150000);
        // A board edge adds the copper to board rule, and the drill to
        // board rule when the other side is a drill.
        EXPECT_GE(clearance(2, 6), 300000);
        EXPECT_EQ(clearance(4, 6), 400000);

        // No answer may exceed the broad phase inflation radius.
        EXPECT_GE(maxClearance, expected);
        EXPECT_GE(maxClearance, 400000);

        // Width and via drill go through the constraint path.
        qint32 min = -1;
        qint32 opt = -1;
        EXPECT_EQ(rs::ffi_pnsrouter_snapshot_constraint(
                      *snapshot, rs::PnsConstraintKind::Width, 1, &min, &opt),
                  rs::PnsResult::Ok);
        EXPECT_EQ(min, (*data.getMinCopperWidth(netClassUuid)).toNm());
        EXPECT_EQ(opt, 250000);

        EXPECT_EQ(rs::ffi_pnsrouter_snapshot_constraint(
                      *snapshot, rs::PnsConstraintKind::ViaHole, 1, &min, &opt),
                  rs::PnsResult::Ok);
        EXPECT_EQ(min, (*data.getMinViaDrillDiameter(netClassUuid)).toNm());
        EXPECT_EQ(opt, 300000);
      }
    }
  }
}

TEST_F(BoardPnsSnapshotTest, testCoordinateOutOfRangeIsAnError) {
  RustHandle<rs::PnsSnapshot> snapshot = makeSnapshot(2);

  const rs::PnsItemHeader header = makeHeader(1, 0, 0, 0);
  const rs::PnsSegmentGeometry geometry{
      rs::PnsPoint{0, 0},
      rs::PnsPoint{3000000000LL, 0},  // 3 metres.
      250000,
  };
  EXPECT_EQ(
      rs::ffi_pnsrouter_snapshot_add_segment(*snapshot, &header, &geometry),
      rs::PnsResult::CoordinateOutOfRange);
}

/*******************************************************************************
 *  Keepout zones
 ******************************************************************************/

TEST_F(BoardPnsSnapshotTest, testKeepoutZoneBecomesOneObstaclePerTriangle) {
  std::unique_ptr<Project> project = openGerberTestProject();
  ASSERT_FALSE(project->getBoards().isEmpty());
  Board& board = *project->getBoards().first();

  // The fixture carries two keepout zones of its own, a four corner
  // diamond on each outer layer, which are two triangles each.
  BoardPnsSnapshot before(board);
  rs::PnsSnapshotStats without = {};
  rs::ffi_pnsrouter_snapshot_stats(*before, &without);
  EXPECT_EQ(without.keepout_count, 4U);

  const BI_Zone* zone =
      addKeepoutZone(board, squareOutline(), {&Layer::topCopper()});

  BoardPnsSnapshot snapshot(board);
  rs::PnsSnapshotStats stats = {};
  rs::ffi_pnsrouter_snapshot_stats(*snapshot, &stats);

  // A rectangle is two triangles, on the one layer the zone names.
  EXPECT_EQ(stats.keepout_count, without.keepout_count + 2);
  EXPECT_EQ(stats.solid_count, without.solid_count + 2);
  EXPECT_EQ(stats.item_count, without.item_count + 2);

  // Both of them are one host object, so a router answer names the zone and
  // not one of its triangles.
  EXPECT_EQ(snapshot.getHostRefs().count(), before.getHostRefs().count() + 1);
  const quint64 hostId = findZoneHostId(snapshot, zone);
  ASSERT_GT(hostId, 0U);
  const BoardPnsHostRef& ref = snapshot.getHostRefs().at(hostId);
  EXPECT_EQ(ref.zone, zone);
  EXPECT_EQ(ref.netLine, nullptr);
  EXPECT_EQ(ref.via, nullptr);
  EXPECT_EQ(ref.pad, nullptr);
  EXPECT_EQ(ref.hole, nullptr);
  EXPECT_EQ(ref.polygon, nullptr);
}

TEST_F(BoardPnsSnapshotTest, testKeepoutZoneCoversEveryLayerItNames) {
  std::unique_ptr<Project> project = openGerberTestProject();
  ASSERT_FALSE(project->getBoards().isEmpty());
  Board& board = *project->getBoards().first();

  BoardPnsSnapshot before(board);
  rs::PnsSnapshotStats without = {};
  rs::ffi_pnsrouter_snapshot_stats(*before, &without);

  // Six corners, one of them reflex, so the outline is not convex and no
  // single polygon shape of the router could stand in for it.
  const BI_Zone* zone = addKeepoutZone(
      board, lShapedOutline(), {&Layer::topCopper(), &Layer::botCopper()});
  EXPECT_EQ(zone->getData().getOutline().getVertices().count(), 7);

  BoardPnsSnapshot snapshot(board);
  rs::PnsSnapshotStats stats = {};
  rs::ffi_pnsrouter_snapshot_stats(*snapshot, &stats);

  // An outline of n corners is n - 2 triangles, on each of the two layers.
  EXPECT_EQ(stats.keepout_count, without.keepout_count + 8);
  EXPECT_GT(findZoneHostId(snapshot, zone), 0U);
}

TEST_F(BoardPnsSnapshotTest, testZoneWithoutTheNoCopperRuleIsNoKeepout) {
  std::unique_ptr<Project> project = openGerberTestProject();
  ASSERT_FALSE(project->getBoards().isEmpty());
  Board& board = *project->getBoards().first();

  BoardPnsSnapshot before(board);
  rs::PnsSnapshotStats without = {};
  rs::ffi_pnsrouter_snapshot_stats(*before, &without);

  // The other three rules are about planes, stop mask and devices, none of
  // which the router places.
  const BI_Zone* zone = addZone(board, squareOutline(), {&Layer::topCopper()},
                                Zone::Rule::NoPlanes | Zone::Rule::NoExposure |
                                    Zone::Rule::NoDevices);

  BoardPnsSnapshot snapshot(board);
  rs::PnsSnapshotStats stats = {};
  rs::ffi_pnsrouter_snapshot_stats(*snapshot, &stats);
  EXPECT_EQ(stats.keepout_count, without.keepout_count);
  EXPECT_EQ(stats.item_count, without.item_count);
  EXPECT_EQ(findZoneHostId(snapshot, zone), 0U);
}

/**
 * @brief A keepout excludes copper, it does not keep a distance from it
 *
 * Which is what the design rule check says too: it reports copper whose area
 * intersects the zone, at no clearance at all
 * (`BoardDesignRuleCheck::checkZones`).
 */
TEST_F(BoardPnsSnapshotTest, testKeepoutHasNoClearance) {
  std::unique_ptr<Project> project = openGerberTestProject();
  ASSERT_FALSE(project->getBoards().isEmpty());
  Board& board = *project->getBoards().first();

  const BI_Zone* zone =
      addKeepoutZone(board, squareOutline(), {&Layer::topCopper()});

  BoardPnsSnapshot snapshot(board);
  const quint64 zoneId = findZoneHostId(snapshot, zone);
  ASSERT_GT(zoneId, 0U);

  // The board's own copper to copper rule is not zero, so a zero answer can
  // only have come from the keepout rung of the ladder.
  EXPECT_GT((*board.getDrcSettings().getMinCopperCopperClearance()).toNm(), 0);

  quint64 traceId = 0;
  foreach (const BI_NetSegment* segment, board.getNetSegments()) {
    foreach (const BI_NetLine* netLine, segment->getNetLines()) {
      if (traceId == 0) {
        traceId = snapshot.getHostId(*netLine);
      }
    }
  }
  ASSERT_GT(traceId, 0U);

  qint32 clearance = -1;
  EXPECT_EQ(rs::ffi_pnsrouter_snapshot_clearance(
                *snapshot, zoneId, rs::PnsItemRole::Copper, traceId,
                rs::PnsItemRole::Copper, &clearance),
            rs::PnsResult::Ok);
  EXPECT_EQ(clearance, 0);

  // A pad is not copper the router places, so the zone is not an obstacle to
  // it at all. Whether that pad belongs there is the design rule check's
  // business, and giving the zone a copper clearance instead would drag
  // every pad standing in it into the router's walkaround clusters.
  quint64 padId = 0;
  foreach (const BI_Device* device, board.getDeviceInstances()) {
    foreach (const BI_Pad* pad, device->getPads()) {
      if (padId == 0) {
        padId = snapshot.getHostId(*pad);
      }
    }
  }
  ASSERT_GT(padId, 0U);
  EXPECT_EQ(rs::ffi_pnsrouter_snapshot_clearance(
                *snapshot, zoneId, rs::PnsItemRole::Copper, padId,
                rs::PnsItemRole::Copper, &clearance),
            rs::PnsResult::NoClearance);
}

/*******************************************************************************
 *  End of File
 ******************************************************************************/

}  // namespace tests
}  // namespace librepcb
