/**
 *    Copyright (C) 2018-present MongoDB, Inc.
 *
 *    This program is free software: you can redistribute it and/or modify
 *    it under the terms of the Server Side Public License, version 1,
 *    as published by MongoDB, Inc.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    Server Side Public License for more details.
 *
 *    You should have received a copy of the Server Side Public License
 *    along with this program. If not, see
 *    <http://www.mongodb.com/licensing/server-side-public-license>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the Server Side Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */

#include "mongo/platform/basic.h"

#include "mongo/db/client.h"
#include "mongo/db/concurrency/locker_noop.h"
#include "mongo/db/db_raii.h"
#include "mongo/db/dbdirectclient.h"
#include "mongo/db/keys_collection_client_sharded.h"
#include "mongo/db/keys_collection_manager.h"
#include "mongo/db/logical_clock.h"
#include "mongo/db/logical_time_validator.h"
#include "mongo/db/op_observer_impl.h"
#include "mongo/db/repl/oplog.h"
#include "mongo/db/repl/oplog_interface_local.h"
#include "mongo/db/repl/repl_client_info.h"
#include "mongo/db/repl/replication_coordinator_mock.h"
#include "mongo/db/repl/storage_interface_mock.h"
#include "mongo/db/service_context_d_test_fixture.h"
#include "mongo/db/session_catalog_mongod.h"
#include "mongo/db/storage/ephemeral_for_test/ephemeral_for_test_recovery_unit.h"
#include "mongo/db/transaction_participant.h"
#include "mongo/db/transaction_participant_gen.h"
#include "mongo/s/config_server_test_fixture.h"
#include "mongo/unittest/death_test.h"
#include "mongo/util/clock_source_mock.h"

namespace mongo {
namespace {

using repl::OplogEntry;
using unittest::assertGet;

class OpObserverTest : public ServiceContextMongoDTest {
public:
    void setUp() override {
        // Set up mongod.
        ServiceContextMongoDTest::setUp();

        auto service = getServiceContext();
        auto opCtx = cc().makeOperationContext();
        repl::StorageInterface::set(service, stdx::make_unique<repl::StorageInterfaceMock>());

        // Set up ReplicationCoordinator and create oplog.
        repl::ReplicationCoordinator::set(
            service,
            stdx::make_unique<repl::ReplicationCoordinatorMock>(service, createReplSettings()));
        repl::setOplogCollectionName(service);
        repl::createOplog(opCtx.get());

        // Ensure that we are primary.
        auto replCoord = repl::ReplicationCoordinator::get(opCtx.get());
        ASSERT_OK(replCoord->setFollowerMode(repl::MemberState::RS_PRIMARY));
    }

protected:
    // Assert that the oplog has the expected number of entries, and return them
    std::vector<BSONObj> getNOplogEntries(OperationContext* opCtx, int n) {
        std::vector<BSONObj> result(n);
        repl::OplogInterfaceLocal oplogInterface(opCtx, NamespaceString::kRsOplogNamespace.ns());
        auto oplogIter = oplogInterface.makeIterator();
        for (int i = n - 1; i >= 0; i--) {
            // The oplogIterator returns the entries in reverse order.
            auto opEntry = unittest::assertGet(oplogIter->next());
            result[i] = opEntry.first;
        }
        ASSERT_EQUALS(ErrorCodes::CollectionIsEmpty, oplogIter->next().getStatus());
        return result;
    }

    // Assert that oplog only has a single entry and return that oplog entry.
    BSONObj getSingleOplogEntry(OperationContext* opCtx) {
        return getNOplogEntries(opCtx, 1).back();
    }

private:
    // Creates a reasonable set of ReplSettings for most tests.  We need to be able to
    // override this to create a larger oplog.
    virtual repl::ReplSettings createReplSettings() {
        repl::ReplSettings settings;
        settings.setOplogSizeBytes(5 * 1024 * 1024);
        settings.setReplSetString("mySet/node1:12345");
        return settings;
    }
};

TEST_F(OpObserverTest, StartIndexBuildExpectedOplogEntry) {
    OpObserverImpl opObserver;
    auto opCtx = cc().makeOperationContext();
    auto uuid = CollectionUUID::gen();
    NamespaceString nss("test.coll");
    UUID indexBuildUUID = UUID::gen();

    BSONObj specX = BSON("key" << BSON("x" << 1) << "name"
                               << "x_1"
                               << "v"
                               << 2);
    BSONObj specA = BSON("key" << BSON("a" << 1) << "name"
                               << "a_1"
                               << "v"
                               << 2);
    std::vector<BSONObj> specs = {specX, specA};

    // Write to the oplog.
    {
        AutoGetDb autoDb(opCtx.get(), nss.db(), MODE_X);
        WriteUnitOfWork wunit(opCtx.get());
        opObserver.onStartIndexBuild(
            opCtx.get(), nss, uuid, indexBuildUUID, specs, false /*fromMigrate*/);
        wunit.commit();
    }

    // Create expected startIndexBuild command.
    BSONObjBuilder startIndexBuildBuilder;
    startIndexBuildBuilder.append("startIndexBuild", nss.coll());
    indexBuildUUID.appendToBuilder(&startIndexBuildBuilder, "indexBuildUUID");
    BSONArrayBuilder indexesArr(startIndexBuildBuilder.subarrayStart("indexes"));
    indexesArr.append(specX);
    indexesArr.append(specA);
    indexesArr.done();
    BSONObj startIndexBuildCmd = startIndexBuildBuilder.done();

    // Ensure the startIndexBuild fields were correctly set.
    auto oplogEntry = getSingleOplogEntry(opCtx.get());
    auto o = oplogEntry.getObjectField("o");
    ASSERT_BSONOBJ_EQ(startIndexBuildCmd, o);
}

TEST_F(OpObserverTest, CommitIndexBuildExpectedOplogEntry) {
    OpObserverImpl opObserver;
    auto opCtx = cc().makeOperationContext();
    auto uuid = CollectionUUID::gen();
    NamespaceString nss("test.coll");
    UUID indexBuildUUID = UUID::gen();

    BSONObj specX = BSON("key" << BSON("x" << 1) << "name"
                               << "x_1"
                               << "v"
                               << 2);
    BSONObj specA = BSON("key" << BSON("a" << 1) << "name"
                               << "a_1"
                               << "v"
                               << 2);
    std::vector<BSONObj> specs = {specX, specA};

    // Write to the oplog.
    {
        AutoGetDb autoDb(opCtx.get(), nss.db(), MODE_X);
        WriteUnitOfWork wunit(opCtx.get());
        opObserver.onCommitIndexBuild(
            opCtx.get(), nss, uuid, indexBuildUUID, specs, false /*fromMigrate*/);
        wunit.commit();
    }

    // Create expected commitIndexBuild command.
    BSONObjBuilder commitIndexBuildBuilder;
    commitIndexBuildBuilder.append("commitIndexBuild", nss.coll());
    indexBuildUUID.appendToBuilder(&commitIndexBuildBuilder, "indexBuildUUID");
    BSONArrayBuilder indexesArr(commitIndexBuildBuilder.subarrayStart("indexes"));
    indexesArr.append(specX);
    indexesArr.append(specA);
    indexesArr.done();
    BSONObj commitIndexBuildCmd = commitIndexBuildBuilder.done();

    // Ensure the commitIndexBuild fields were correctly set.
    auto oplogEntry = getSingleOplogEntry(opCtx.get());
    auto o = oplogEntry.getObjectField("o");
    ASSERT_BSONOBJ_EQ(commitIndexBuildCmd, o);
}

TEST_F(OpObserverTest, AbortIndexBuildExpectedOplogEntry) {
    OpObserverImpl opObserver;
    auto opCtx = cc().makeOperationContext();
    auto uuid = CollectionUUID::gen();
    NamespaceString nss("test.coll");
    UUID indexBuildUUID = UUID::gen();

    BSONObj specX = BSON("key" << BSON("x" << 1) << "name"
                               << "x_1"
                               << "v"
                               << 2);
    BSONObj specA = BSON("key" << BSON("a" << 1) << "name"
                               << "a_1"
                               << "v"
                               << 2);
    std::vector<BSONObj> specs = {specX, specA};

    // Write to the oplog.
    {
        AutoGetDb autoDb(opCtx.get(), nss.db(), MODE_X);
        WriteUnitOfWork wunit(opCtx.get());
        opObserver.onAbortIndexBuild(
            opCtx.get(), nss, uuid, indexBuildUUID, specs, false /*fromMigrate*/);
        wunit.commit();
    }

    // Create expected abortIndexBuild command.
    BSONObjBuilder abortIndexBuildBuilder;
    abortIndexBuildBuilder.append("abortIndexBuild", nss.coll());
    indexBuildUUID.appendToBuilder(&abortIndexBuildBuilder, "indexBuildUUID");
    BSONArrayBuilder indexesArr(abortIndexBuildBuilder.subarrayStart("indexes"));
    indexesArr.append(specX);
    indexesArr.append(specA);
    indexesArr.done();
    BSONObj abortIndexBuildCmd = abortIndexBuildBuilder.done();

    // Ensure the abortIndexBuild fields were correctly set.
    auto oplogEntry = getSingleOplogEntry(opCtx.get());
    auto o = oplogEntry.getObjectField("o");
    ASSERT_BSONOBJ_EQ(abortIndexBuildCmd, o);
}

TEST_F(OpObserverTest, CollModWithCollectionOptionsAndTTLInfo) {
    OpObserverImpl opObserver;
    auto opCtx = cc().makeOperationContext();
    auto uuid = CollectionUUID::gen();

    // Create 'collMod' command.
    NamespaceString nss("test.coll");
    BSONObj collModCmd = BSON("collMod" << nss.coll() << "validationLevel"
                                        << "off"
                                        << "validationAction"
                                        << "warn"
                                        // We verify that 'onCollMod' ignores this field.
                                        << "index"
                                        << "indexData");

    CollectionOptions oldCollOpts;
    oldCollOpts.validationLevel = "strict";
    oldCollOpts.validationAction = "error";
    oldCollOpts.flags = 2;
    oldCollOpts.flagsSet = true;

    TTLCollModInfo ttlInfo;
    ttlInfo.expireAfterSeconds = Seconds(10);
    ttlInfo.oldExpireAfterSeconds = Seconds(5);
    ttlInfo.indexName = "name_of_index";

    // Write to the oplog.
    {
        AutoGetDb autoDb(opCtx.get(), nss.db(), MODE_X);
        WriteUnitOfWork wunit(opCtx.get());
        opObserver.onCollMod(opCtx.get(), nss, uuid, collModCmd, oldCollOpts, ttlInfo);
        wunit.commit();
    }

    auto oplogEntry = getSingleOplogEntry(opCtx.get());

    // Ensure that collMod fields were properly added to the oplog entry.
    auto o = oplogEntry.getObjectField("o");
    auto oExpected =
        BSON("collMod" << nss.coll() << "validationLevel"
                       << "off"
                       << "validationAction"
                       << "warn"
                       << "index"
                       << BSON("name" << ttlInfo.indexName << "expireAfterSeconds"
                                      << durationCount<Seconds>(ttlInfo.expireAfterSeconds)));
    ASSERT_BSONOBJ_EQ(oExpected, o);

    // Ensure that the old collection metadata was saved.
    auto o2 = oplogEntry.getObjectField("o2");
    auto o2Expected =
        BSON("collectionOptions_old" << BSON("flags" << oldCollOpts.flags << "validationLevel"
                                                     << oldCollOpts.validationLevel
                                                     << "validationAction"
                                                     << oldCollOpts.validationAction)
                                     << "expireAfterSeconds_old"
                                     << durationCount<Seconds>(ttlInfo.oldExpireAfterSeconds));

    ASSERT_BSONOBJ_EQ(o2Expected, o2);
}

TEST_F(OpObserverTest, CollModWithOnlyCollectionOptions) {
    OpObserverImpl opObserver;
    auto opCtx = cc().makeOperationContext();
    auto uuid = CollectionUUID::gen();

    // Create 'collMod' command.
    NamespaceString nss("test.coll");
    BSONObj collModCmd = BSON("collMod" << nss.coll() << "validationLevel"
                                        << "off"
                                        << "validationAction"
                                        << "warn");

    CollectionOptions oldCollOpts;
    oldCollOpts.validationLevel = "strict";
    oldCollOpts.validationAction = "error";

    // Write to the oplog.
    {
        AutoGetDb autoDb(opCtx.get(), nss.db(), MODE_X);
        WriteUnitOfWork wunit(opCtx.get());
        opObserver.onCollMod(opCtx.get(), nss, uuid, collModCmd, oldCollOpts, boost::none);
        wunit.commit();
    }

    auto oplogEntry = getSingleOplogEntry(opCtx.get());

    // Ensure that collMod fields were properly added to oplog entry.
    auto o = oplogEntry.getObjectField("o");
    auto oExpected = collModCmd;
    ASSERT_BSONOBJ_EQ(oExpected, o);

    // Ensure that the old collection metadata was saved and that TTL info is not present.
    auto o2 = oplogEntry.getObjectField("o2");
    auto o2Expected =
        BSON("collectionOptions_old"
             << BSON("validationLevel" << oldCollOpts.validationLevel << "validationAction"
                                       << oldCollOpts.validationAction));
    ASSERT_BSONOBJ_EQ(o2Expected, o2);
}

TEST_F(OpObserverTest, OnDropCollectionReturnsDropOpTime) {
    OpObserverImpl opObserver;
    auto opCtx = cc().makeOperationContext();
    auto uuid = CollectionUUID::gen();

    // Create 'drop' command.
    NamespaceString nss("test.coll");
    auto dropCmd = BSON("drop" << nss.coll());

    // Write to the oplog.
    repl::OpTime dropOpTime;
    {
        AutoGetDb autoDb(opCtx.get(), nss.db(), MODE_X);
        WriteUnitOfWork wunit(opCtx.get());
        opObserver.onDropCollection(
            opCtx.get(), nss, uuid, 0U, OpObserver::CollectionDropType::kTwoPhase);
        dropOpTime = OpObserver::Times::get(opCtx.get()).reservedOpTimes.front();
        wunit.commit();
    }

    auto oplogEntry = getSingleOplogEntry(opCtx.get());

    // Ensure that drop fields were properly added to oplog entry.
    auto o = oplogEntry.getObjectField("o");
    auto oExpected = dropCmd;
    ASSERT_BSONOBJ_EQ(oExpected, o);

    // Ensure that the drop optime returned is the same as the last optime in the ReplClientInfo.
    ASSERT_EQUALS(repl::ReplClientInfo::forClient(&cc()).getLastOp(), dropOpTime);
}

TEST_F(OpObserverTest, OnRenameCollectionReturnsRenameOpTime) {
    OpObserverImpl opObserver;
    auto opCtx = cc().makeOperationContext();

    auto uuid = CollectionUUID::gen();
    auto dropTargetUuid = CollectionUUID::gen();
    auto stayTemp = false;
    NamespaceString sourceNss("test.foo");
    NamespaceString targetNss("test.bar");

    // Write to the oplog.
    repl::OpTime renameOpTime;
    {
        AutoGetDb autoDb(opCtx.get(), sourceNss.db(), MODE_X);
        WriteUnitOfWork wunit(opCtx.get());
        opObserver.onRenameCollection(
            opCtx.get(), sourceNss, targetNss, uuid, dropTargetUuid, 0U, stayTemp);
        renameOpTime = OpObserver::Times::get(opCtx.get()).reservedOpTimes.front();
        wunit.commit();
    }

    auto oplogEntry = getSingleOplogEntry(opCtx.get());

    // Ensure that renameCollection fields were properly added to oplog entry.
    ASSERT_EQUALS(uuid, unittest::assertGet(UUID::parse(oplogEntry["ui"])));
    auto o = oplogEntry.getObjectField("o");
    auto oExpected = BSON(
        "renameCollection" << sourceNss.ns() << "to" << targetNss.ns() << "stayTemp" << stayTemp
                           << "dropTarget"
                           << dropTargetUuid);
    ASSERT_BSONOBJ_EQ(oExpected, o);

    // Ensure that the rename optime returned is the same as the last optime in the ReplClientInfo.
    ASSERT_EQUALS(repl::ReplClientInfo::forClient(&cc()).getLastOp(), renameOpTime);
}

TEST_F(OpObserverTest, OnRenameCollectionOmitsDropTargetFieldIfDropTargetUuidIsNull) {
    OpObserverImpl opObserver;
    auto opCtx = cc().makeOperationContext();

    auto uuid = CollectionUUID::gen();
    auto stayTemp = true;
    NamespaceString sourceNss("test.foo");
    NamespaceString targetNss("test.bar");

    // Write to the oplog.
    {
        AutoGetDb autoDb(opCtx.get(), sourceNss.db(), MODE_X);
        WriteUnitOfWork wunit(opCtx.get());
        opObserver.onRenameCollection(opCtx.get(), sourceNss, targetNss, uuid, {}, 0U, stayTemp);
        wunit.commit();
    }

    auto oplogEntry = getSingleOplogEntry(opCtx.get());

    // Ensure that renameCollection fields were properly added to oplog entry.
    ASSERT_EQUALS(uuid, unittest::assertGet(UUID::parse(oplogEntry["ui"])));
    auto o = oplogEntry.getObjectField("o");
    auto oExpected = BSON(
        "renameCollection" << sourceNss.ns() << "to" << targetNss.ns() << "stayTemp" << stayTemp);
    ASSERT_BSONOBJ_EQ(oExpected, o);
}

/**
 * Test fixture for testing OpObserver behavior specific to the SessionCatalog.
 */
class OpObserverSessionCatalogRollbackTest : public OpObserverTest {
public:
    void setUp() override {
        OpObserverTest::setUp();

        auto opCtx = cc().makeOperationContext();
        MongoDSessionCatalog::onStepUp(opCtx.get());
    }

    /**
     * Simulate a new write occurring on given session with the given transaction number and
     * statement id.
     */
    void simulateSessionWrite(OperationContext* opCtx,
                              TransactionParticipant::Participant txnParticipant,
                              NamespaceString nss,
                              TxnNumber txnNum,
                              StmtId stmtId) {
        txnParticipant.beginOrContinue(opCtx, txnNum, boost::none, boost::none);

        {
            AutoGetCollection autoColl(opCtx, nss, MODE_IX);
            WriteUnitOfWork wuow(opCtx);
            auto opTime = repl::OpTime(Timestamp(10, 1), 1);  // Dummy timestamp.
            txnParticipant.onWriteOpCompletedOnPrimary(
                opCtx, txnNum, {stmtId}, opTime, Date_t::now(), boost::none);
            wuow.commit();
        }
    }
};

TEST_F(OpObserverSessionCatalogRollbackTest,
       OnRollbackDoesntInvalidateSessionCatalogIfNoSessionOpsRolledBack) {
    const NamespaceString nss("testDB", "testColl");

    auto sessionId = makeLogicalSessionIdForTest();

    const TxnNumber txnNum = 0;
    const StmtId stmtId = 1000;

    {
        auto opCtx = cc().makeOperationContext();
        opCtx->setLogicalSessionId(sessionId);
        MongoDOperationContextSession ocs(opCtx.get());
        auto txnParticipant = TransactionParticipant::get(opCtx.get());
        txnParticipant.refreshFromStorageIfNeeded(opCtx.get());

        // Simulate a write occurring on that session
        simulateSessionWrite(opCtx.get(), txnParticipant, nss, txnNum, stmtId);

        // Check that the statement executed
        ASSERT(txnParticipant.checkStatementExecutedNoOplogEntryFetch(stmtId));
    }

    // Because there are no sessions to rollback, the OpObserver should not invalidate the in-memory
    // session state, so the check after this should still succeed.
    {
        auto opCtx = cc().makeOperationContext();

        OpObserverImpl opObserver;
        OpObserver::RollbackObserverInfo rbInfo;
        opObserver.onReplicationRollback(opCtx.get(), rbInfo);
    }

    {
        auto opCtx = cc().makeOperationContext();
        opCtx->setLogicalSessionId(sessionId);
        MongoDOperationContextSession ocs(opCtx.get());
        auto txnParticipant = TransactionParticipant::get(opCtx.get());
        ASSERT(txnParticipant.checkStatementExecutedNoOplogEntryFetch(stmtId));
    }
}

TEST_F(OpObserverTest, MultipleAboutToDeleteAndOnDelete) {
    auto uuid = UUID::gen();
    OpObserverImpl opObserver;
    auto opCtx = cc().makeOperationContext();
    NamespaceString nss = {"test", "coll"};
    AutoGetDb autoDb(opCtx.get(), nss.db(), MODE_X);
    WriteUnitOfWork wunit(opCtx.get());
    opObserver.aboutToDelete(opCtx.get(), nss, BSON("_id" << 1));
    opObserver.onDelete(opCtx.get(), nss, uuid, {}, false, {});
    opObserver.aboutToDelete(opCtx.get(), nss, BSON("_id" << 1));
    opObserver.onDelete(opCtx.get(), nss, uuid, {}, false, {});
}

DEATH_TEST_F(OpObserverTest, AboutToDeleteMustPreceedOnDelete, "invariant") {
    OpObserverImpl opObserver;
    auto opCtx = cc().makeOperationContext();
    opCtx->swapLockState(stdx::make_unique<LockerNoop>());
    NamespaceString nss = {"test", "coll"};
    opObserver.onDelete(opCtx.get(), nss, {}, {}, false, {});
}

DEATH_TEST_F(OpObserverTest, EachOnDeleteRequiresAboutToDelete, "invariant") {
    OpObserverImpl opObserver;
    auto opCtx = cc().makeOperationContext();
    opCtx->swapLockState(stdx::make_unique<LockerNoop>());
    NamespaceString nss = {"test", "coll"};
    opObserver.aboutToDelete(opCtx.get(), nss, {});
    opObserver.onDelete(opCtx.get(), nss, {}, {}, false, {});
    opObserver.onDelete(opCtx.get(), nss, {}, {}, false, {});
}

DEATH_TEST_F(OpObserverTest,
             NodeCrashesIfShardIdentityDocumentRolledBack,
             "Fatal Assertion 50712") {
    OpObserverImpl opObserver;
    auto opCtx = cc().makeOperationContext();

    OpObserver::RollbackObserverInfo rbInfo;
    rbInfo.shardIdentityRolledBack = true;
    opObserver.onReplicationRollback(opCtx.get(), rbInfo);
}

/**
 * Test fixture for testing OpObserver behavior specific to multi-document transactions.
 */

class OpObserverTransactionTest : public OpObserverTest {
public:
    void setUp() override {
        OpObserverTest::setUp();
        _opCtx = cc().makeOperationContext();

        _opObserver.emplace();

        MongoDSessionCatalog::onStepUp(opCtx());
        _times.emplace(opCtx());

        opCtx()->setLogicalSessionId(makeLogicalSessionIdForTest());
        opCtx()->setTxnNumber(txnNum());
        _sessionCheckout = std::make_unique<MongoDOperationContextSession>(opCtx());

        auto txnParticipant = TransactionParticipant::get(opCtx());
        txnParticipant.beginOrContinue(opCtx(), *opCtx()->getTxnNumber(), false, true);
    }

    void tearDown() override {
        _sessionCheckout.reset();
        _times.reset();
        _opCtx.reset();

        OpObserverTest::tearDown();
    }


protected:
    void checkSessionAndTransactionFields(const BSONObj& oplogEntry, int expectedStmtId) {
        ASSERT_BSONOBJ_EQ(session()->getSessionId().toBSON(), oplogEntry.getObjectField("lsid"));
        ASSERT_EQ(*opCtx()->getTxnNumber(), oplogEntry.getField("txnNumber").safeNumberLong());
        ASSERT_EQ(expectedStmtId, oplogEntry.getIntField("stmtId"));
    }
    void checkCommonFields(const BSONObj& oplogEntry, int expectedStmtId = 0) {
        ASSERT_EQ("c"_sd, oplogEntry.getStringField("op"));
        ASSERT_EQ("admin.$cmd"_sd, oplogEntry.getStringField("ns"));
        checkSessionAndTransactionFields(oplogEntry, expectedStmtId);
    }

    void assertTxnRecord(TxnNumber txnNum,
                         repl::OpTime opTime,
                         boost::optional<DurableTxnStateEnum> txnState) {
        DBDirectClient client(opCtx());
        auto cursor = client.query(NamespaceString::kSessionTransactionsTableNamespace,
                                   {BSON("_id" << session()->getSessionId().toBSON())});
        ASSERT(cursor);
        ASSERT(cursor->more());

        auto txnRecordObj = cursor->next();
        auto txnRecord =
            SessionTxnRecord::parse(IDLParserErrorContext("SessionEntryWritten"), txnRecordObj);
        ASSERT(!cursor->more());
        ASSERT_EQ(session()->getSessionId(), txnRecord.getSessionId());
        ASSERT_EQ(txnNum, txnRecord.getTxnNum());
        ASSERT(txnRecord.getState() == txnState);
        ASSERT_EQ(txnState != boost::none,
                  txnRecordObj.hasField(SessionTxnRecord::kStateFieldName));

        auto txnParticipant = TransactionParticipant::get(opCtx());
        if (!opTime.isNull()) {
            ASSERT_EQ(opTime, txnRecord.getLastWriteOpTime());
            ASSERT_EQ(opTime, txnParticipant.getLastWriteOpTime());
        } else {
            ASSERT_EQ(txnRecord.getLastWriteOpTime(), txnParticipant.getLastWriteOpTime());
        }
    }

    void assertNoTxnRecord() {
        DBDirectClient client(opCtx());
        auto cursor = client.query(NamespaceString::kSessionTransactionsTableNamespace,
                                   {BSON("_id" << session()->getSessionId().toBSON())});
        ASSERT(cursor);
        ASSERT(!cursor->more());
    }

    Session* session() {
        return OperationContextSession::get(opCtx());
    }

    OpObserverImpl& opObserver() {
        return *_opObserver;
    }

    OperationContext* opCtx() {
        return _opCtx.get();
    }

    TxnNumber& txnNum() {
        return _txnNum;
    }

private:
    class ExposeOpObserverTimes : public OpObserver {
    public:
        typedef OpObserver::ReservedTimes ReservedTimes;
    };

    ServiceContext::UniqueOperationContext _opCtx;

    boost::optional<OpObserverImpl> _opObserver;
    boost::optional<ExposeOpObserverTimes::ReservedTimes> _times;

    std::unique_ptr<MongoDOperationContextSession> _sessionCheckout;
    TxnNumber _txnNum = 0;
};

/**
 * Test fixture with sessions and an extra-large oplog for testing large transactions.
 */
class OpObserverLargeTransactionTest : public OpObserverTransactionTest {
private:
    repl::ReplSettings createReplSettings() override {
        repl::ReplSettings settings;
        // We need an oplog comfortably large enough to hold an oplog entry that exceeds the BSON
        // size limit.  Otherwise we will get the wrong error code when trying to write one.
        settings.setOplogSizeBytes(BSONObjMaxInternalSize + 2 * 1024 * 1024);
        settings.setReplSetString("mySet/node1:12345");
        return settings;
    }
};

// Tests that a transaction aborts if it becomes too large only during the commit.
TEST_F(OpObserverLargeTransactionTest, TransactionTooLargeWhileCommitting) {
    const NamespaceString nss("testDB", "testColl");
    auto uuid = CollectionUUID::gen();

    auto txnParticipant = TransactionParticipant::get(opCtx());
    txnParticipant.unstashTransactionResources(opCtx(), "insert");

    // This size is crafted such that two operations of this size are not too big to fit in a single
    // oplog entry, but two operations plus oplog overhead are too big to fit in a single oplog
    // entry.
    constexpr size_t kHalfTransactionSize = BSONObjMaxInternalSize / 2 - 175;
    std::unique_ptr<uint8_t[]> halfTransactionData(new uint8_t[kHalfTransactionSize]());
    auto operation = repl::OplogEntry::makeInsertOperation(
        nss,
        uuid,
        BSON(
            "_id" << 0 << "data"
                  << BSONBinData(halfTransactionData.get(), kHalfTransactionSize, BinDataGeneral)));
    txnParticipant.addTransactionOperation(opCtx(), operation);
    txnParticipant.addTransactionOperation(opCtx(), operation);
    ASSERT_THROWS_CODE(opObserver().onUnpreparedTransactionCommit(
                           opCtx(), txnParticipant.retrieveCompletedTransactionOperations(opCtx())),
                       AssertionException,
                       ErrorCodes::TransactionTooLarge);
}

TEST_F(OpObserverTransactionTest, TransactionalPrepareTest) {
    const NamespaceString nss1("testDB", "testColl");
    const NamespaceString nss2("testDB2", "testColl2");
    auto uuid1 = CollectionUUID::gen();
    auto uuid2 = CollectionUUID::gen();
    auto txnParticipant = TransactionParticipant::get(opCtx());
    txnParticipant.unstashTransactionResources(opCtx(), "insert");

    WriteUnitOfWork wuow(opCtx());
    AutoGetCollection autoColl1(opCtx(), nss1, MODE_IX);
    AutoGetCollection autoColl2(opCtx(), nss2, MODE_IX);

    std::vector<InsertStatement> inserts1;
    inserts1.emplace_back(0,
                          BSON("_id" << 0 << "data"
                                     << "x"));
    inserts1.emplace_back(1,
                          BSON("_id" << 1 << "data"
                                     << "y"));
    opObserver().onInserts(opCtx(), nss1, uuid1, inserts1.begin(), inserts1.end(), false);

    CollectionUpdateArgs updateArgs2;
    updateArgs2.stmtId = 1;
    updateArgs2.updatedDoc = BSON("_id" << 0 << "data"
                                        << "y");
    updateArgs2.update = BSON("$set" << BSON("data"
                                             << "y"));
    updateArgs2.criteria = BSON("_id" << 0);
    OplogUpdateEntryArgs update2(std::move(updateArgs2), nss2, uuid2);
    opObserver().onUpdate(opCtx(), update2);

    opObserver().aboutToDelete(opCtx(),
                               nss1,
                               BSON("_id" << 0 << "data"
                                          << "x"));
    opObserver().onDelete(opCtx(), nss1, uuid1, 0, false, boost::none);

    {
        WriteUnitOfWork wuow(opCtx());
        OplogSlot slot = repl::getNextOpTime(opCtx());
        txnParticipant.transitionToPreparedforTest(opCtx(), slot.opTime);
        opCtx()->recoveryUnit()->setPrepareTimestamp(slot.opTime.getTimestamp());
        opObserver().onTransactionPrepare(
            opCtx(), {slot}, txnParticipant.retrieveCompletedTransactionOperations(opCtx()));
    }

    auto oplogEntryObj = getSingleOplogEntry(opCtx());
    checkCommonFields(oplogEntryObj);
    OplogEntry oplogEntry = assertGet(OplogEntry::parse(oplogEntryObj));
    auto o = oplogEntry.getObject();
    auto oExpected = BSON("applyOps" << BSON_ARRAY(BSON("op"
                                                        << "i"
                                                        << "ns"
                                                        << nss1.toString()
                                                        << "ui"
                                                        << uuid1
                                                        << "o"
                                                        << BSON("_id" << 0 << "data"
                                                                      << "x"))
                                                   << BSON("op"
                                                           << "i"
                                                           << "ns"
                                                           << nss1.toString()
                                                           << "ui"
                                                           << uuid1
                                                           << "o"
                                                           << BSON("_id" << 1 << "data"
                                                                         << "y"))
                                                   << BSON("op"
                                                           << "u"
                                                           << "ns"
                                                           << nss2.toString()
                                                           << "ui"
                                                           << uuid2
                                                           << "o"
                                                           << BSON("$set" << BSON("data"
                                                                                  << "y"))
                                                           << "o2"
                                                           << BSON("_id" << 0))
                                                   << BSON("op"
                                                           << "d"
                                                           << "ns"
                                                           << nss1.toString()
                                                           << "ui"
                                                           << uuid1
                                                           << "o"
                                                           << BSON("_id" << 0)))
                                     << "prepare"
                                     << true);
    ASSERT_BSONOBJ_EQ(oExpected, o);
    ASSERT(oplogEntry.getPrepare());
    ASSERT(oplogEntry.getPrepare().get());
    ASSERT_EQ(oplogEntry.getTimestamp(), opCtx()->recoveryUnit()->getPrepareTimestamp());
}

TEST_F(OpObserverTransactionTest, TransactionalPreparedCommitTest) {
    const NamespaceString nss("testDB", "testColl");
    const auto uuid = CollectionUUID::gen();
    const auto doc = BSON("_id" << 0 << "data"
                                << "x");

    auto txnParticipant = TransactionParticipant::get(opCtx());
    txnParticipant.unstashTransactionResources(opCtx(), "insert");

    std::vector<InsertStatement> insert;
    insert.emplace_back(0, doc);

    OplogSlot commitSlot;
    Timestamp prepareTimestamp;
    {
        WriteUnitOfWork wuow(opCtx());
        AutoGetCollection autoColl(opCtx(), nss, MODE_IX);
        opObserver().onInserts(opCtx(), nss, uuid, insert.begin(), insert.end(), false);

        const auto prepareSlot = repl::getNextOpTime(opCtx());
        txnParticipant.transitionToPreparedforTest(opCtx(), prepareSlot.opTime);
        prepareTimestamp = prepareSlot.opTime.getTimestamp();
        opObserver().onTransactionPrepare(
            opCtx(), {prepareSlot}, txnParticipant.retrieveCompletedTransactionOperations(opCtx()));

        commitSlot = repl::getNextOpTime(opCtx());
    }

    // Mimic committing the transaction.
    opCtx()->setWriteUnitOfWork(nullptr);
    opCtx()->lockState()->unsetMaxLockTimeout();

    txnParticipant.transitionToCommittingWithPrepareforTest(opCtx());
    opObserver().onPreparedTransactionCommit(
        opCtx(),
        commitSlot,
        prepareTimestamp,
        txnParticipant.retrieveCompletedTransactionOperations(opCtx()));

    repl::OplogInterfaceLocal oplogInterface(opCtx(), NamespaceString::kRsOplogNamespace.ns());
    auto oplogIter = oplogInterface.makeIterator();
    {
        auto oplogEntryObj = unittest::assertGet(oplogIter->next()).first;
        checkCommonFields(oplogEntryObj, 1);
        OplogEntry oplogEntry = assertGet(OplogEntry::parse(oplogEntryObj));
        auto o = oplogEntry.getObject();
        auto oExpected = BSON("commitTransaction" << 1 << "commitTimestamp" << prepareTimestamp);
        ASSERT_BSONOBJ_EQ(oExpected, o);
        ASSERT_FALSE(oplogEntry.getPrepare());
    }

    {
        auto oplogEntryObj = unittest::assertGet(oplogIter->next()).first;
        checkCommonFields(oplogEntryObj);
        OplogEntry oplogEntry = assertGet(OplogEntry::parse(oplogEntryObj));
        auto o = oplogEntry.getObject();
        auto oExpected = BSON("applyOps" << BSON_ARRAY(BSON("op"
                                                            << "i"
                                                            << "ns"
                                                            << nss.toString()
                                                            << "ui"
                                                            << uuid
                                                            << "o"
                                                            << doc))
                                         << "prepare"
                                         << true);
        ASSERT_BSONOBJ_EQ(oExpected, o);
        ASSERT(oplogEntry.getPrepare());
    }

    ASSERT_EQUALS(ErrorCodes::CollectionIsEmpty, oplogIter->next().getStatus());
}

TEST_F(OpObserverTransactionTest, TransactionalPreparedAbortTest) {
    const NamespaceString nss("testDB", "testColl");
    const auto uuid = CollectionUUID::gen();
    const auto doc = BSON("_id" << 0 << "data"
                                << "x");

    auto txnParticipant = TransactionParticipant::get(opCtx());
    txnParticipant.unstashTransactionResources(opCtx(), "insert");

    std::vector<InsertStatement> insert;
    insert.emplace_back(0, doc);

    OplogSlot abortSlot;
    {
        WriteUnitOfWork wuow(opCtx());
        AutoGetCollection autoColl(opCtx(), nss, MODE_IX);
        opObserver().onInserts(opCtx(), nss, uuid, insert.begin(), insert.end(), false);

        const auto prepareSlot = repl::getNextOpTime(opCtx());
        txnParticipant.transitionToPreparedforTest(opCtx(), prepareSlot.opTime);
        opObserver().onTransactionPrepare(
            opCtx(), {prepareSlot}, txnParticipant.retrieveCompletedTransactionOperations(opCtx()));
        abortSlot = repl::getNextOpTime(opCtx());
    }

    // Mimic aborting the transaction.
    opCtx()->setWriteUnitOfWork(nullptr);
    opCtx()->lockState()->unsetMaxLockTimeout();
    opObserver().onTransactionAbort(opCtx(), abortSlot);
    txnParticipant.transitionToAbortedWithPrepareforTest(opCtx());

    repl::OplogInterfaceLocal oplogInterface(opCtx(), NamespaceString::kRsOplogNamespace.ns());
    auto oplogIter = oplogInterface.makeIterator();
    {
        auto oplogEntryObj = unittest::assertGet(oplogIter->next()).first;
        checkCommonFields(oplogEntryObj, 1);
        OplogEntry oplogEntry = assertGet(OplogEntry::parse(oplogEntryObj));
        auto o = oplogEntry.getObject();
        auto oExpected = BSON("abortTransaction" << 1);
        ASSERT_BSONOBJ_EQ(oExpected, o);
        ASSERT_FALSE(oplogEntry.getPrepare());
    }

    {
        auto oplogEntryObj = unittest::assertGet(oplogIter->next()).first;
        checkCommonFields(oplogEntryObj);
        OplogEntry oplogEntry = assertGet(OplogEntry::parse(oplogEntryObj));
        auto o = oplogEntry.getObject();
        auto oExpected = BSON("applyOps" << BSON_ARRAY(BSON("op"
                                                            << "i"
                                                            << "ns"
                                                            << nss.toString()
                                                            << "ui"
                                                            << uuid
                                                            << "o"
                                                            << doc))
                                         << "prepare"
                                         << true);
        ASSERT_BSONOBJ_EQ(oExpected, o);
        ASSERT(oplogEntry.getPrepare());
    }

    ASSERT_EQUALS(ErrorCodes::CollectionIsEmpty, oplogIter->next().getStatus());
}

TEST_F(OpObserverTransactionTest, TransactionalUnpreparedAbortTest) {
    const NamespaceString nss("testDB", "testColl");
    const auto uuid = CollectionUUID::gen();
    auto txnParticipant = TransactionParticipant::get(opCtx());
    txnParticipant.unstashTransactionResources(opCtx(), "insert");

    std::vector<InsertStatement> insert;
    insert.emplace_back(0,
                        BSON("_id" << 0 << "data"
                                   << "x"));

    {
        WriteUnitOfWork wuow(opCtx());
        AutoGetCollection autoColl(opCtx(), nss, MODE_IX);
        opObserver().onInserts(opCtx(), nss, uuid, insert.begin(), insert.end(), false);

        txnParticipant.transitionToAbortedWithoutPrepareforTest(opCtx());
        opObserver().onTransactionAbort(opCtx(), boost::none);
    }

    // Assert no oplog entries were written.
    repl::OplogInterfaceLocal oplogInterface(opCtx(), NamespaceString::kRsOplogNamespace.ns());
    auto oplogIter = oplogInterface.makeIterator();
    ASSERT_EQUALS(ErrorCodes::CollectionIsEmpty, oplogIter->next().getStatus());
}

TEST_F(OpObserverTransactionTest, PreparingEmptyTransactionLogsEmptyApplyOps) {
    auto txnParticipant = TransactionParticipant::get(opCtx());
    txnParticipant.unstashTransactionResources(opCtx(), "prepareTransaction");

    {
        WriteUnitOfWork wuow(opCtx());
        OplogSlot slot = repl::getNextOpTime(opCtx());
        txnParticipant.transitionToPreparedforTest(opCtx(), slot.opTime);
        opCtx()->recoveryUnit()->setPrepareTimestamp(slot.opTime.getTimestamp());
        opObserver().onTransactionPrepare(
            opCtx(), {slot}, txnParticipant.retrieveCompletedTransactionOperations(opCtx()));
    }

    auto oplogEntryObj = getSingleOplogEntry(opCtx());
    checkCommonFields(oplogEntryObj);
    OplogEntry oplogEntry = assertGet(OplogEntry::parse(oplogEntryObj));
    auto o = oplogEntry.getObject();
    auto oExpected = BSON("applyOps" << BSONArray() << "prepare" << true);
    ASSERT_BSONOBJ_EQ(oExpected, o);
    ASSERT(oplogEntry.getPrepare());
    ASSERT(oplogEntry.getPrepare().get());
    ASSERT_EQ(oplogEntry.getTimestamp(), opCtx()->recoveryUnit()->getPrepareTimestamp());
}

TEST_F(OpObserverTransactionTest, PreparingTransactionWritesToTransactionTable) {
    auto txnParticipant = TransactionParticipant::get(opCtx());
    txnParticipant.unstashTransactionResources(opCtx(), "prepareTransaction");

    repl::OpTime prepareOpTime;
    {
        WriteUnitOfWork wuow(opCtx());
        OplogSlot slot = repl::getNextOpTime(opCtx());
        txnParticipant.transitionToPreparedforTest(opCtx(), slot.opTime);
        prepareOpTime = slot.opTime;
        opObserver().onTransactionPrepare(
            opCtx(), {slot}, txnParticipant.retrieveCompletedTransactionOperations(opCtx()));
        opCtx()->recoveryUnit()->setPrepareTimestamp(slot.opTime.getTimestamp());
    }

    ASSERT_EQ(prepareOpTime.getTimestamp(), opCtx()->recoveryUnit()->getPrepareTimestamp());
    txnParticipant.stashTransactionResources(opCtx());
    assertTxnRecord(txnNum(), prepareOpTime, DurableTxnStateEnum::kPrepared);
    txnParticipant.unstashTransactionResources(opCtx(), "abortTransaction");
}

TEST_F(OpObserverTransactionTest, AbortingUnpreparedTransactionDoesNotWriteToTransactionTable) {
    auto txnParticipant = TransactionParticipant::get(opCtx());
    txnParticipant.unstashTransactionResources(opCtx(), "prepareTransaction");

    opObserver().onTransactionAbort(opCtx(), boost::none);
    txnParticipant.stashTransactionResources(opCtx());

    // Abort the storage-transaction without calling the OpObserver.
    txnParticipant.shutdown(opCtx());

    assertNoTxnRecord();
}

TEST_F(OpObserverTransactionTest, AbortingPreparedTransactionWritesToTransactionTable) {
    auto txnParticipant = TransactionParticipant::get(opCtx());
    txnParticipant.unstashTransactionResources(opCtx(), "prepareTransaction");

    OplogSlot abortSlot;
    {
        WriteUnitOfWork wuow(opCtx());
        OplogSlot slot = repl::getNextOpTime(opCtx());
        opCtx()->recoveryUnit()->setPrepareTimestamp(slot.opTime.getTimestamp());
        opObserver().onTransactionPrepare(
            opCtx(), {slot}, txnParticipant.retrieveCompletedTransactionOperations(opCtx()));
        txnParticipant.transitionToPreparedforTest(opCtx(), slot.opTime);
        abortSlot = repl::getNextOpTime(opCtx());
    }

    // Mimic aborting the transaction.
    opCtx()->setWriteUnitOfWork(nullptr);
    opCtx()->lockState()->unsetMaxLockTimeout();
    opObserver().onTransactionAbort(opCtx(), abortSlot);
    txnParticipant.transitionToAbortedWithPrepareforTest(opCtx());

    txnParticipant.stashTransactionResources(opCtx());

    // Abort the storage-transaction without calling the OpObserver.
    txnParticipant.shutdown(opCtx());

    assertTxnRecord(txnNum(), {}, DurableTxnStateEnum::kAborted);
}

TEST_F(OpObserverTransactionTest, CommittingUnpreparedNonEmptyTransactionWritesToTransactionTable) {
    const NamespaceString nss("testDB", "testColl");
    const auto uuid = CollectionUUID::gen();
    auto txnParticipant = TransactionParticipant::get(opCtx());
    txnParticipant.unstashTransactionResources(opCtx(), "prepareTransaction");

    std::vector<InsertStatement> insert;
    insert.emplace_back(0,
                        BSON("_id" << 0 << "data"
                                   << "x"));

    {
        AutoGetCollection autoColl(opCtx(), nss, MODE_IX);
        opObserver().onInserts(opCtx(), nss, uuid, insert.begin(), insert.end(), false);
    }

    opObserver().onUnpreparedTransactionCommit(
        opCtx(), txnParticipant.retrieveCompletedTransactionOperations(opCtx()));
    opCtx()->getWriteUnitOfWork()->commit();

    assertTxnRecord(txnNum(), {}, DurableTxnStateEnum::kCommitted);
}

TEST_F(OpObserverTransactionTest,
       CommittingUnpreparedEmptyTransactionDoesNotWriteToTransactionTable) {
    auto txnParticipant = TransactionParticipant::get(opCtx());
    txnParticipant.unstashTransactionResources(opCtx(), "prepareTransaction");

    opObserver().onUnpreparedTransactionCommit(
        opCtx(), txnParticipant.retrieveCompletedTransactionOperations(opCtx()));

    txnParticipant.stashTransactionResources(opCtx());

    // Abort the storage-transaction without calling the OpObserver.
    txnParticipant.shutdown(opCtx());

    assertNoTxnRecord();
}

TEST_F(OpObserverTransactionTest, CommittingPreparedTransactionWritesToTransactionTable) {
    auto txnParticipant = TransactionParticipant::get(opCtx());
    txnParticipant.unstashTransactionResources(opCtx(), "prepareTransaction");

    repl::OpTime prepareOpTime;
    {
        WriteUnitOfWork wuow(opCtx());
        OplogSlot slot = repl::getNextOpTime(opCtx());
        prepareOpTime = slot.opTime;
        opCtx()->recoveryUnit()->setPrepareTimestamp(slot.opTime.getTimestamp());
        opObserver().onTransactionPrepare(
            opCtx(), {slot}, txnParticipant.retrieveCompletedTransactionOperations(opCtx()));
        txnParticipant.transitionToPreparedforTest(opCtx(), slot.opTime);
    }

    OplogSlot commitSlot = repl::getNextOpTime(opCtx());
    repl::OpTime commitOpTime = commitSlot.opTime;
    ASSERT_LTE(prepareOpTime, commitOpTime);

    // Mimic committing the transaction.
    opCtx()->setWriteUnitOfWork(nullptr);
    opCtx()->lockState()->unsetMaxLockTimeout();

    txnParticipant.transitionToCommittingWithPrepareforTest(opCtx());
    opObserver().onPreparedTransactionCommit(
        opCtx(),
        commitSlot,
        prepareOpTime.getTimestamp(),
        txnParticipant.retrieveCompletedTransactionOperations(opCtx()));

    assertTxnRecord(txnNum(), commitOpTime, DurableTxnStateEnum::kCommitted);
}

TEST_F(OpObserverTransactionTest, TransactionalInsertTest) {
    const NamespaceString nss1("testDB", "testColl");
    const NamespaceString nss2("testDB2", "testColl2");
    auto uuid1 = CollectionUUID::gen();
    auto uuid2 = CollectionUUID::gen();
    auto txnParticipant = TransactionParticipant::get(opCtx());
    txnParticipant.unstashTransactionResources(opCtx(), "insert");

    std::vector<InsertStatement> inserts1;
    inserts1.emplace_back(0,
                          BSON("_id" << 0 << "data"
                                     << "x"));
    inserts1.emplace_back(1,
                          BSON("_id" << 1 << "data"
                                     << "y"));
    std::vector<InsertStatement> inserts2;
    inserts2.emplace_back(0,
                          BSON("_id" << 2 << "data"
                                     << "z"));
    inserts2.emplace_back(1,
                          BSON("_id" << 3 << "data"
                                     << "w"));
    WriteUnitOfWork wuow(opCtx());
    AutoGetCollection autoColl1(opCtx(), nss1, MODE_IX);
    AutoGetCollection autoColl2(opCtx(), nss2, MODE_IX);
    opObserver().onInserts(opCtx(), nss1, uuid1, inserts1.begin(), inserts1.end(), false);
    opObserver().onInserts(opCtx(), nss2, uuid2, inserts2.begin(), inserts2.end(), false);
    opObserver().onUnpreparedTransactionCommit(
        opCtx(), txnParticipant.retrieveCompletedTransactionOperations(opCtx()));
    auto oplogEntryObj = getSingleOplogEntry(opCtx());
    checkCommonFields(oplogEntryObj);
    OplogEntry oplogEntry = assertGet(OplogEntry::parse(oplogEntryObj));
    auto o = oplogEntry.getObject();
    auto oExpected = BSON("applyOps" << BSON_ARRAY(BSON("op"
                                                        << "i"
                                                        << "ns"
                                                        << nss1.toString()
                                                        << "ui"
                                                        << uuid1
                                                        << "o"
                                                        << BSON("_id" << 0 << "data"
                                                                      << "x"))
                                                   << BSON("op"
                                                           << "i"
                                                           << "ns"
                                                           << nss1.toString()
                                                           << "ui"
                                                           << uuid1
                                                           << "o"
                                                           << BSON("_id" << 1 << "data"
                                                                         << "y"))
                                                   << BSON("op"
                                                           << "i"
                                                           << "ns"
                                                           << nss2.toString()
                                                           << "ui"
                                                           << uuid2
                                                           << "o"
                                                           << BSON("_id" << 2 << "data"
                                                                         << "z"))
                                                   << BSON("op"
                                                           << "i"
                                                           << "ns"
                                                           << nss2.toString()
                                                           << "ui"
                                                           << uuid2
                                                           << "o"
                                                           << BSON("_id" << 3 << "data"
                                                                         << "w"))));
    ASSERT_BSONOBJ_EQ(oExpected, o);
    ASSERT(!oplogEntry.getPrepare());
    ASSERT_FALSE(oplogEntryObj.hasField("prepare"));
}

TEST_F(OpObserverTransactionTest, TransactionalUpdateTest) {
    const NamespaceString nss1("testDB", "testColl");
    const NamespaceString nss2("testDB2", "testColl2");
    auto uuid1 = CollectionUUID::gen();
    auto uuid2 = CollectionUUID::gen();
    auto txnParticipant = TransactionParticipant::get(opCtx());
    txnParticipant.unstashTransactionResources(opCtx(), "update");

    CollectionUpdateArgs updateArgs1;
    updateArgs1.stmtId = 0;
    updateArgs1.updatedDoc = BSON("_id" << 0 << "data"
                                        << "x");
    updateArgs1.update = BSON("$set" << BSON("data"
                                             << "x"));
    updateArgs1.criteria = BSON("_id" << 0);
    OplogUpdateEntryArgs update1(std::move(updateArgs1), nss1, uuid1);

    CollectionUpdateArgs updateArgs2;
    updateArgs2.stmtId = 1;
    updateArgs2.updatedDoc = BSON("_id" << 1 << "data"
                                        << "y");
    updateArgs2.update = BSON("$set" << BSON("data"
                                             << "y"));
    updateArgs2.criteria = BSON("_id" << 1);
    OplogUpdateEntryArgs update2(std::move(updateArgs2), nss2, uuid2);

    WriteUnitOfWork wuow(opCtx());
    AutoGetCollection autoColl1(opCtx(), nss1, MODE_IX);
    AutoGetCollection autoColl2(opCtx(), nss2, MODE_IX);
    opObserver().onUpdate(opCtx(), update1);
    opObserver().onUpdate(opCtx(), update2);
    opObserver().onUnpreparedTransactionCommit(
        opCtx(), txnParticipant.retrieveCompletedTransactionOperations(opCtx()));
    auto oplogEntry = getSingleOplogEntry(opCtx());
    checkCommonFields(oplogEntry);
    auto o = oplogEntry.getObjectField("o");
    auto oExpected = BSON("applyOps" << BSON_ARRAY(BSON("op"
                                                        << "u"
                                                        << "ns"
                                                        << nss1.toString()
                                                        << "ui"
                                                        << uuid1
                                                        << "o"
                                                        << BSON("$set" << BSON("data"
                                                                               << "x"))
                                                        << "o2"
                                                        << BSON("_id" << 0))
                                                   << BSON("op"
                                                           << "u"
                                                           << "ns"
                                                           << nss2.toString()
                                                           << "ui"
                                                           << uuid2
                                                           << "o"
                                                           << BSON("$set" << BSON("data"
                                                                                  << "y"))
                                                           << "o2"
                                                           << BSON("_id" << 1))));
    ASSERT_BSONOBJ_EQ(oExpected, o);
    ASSERT_FALSE(oplogEntry.hasField("prepare"));
    ASSERT_FALSE(oplogEntry.getBoolField("prepare"));
}

TEST_F(OpObserverTransactionTest, TransactionalDeleteTest) {
    const NamespaceString nss1("testDB", "testColl");
    const NamespaceString nss2("testDB2", "testColl2");
    auto uuid1 = CollectionUUID::gen();
    auto uuid2 = CollectionUUID::gen();

    auto txnParticipant = TransactionParticipant::get(opCtx());
    txnParticipant.unstashTransactionResources(opCtx(), "delete");

    WriteUnitOfWork wuow(opCtx());
    AutoGetCollection autoColl1(opCtx(), nss1, MODE_IX);
    AutoGetCollection autoColl2(opCtx(), nss2, MODE_IX);
    opObserver().aboutToDelete(opCtx(),
                               nss1,
                               BSON("_id" << 0 << "data"
                                          << "x"));
    opObserver().onDelete(opCtx(), nss1, uuid1, 0, false, boost::none);
    opObserver().aboutToDelete(opCtx(),
                               nss2,
                               BSON("_id" << 1 << "data"
                                          << "y"));
    opObserver().onDelete(opCtx(), nss2, uuid2, 0, false, boost::none);
    opObserver().onUnpreparedTransactionCommit(
        opCtx(), txnParticipant.retrieveCompletedTransactionOperations(opCtx()));
    auto oplogEntry = getSingleOplogEntry(opCtx());
    checkCommonFields(oplogEntry);
    auto o = oplogEntry.getObjectField("o");
    auto oExpected = BSON("applyOps" << BSON_ARRAY(BSON("op"
                                                        << "d"
                                                        << "ns"
                                                        << nss1.toString()
                                                        << "ui"
                                                        << uuid1
                                                        << "o"
                                                        << BSON("_id" << 0))
                                                   << BSON("op"
                                                           << "d"
                                                           << "ns"
                                                           << nss2.toString()
                                                           << "ui"
                                                           << uuid2
                                                           << "o"
                                                           << BSON("_id" << 1))));
    ASSERT_BSONOBJ_EQ(oExpected, o);
    ASSERT_FALSE(oplogEntry.hasField("prepare"));
    ASSERT_FALSE(oplogEntry.getBoolField("prepare"));
}

class OpObserverMultiEntryTransactionTest : public OpObserverTransactionTest {
    void setUp() override {
        gUseMultipleOplogEntryFormatForTransactions = true;
        OpObserverTransactionTest::setUp();
    }

    void tearDown() override {
        OpObserverTransactionTest::tearDown();
        gUseMultipleOplogEntryFormatForTransactions = false;
    }
};

TEST_F(OpObserverMultiEntryTransactionTest,
       CommittingUnpreparedEmptyTransactionDoesNotWriteToTransactionTableOrOplog) {
    auto txnParticipant = TransactionParticipant::get(opCtx());
    txnParticipant.unstashTransactionResources(opCtx(), "prepareTransaction");

    opObserver().onUnpreparedTransactionCommit(
        opCtx(), txnParticipant.retrieveCompletedTransactionOperations(opCtx()));

    txnParticipant.stashTransactionResources(opCtx());

    getNOplogEntries(opCtx(), 0);

    // Abort the storage-transaction without calling the OpObserver.
    txnParticipant.shutdown(opCtx());

    assertNoTxnRecord();
}

TEST_F(OpObserverMultiEntryTransactionTest, TransactionalInsertTest) {
    const NamespaceString nss1("testDB", "testColl");
    const NamespaceString nss2("testDB2", "testColl2");
    auto uuid1 = CollectionUUID::gen();
    auto uuid2 = CollectionUUID::gen();
    auto txnParticipant = TransactionParticipant::get(opCtx());
    txnParticipant.unstashTransactionResources(opCtx(), "insert");

    std::vector<InsertStatement> inserts1;
    inserts1.emplace_back(0,
                          BSON("_id" << 0 << "data"
                                     << "x"));
    inserts1.emplace_back(1,
                          BSON("_id" << 1 << "data"
                                     << "y"));
    std::vector<InsertStatement> inserts2;
    inserts2.emplace_back(0,
                          BSON("_id" << 2 << "data"
                                     << "z"));
    inserts2.emplace_back(1,
                          BSON("_id" << 3 << "data"
                                     << "w"));
    WriteUnitOfWork wuow(opCtx());
    AutoGetCollection autoColl1(opCtx(), nss1, MODE_IX);
    AutoGetCollection autoColl2(opCtx(), nss2, MODE_IX);
    opObserver().onInserts(opCtx(), nss1, uuid1, inserts1.begin(), inserts1.end(), false);
    opObserver().onInserts(opCtx(), nss2, uuid2, inserts2.begin(), inserts2.end(), false);
    opObserver().onUnpreparedTransactionCommit(
        opCtx(), txnParticipant.retrieveCompletedTransactionOperations(opCtx()));
    auto oplogEntryObjs = getNOplogEntries(opCtx(), 5);
    StmtId expectedStmtId = 0;
    std::vector<OplogEntry> oplogEntries;
    mongo::repl::OpTime expectedPrevWriteOpTime;
    for (const auto& oplogEntryObj : oplogEntryObjs) {
        checkSessionAndTransactionFields(oplogEntryObj, expectedStmtId);
        oplogEntries.push_back(assertGet(OplogEntry::parse(oplogEntryObj)));
        const auto& oplogEntry = oplogEntries.back();
        if (expectedStmtId++ < 4) {
            ASSERT_EQ("i", oplogEntryObj["op"].String());
            ASSERT(oplogEntry.getInTxn());
        } else {
            ASSERT_EQ("admin.$cmd"_sd, oplogEntryObj["ns"].String());
            ASSERT_EQ("c", oplogEntryObj["op"].String());
        }
        ASSERT(!oplogEntry.getPrepare());
        ASSERT_FALSE(oplogEntryObj.hasField("prepare"));
        ASSERT_TRUE(oplogEntry.getPrevWriteOpTimeInTransaction());
        ASSERT_EQ(expectedPrevWriteOpTime, *oplogEntry.getPrevWriteOpTimeInTransaction());
        ASSERT_LT(expectedPrevWriteOpTime.getTimestamp(), oplogEntry.getTimestamp());
        expectedPrevWriteOpTime = repl::OpTime{oplogEntry.getTimestamp(), *oplogEntry.getTerm()};
    }
    ASSERT_EQ(nss1, oplogEntries[0].getNss());
    ASSERT_EQ(uuid1, *oplogEntries[0].getUuid());
    ASSERT_BSONOBJ_EQ(BSON("_id" << 0 << "data"
                                 << "x"),
                      oplogEntries[0].getObject());
    ASSERT_FALSE(oplogEntries[0].getObject2());

    ASSERT_EQ(nss1, oplogEntries[1].getNss());
    ASSERT_EQ(uuid1, *oplogEntries[1].getUuid());
    ASSERT_BSONOBJ_EQ(BSON("_id" << 1 << "data"
                                 << "y"),
                      oplogEntries[1].getObject());
    ASSERT_FALSE(oplogEntries[1].getObject2());

    ASSERT_EQ(nss2, oplogEntries[2].getNss());
    ASSERT_EQ(uuid2, *oplogEntries[2].getUuid());
    ASSERT_BSONOBJ_EQ(BSON("_id" << 2 << "data"
                                 << "z"),
                      oplogEntries[2].getObject());
    ASSERT_FALSE(oplogEntries[2].getObject2());

    ASSERT_EQ(nss2, oplogEntries[3].getNss());
    ASSERT_EQ(uuid2, *oplogEntries[3].getUuid());
    ASSERT_BSONOBJ_EQ(BSON("_id" << 3 << "data"
                                 << "w"),
                      oplogEntries[3].getObject());
    ASSERT_FALSE(oplogEntries[3].getObject2());
    ASSERT_BSONOBJ_EQ(BSON("commitTransaction" << 1 << "prepare" << false),
                      oplogEntries[4].getObject());
}

TEST_F(OpObserverMultiEntryTransactionTest, TransactionalUpdateTest) {
    const NamespaceString nss1("testDB", "testColl");
    const NamespaceString nss2("testDB2", "testColl2");
    auto uuid1 = CollectionUUID::gen();
    auto uuid2 = CollectionUUID::gen();
    auto txnParticipant = TransactionParticipant::get(opCtx());
    txnParticipant.unstashTransactionResources(opCtx(), "update");

    CollectionUpdateArgs updateArgs1;
    updateArgs1.stmtId = 0;
    updateArgs1.updatedDoc = BSON("_id" << 0 << "data"
                                        << "x");
    updateArgs1.update = BSON("$set" << BSON("data"
                                             << "x"));
    updateArgs1.criteria = BSON("_id" << 0);
    OplogUpdateEntryArgs update1(std::move(updateArgs1), nss1, uuid1);

    CollectionUpdateArgs updateArgs2;
    updateArgs2.stmtId = 1;
    updateArgs2.updatedDoc = BSON("_id" << 1 << "data"
                                        << "y");
    updateArgs2.update = BSON("$set" << BSON("data"
                                             << "y"));
    updateArgs2.criteria = BSON("_id" << 1);
    OplogUpdateEntryArgs update2(std::move(updateArgs2), nss2, uuid2);

    WriteUnitOfWork wuow(opCtx());
    AutoGetCollection autoColl1(opCtx(), nss1, MODE_IX);
    AutoGetCollection autoColl2(opCtx(), nss2, MODE_IX);
    opObserver().onUpdate(opCtx(), update1);
    opObserver().onUpdate(opCtx(), update2);
    opObserver().onUnpreparedTransactionCommit(
        opCtx(), txnParticipant.retrieveCompletedTransactionOperations(opCtx()));
    auto oplogEntryObjs = getNOplogEntries(opCtx(), 3);
    StmtId expectedStmtId = 0;
    std::vector<OplogEntry> oplogEntries;
    mongo::repl::OpTime expectedPrevWriteOpTime;
    for (const auto& oplogEntryObj : oplogEntryObjs) {
        checkSessionAndTransactionFields(oplogEntryObj, expectedStmtId);
        oplogEntries.push_back(assertGet(OplogEntry::parse(oplogEntryObj)));
        const auto& oplogEntry = oplogEntries.back();
        if (expectedStmtId++ < 2) {
            ASSERT_EQ("u", oplogEntryObj["op"].String());
            ASSERT(oplogEntry.getInTxn());
        } else {
            ASSERT_EQ("admin.$cmd"_sd, oplogEntryObj["ns"].String());
            ASSERT_EQ("c", oplogEntryObj["op"].String());
        }
        ASSERT(!oplogEntry.getPrepare());
        ASSERT_FALSE(oplogEntryObj.hasField("prepare"));
        ASSERT_TRUE(oplogEntry.getPrevWriteOpTimeInTransaction());
        ASSERT_EQ(expectedPrevWriteOpTime, *oplogEntry.getPrevWriteOpTimeInTransaction());
        ASSERT_LT(expectedPrevWriteOpTime.getTimestamp(), oplogEntry.getTimestamp());
        expectedPrevWriteOpTime = repl::OpTime{oplogEntry.getTimestamp(), *oplogEntry.getTerm()};
    }
    ASSERT_EQ(nss1, oplogEntries[0].getNss());
    ASSERT_EQ(uuid1, *oplogEntries[0].getUuid());
    ASSERT_BSONOBJ_EQ(BSON("$set" << BSON("data"
                                          << "x")),
                      oplogEntries[0].getObject());
    ASSERT_TRUE(oplogEntries[0].getObject2());
    ASSERT_BSONOBJ_EQ(*oplogEntries[0].getObject2(), BSON("_id" << 0));

    ASSERT_EQ(nss2, oplogEntries[1].getNss());
    ASSERT_EQ(uuid2, *oplogEntries[1].getUuid());
    ASSERT_BSONOBJ_EQ(BSON("$set" << BSON("data"
                                          << "y")),
                      oplogEntries[1].getObject());
    ASSERT_TRUE(oplogEntries[1].getObject2());
    ASSERT_BSONOBJ_EQ(*oplogEntries[1].getObject2(), BSON("_id" << 1));
    ASSERT_BSONOBJ_EQ(BSON("commitTransaction" << 1 << "prepare" << false),
                      oplogEntries[2].getObject());
}

TEST_F(OpObserverMultiEntryTransactionTest, TransactionalDeleteTest) {
    const NamespaceString nss1("testDB", "testColl");
    const NamespaceString nss2("testDB2", "testColl2");
    auto uuid1 = CollectionUUID::gen();
    auto uuid2 = CollectionUUID::gen();

    auto txnParticipant = TransactionParticipant::get(opCtx());
    txnParticipant.unstashTransactionResources(opCtx(), "delete");

    WriteUnitOfWork wuow(opCtx());
    AutoGetCollection autoColl1(opCtx(), nss1, MODE_IX);
    AutoGetCollection autoColl2(opCtx(), nss2, MODE_IX);
    opObserver().aboutToDelete(opCtx(),
                               nss1,
                               BSON("_id" << 0 << "data"
                                          << "x"));
    opObserver().onDelete(opCtx(), nss1, uuid1, 0, false, boost::none);
    opObserver().aboutToDelete(opCtx(),
                               nss2,
                               BSON("_id" << 1 << "data"
                                          << "y"));
    opObserver().onDelete(opCtx(), nss2, uuid2, 0, false, boost::none);
    opObserver().onUnpreparedTransactionCommit(
        opCtx(), txnParticipant.retrieveCompletedTransactionOperations(opCtx()));
    auto oplogEntryObjs = getNOplogEntries(opCtx(), 3);
    StmtId expectedStmtId = 0;
    std::vector<OplogEntry> oplogEntries;
    mongo::repl::OpTime expectedPrevWriteOpTime;
    for (const auto& oplogEntryObj : oplogEntryObjs) {
        checkSessionAndTransactionFields(oplogEntryObj, expectedStmtId);
        oplogEntries.push_back(assertGet(OplogEntry::parse(oplogEntryObj)));
        const auto& oplogEntry = oplogEntries.back();
        if (expectedStmtId++ < 2) {
            ASSERT_EQ("d", oplogEntryObj["op"].String());
            ASSERT(oplogEntry.getInTxn());
        } else {
            ASSERT_EQ("admin.$cmd"_sd, oplogEntryObj["ns"].String());
            ASSERT_EQ("c", oplogEntryObj["op"].String());
        }
        ASSERT(!oplogEntry.getPrepare());
        ASSERT_FALSE(oplogEntryObj.hasField("prepare"));
        ASSERT_TRUE(oplogEntry.getPrevWriteOpTimeInTransaction());
        ASSERT_EQ(expectedPrevWriteOpTime, *oplogEntry.getPrevWriteOpTimeInTransaction());
        ASSERT_LT(expectedPrevWriteOpTime.getTimestamp(), oplogEntry.getTimestamp());
        expectedPrevWriteOpTime = repl::OpTime{oplogEntry.getTimestamp(), *oplogEntry.getTerm()};
    }
    ASSERT_EQ(nss1, oplogEntries[0].getNss());
    ASSERT_EQ(uuid1, *oplogEntries[0].getUuid());
    ASSERT_BSONOBJ_EQ(oplogEntries[0].getObject(), BSON("_id" << 0));
    ASSERT_FALSE(oplogEntries[0].getObject2());

    ASSERT_EQ(nss2, oplogEntries[1].getNss());
    ASSERT_EQ(uuid2, *oplogEntries[1].getUuid());
    ASSERT_BSONOBJ_EQ(oplogEntries[1].getObject(), BSON("_id" << 1));
    ASSERT_FALSE(oplogEntries[1].getObject2());
    ASSERT_BSONOBJ_EQ(BSON("commitTransaction" << 1 << "prepare" << false),
                      oplogEntries[2].getObject());
}


TEST_F(OpObserverMultiEntryTransactionTest,
       PreparingEmptyTransactionOnlyWritesPrepareOplogEntryAndToTransactionTable) {
    auto txnParticipant = TransactionParticipant::get(opCtx());
    txnParticipant.unstashTransactionResources(opCtx(), "prepareTransaction");
    repl::OpTime prepareOpTime;
    auto reservedSlots = repl::getNextOpTimes(opCtx(), 1);
    prepareOpTime = reservedSlots.back().opTime;
    opCtx()->recoveryUnit()->setPrepareTimestamp(prepareOpTime.getTimestamp());
    opObserver().onTransactionPrepare(
        opCtx(), reservedSlots, txnParticipant.retrieveCompletedTransactionOperations(opCtx()));

    auto oplogEntryObjs = getNOplogEntries(opCtx(), 1);
    auto prepareEntryObj = oplogEntryObjs.back();
    const auto prepareOplogEntry = assertGet(OplogEntry::parse(prepareEntryObj));
    checkSessionAndTransactionFields(prepareEntryObj, 0);

    ASSERT_EQ(prepareOpTime.getTimestamp(), opCtx()->recoveryUnit()->getPrepareTimestamp());
    ASSERT_BSONOBJ_EQ(BSON("prepareTransaction" << 1), prepareOplogEntry.getObject());
    txnParticipant.stashTransactionResources(opCtx());
    assertTxnRecord(txnNum(), prepareOpTime, DurableTxnStateEnum::kPrepared);
    txnParticipant.unstashTransactionResources(opCtx(), "abortTransaction");

    ASSERT_EQ(prepareOpTime, txnParticipant.getLastWriteOpTime());
}

TEST_F(OpObserverMultiEntryTransactionTest, TransactionalInsertPrepareTest) {
    const NamespaceString nss1("testDB", "testColl");
    const NamespaceString nss2("testDB2", "testColl2");
    auto uuid1 = CollectionUUID::gen();
    auto uuid2 = CollectionUUID::gen();
    auto txnParticipant = TransactionParticipant::get(opCtx());
    txnParticipant.unstashTransactionResources(opCtx(), "insert");

    AutoGetCollection autoColl1(opCtx(), nss1, MODE_IX);
    AutoGetCollection autoColl2(opCtx(), nss2, MODE_IX);

    std::vector<InsertStatement> inserts1;
    inserts1.emplace_back(0, BSON("_id" << 0));
    inserts1.emplace_back(1, BSON("_id" << 1));
    std::vector<InsertStatement> inserts2;
    inserts2.emplace_back(0, BSON("_id" << 2));
    inserts2.emplace_back(1, BSON("_id" << 3));

    opObserver().onInserts(opCtx(), nss1, uuid1, inserts1.begin(), inserts1.end(), false);
    opObserver().onInserts(opCtx(), nss2, uuid2, inserts2.begin(), inserts2.end(), false);

    repl::OpTime prepareOpTime;
    auto reservedSlots = repl::getNextOpTimes(opCtx(), 5);
    prepareOpTime = reservedSlots.back().opTime;
    txnParticipant.transitionToPreparedforTest(opCtx(), prepareOpTime);
    opCtx()->recoveryUnit()->setPrepareTimestamp(prepareOpTime.getTimestamp());
    opObserver().onTransactionPrepare(
        opCtx(), reservedSlots, txnParticipant.retrieveCompletedTransactionOperations(opCtx()));
    auto oplogEntryObjs = getNOplogEntries(opCtx(), 5);
    StmtId expectedStmtId = 0;
    std::vector<OplogEntry> oplogEntries;
    mongo::repl::OpTime expectedPrevWriteOpTime;
    for (const auto& oplogEntryObj : oplogEntryObjs) {
        checkSessionAndTransactionFields(oplogEntryObj, expectedStmtId);
        oplogEntries.push_back(assertGet(OplogEntry::parse(oplogEntryObj)));
        const auto& oplogEntry = oplogEntries.back();
        if (expectedStmtId++ < 4) {
            ASSERT_TRUE(oplogEntry.isCrudOpType());
            ASSERT(oplogEntry.getOpType() == repl::OpTypeEnum::kInsert);
            ASSERT_TRUE(oplogEntry.getInTxn());
        } else {
            ASSERT_EQ("admin.$cmd"_sd, oplogEntry.getNss().toString());
            ASSERT_TRUE(oplogEntry.isCommand());
            ASSERT_TRUE(OplogEntry::CommandType::kPrepareTransaction ==
                        oplogEntry.getCommandType());
        }
        ASSERT(!oplogEntry.getPrepare());
        ASSERT_TRUE(oplogEntry.getPrevWriteOpTimeInTransaction());
        ASSERT_EQ(expectedPrevWriteOpTime, *oplogEntry.getPrevWriteOpTimeInTransaction());
        ASSERT_LT(expectedPrevWriteOpTime.getTimestamp(), oplogEntry.getTimestamp());
        expectedPrevWriteOpTime = repl::OpTime{oplogEntry.getTimestamp(), *oplogEntry.getTerm()};
    }
    ASSERT_EQ(nss1, oplogEntries[0].getNss());
    ASSERT_EQ(uuid1, *oplogEntries[0].getUuid());
    ASSERT_BSONOBJ_EQ(BSON("_id" << 0), oplogEntries[0].getObject());
    ASSERT_FALSE(oplogEntries[0].getObject2());

    ASSERT_EQ(nss1, oplogEntries[1].getNss());
    ASSERT_EQ(uuid1, *oplogEntries[1].getUuid());
    ASSERT_BSONOBJ_EQ(BSON("_id" << 1), oplogEntries[1].getObject());
    ASSERT_FALSE(oplogEntries[1].getObject2());

    ASSERT_EQ(nss2, oplogEntries[2].getNss());
    ASSERT_EQ(uuid2, *oplogEntries[2].getUuid());
    ASSERT_BSONOBJ_EQ(BSON("_id" << 2), oplogEntries[2].getObject());
    ASSERT_FALSE(oplogEntries[2].getObject2());

    ASSERT_EQ(nss2, oplogEntries[3].getNss());
    ASSERT_EQ(uuid2, *oplogEntries[3].getUuid());
    ASSERT_BSONOBJ_EQ(BSON("_id" << 3), oplogEntries[3].getObject());
    ASSERT_FALSE(oplogEntries[3].getObject2());

    ASSERT_EQ(prepareOpTime.getTimestamp(), opCtx()->recoveryUnit()->getPrepareTimestamp());
    ASSERT_BSONOBJ_EQ(BSON("prepareTransaction" << 1), oplogEntries[4].getObject());
    ASSERT_FALSE(oplogEntries[4].getObject2());

    ASSERT_EQ(prepareOpTime, txnParticipant.getLastWriteOpTime());
    txnParticipant.stashTransactionResources(opCtx());
    assertTxnRecord(txnNum(), prepareOpTime, DurableTxnStateEnum::kPrepared);
    txnParticipant.unstashTransactionResources(opCtx(), "abortTransaction");
}

TEST_F(OpObserverMultiEntryTransactionTest, TransactionalUpdatePrepareTest) {
    const NamespaceString nss1("testDB", "testColl");
    const NamespaceString nss2("testDB2", "testColl2");
    auto uuid1 = CollectionUUID::gen();
    auto uuid2 = CollectionUUID::gen();
    auto txnParticipant = TransactionParticipant::get(opCtx());
    txnParticipant.unstashTransactionResources(opCtx(), "update");

    CollectionUpdateArgs updateArgs1;
    updateArgs1.stmtId = 0;
    updateArgs1.updatedDoc = BSON("_id" << 0 << "data"
                                        << "x");
    updateArgs1.update = BSON("$set" << BSON("data"
                                             << "x"));
    updateArgs1.criteria = BSON("_id" << 0);
    OplogUpdateEntryArgs update1(std::move(updateArgs1), nss1, uuid1);

    CollectionUpdateArgs updateArgs2;
    updateArgs2.stmtId = 1;
    updateArgs2.updatedDoc = BSON("_id" << 1 << "data"
                                        << "y");
    updateArgs2.update = BSON("$set" << BSON("data"
                                             << "y"));
    updateArgs2.criteria = BSON("_id" << 1);
    OplogUpdateEntryArgs update2(std::move(updateArgs2), nss2, uuid2);

    AutoGetCollection autoColl1(opCtx(), nss1, MODE_IX);
    AutoGetCollection autoColl2(opCtx(), nss2, MODE_IX);
    opObserver().onUpdate(opCtx(), update1);
    opObserver().onUpdate(opCtx(), update2);

    repl::OpTime prepareOpTime;
    auto reservedSlots = repl::getNextOpTimes(opCtx(), 3);
    prepareOpTime = reservedSlots.back().opTime;
    txnParticipant.transitionToPreparedforTest(opCtx(), prepareOpTime);
    opCtx()->recoveryUnit()->setPrepareTimestamp(prepareOpTime.getTimestamp());
    opObserver().onTransactionPrepare(
        opCtx(), reservedSlots, txnParticipant.retrieveCompletedTransactionOperations(opCtx()));

    auto oplogEntryObjs = getNOplogEntries(opCtx(), 3);
    StmtId expectedStmtId = 0;
    std::vector<OplogEntry> oplogEntries;
    mongo::repl::OpTime expectedPrevWriteOpTime;
    for (const auto& oplogEntryObj : oplogEntryObjs) {
        checkSessionAndTransactionFields(oplogEntryObj, expectedStmtId);
        oplogEntries.push_back(assertGet(OplogEntry::parse(oplogEntryObj)));
        const auto& oplogEntry = oplogEntries.back();
        if (expectedStmtId++ < 2) {
            ASSERT_TRUE(oplogEntry.isCrudOpType());
            ASSERT(oplogEntry.getOpType() == repl::OpTypeEnum::kUpdate);
            ASSERT_TRUE(oplogEntry.getInTxn());
        } else {
            ASSERT_EQ("admin.$cmd"_sd, oplogEntry.getNss().toString());
            ASSERT_TRUE(oplogEntry.isCommand());
            ASSERT_TRUE(OplogEntry::CommandType::kPrepareTransaction ==
                        oplogEntry.getCommandType());
        }
        ASSERT(!oplogEntry.getPrepare());
        ASSERT_TRUE(oplogEntry.getPrevWriteOpTimeInTransaction());
        ASSERT_EQ(expectedPrevWriteOpTime, *oplogEntry.getPrevWriteOpTimeInTransaction());
        ASSERT_LT(expectedPrevWriteOpTime.getTimestamp(), oplogEntry.getTimestamp());
        expectedPrevWriteOpTime = repl::OpTime{oplogEntry.getTimestamp(), *oplogEntry.getTerm()};
    }
    ASSERT_EQ(nss1, oplogEntries[0].getNss());
    ASSERT_EQ(uuid1, *oplogEntries[0].getUuid());
    ASSERT_BSONOBJ_EQ(BSON("$set" << BSON("data"
                                          << "x")),
                      oplogEntries[0].getObject());
    ASSERT_TRUE(oplogEntries[0].getObject2());
    ASSERT_BSONOBJ_EQ(*oplogEntries[0].getObject2(), BSON("_id" << 0));

    ASSERT_EQ(nss2, oplogEntries[1].getNss());
    ASSERT_EQ(uuid2, *oplogEntries[1].getUuid());
    ASSERT_BSONOBJ_EQ(BSON("$set" << BSON("data"
                                          << "y")),
                      oplogEntries[1].getObject());
    ASSERT_TRUE(oplogEntries[1].getObject2());
    ASSERT_BSONOBJ_EQ(*oplogEntries[1].getObject2(), BSON("_id" << 1));

    ASSERT_EQ(prepareOpTime.getTimestamp(), opCtx()->recoveryUnit()->getPrepareTimestamp());
    ASSERT_BSONOBJ_EQ(BSON("prepareTransaction" << 1), oplogEntries[2].getObject());
    ASSERT_FALSE(oplogEntries[2].getObject2());

    ASSERT_EQ(prepareOpTime, txnParticipant.getLastWriteOpTime());

    txnParticipant.stashTransactionResources(opCtx());
    assertTxnRecord(txnNum(), prepareOpTime, DurableTxnStateEnum::kPrepared);
    txnParticipant.unstashTransactionResources(opCtx(), "abortTransaction");
}

TEST_F(OpObserverMultiEntryTransactionTest, TransactionalDeletePrepareTest) {
    const NamespaceString nss1("testDB", "testColl");
    const NamespaceString nss2("testDB2", "testColl2");
    auto uuid1 = CollectionUUID::gen();
    auto uuid2 = CollectionUUID::gen();

    auto txnParticipant = TransactionParticipant::get(opCtx());
    txnParticipant.unstashTransactionResources(opCtx(), "delete");

    AutoGetCollection autoColl1(opCtx(), nss1, MODE_IX);
    AutoGetCollection autoColl2(opCtx(), nss2, MODE_IX);
    opObserver().aboutToDelete(opCtx(),
                               nss1,
                               BSON("_id" << 0 << "data"
                                          << "x"));
    opObserver().onDelete(opCtx(), nss1, uuid1, 0, false, boost::none);
    opObserver().aboutToDelete(opCtx(),
                               nss2,
                               BSON("_id" << 1 << "data"
                                          << "y"));
    opObserver().onDelete(opCtx(), nss2, uuid2, 0, false, boost::none);

    repl::OpTime prepareOpTime;
    auto reservedSlots = repl::getNextOpTimes(opCtx(), 3);
    prepareOpTime = reservedSlots.back().opTime;
    txnParticipant.transitionToPreparedforTest(opCtx(), prepareOpTime);
    prepareOpTime = reservedSlots.back().opTime;
    opCtx()->recoveryUnit()->setPrepareTimestamp(prepareOpTime.getTimestamp());
    opObserver().onTransactionPrepare(
        opCtx(), reservedSlots, txnParticipant.retrieveCompletedTransactionOperations(opCtx()));

    auto oplogEntryObjs = getNOplogEntries(opCtx(), 3);
    StmtId expectedStmtId = 0;
    std::vector<OplogEntry> oplogEntries;
    mongo::repl::OpTime expectedPrevWriteOpTime;
    for (const auto& oplogEntryObj : oplogEntryObjs) {
        checkSessionAndTransactionFields(oplogEntryObj, expectedStmtId);
        oplogEntries.push_back(assertGet(OplogEntry::parse(oplogEntryObj)));
        const auto& oplogEntry = oplogEntries.back();
        if (expectedStmtId++ < 2) {
            ASSERT_TRUE(oplogEntry.isCrudOpType());
            ASSERT(oplogEntry.getOpType() == repl::OpTypeEnum::kDelete);
            ASSERT_TRUE(oplogEntry.getInTxn());
        } else {
            ASSERT_EQ("admin.$cmd"_sd, oplogEntry.getNss().toString());
            ASSERT_TRUE(oplogEntry.isCommand());
            ASSERT_TRUE(OplogEntry::CommandType::kPrepareTransaction ==
                        oplogEntry.getCommandType());
        }
        ASSERT(!oplogEntry.getPrepare());
        ASSERT_TRUE(oplogEntry.getPrevWriteOpTimeInTransaction());
        ASSERT_EQ(expectedPrevWriteOpTime, *oplogEntry.getPrevWriteOpTimeInTransaction());
        ASSERT_LT(expectedPrevWriteOpTime.getTimestamp(), oplogEntry.getTimestamp());
        expectedPrevWriteOpTime = repl::OpTime{oplogEntry.getTimestamp(), *oplogEntry.getTerm()};
    }
    ASSERT_EQ(nss1, oplogEntries[0].getNss());
    ASSERT_EQ(uuid1, *oplogEntries[0].getUuid());
    ASSERT_BSONOBJ_EQ(oplogEntries[0].getObject(), BSON("_id" << 0));
    ASSERT_FALSE(oplogEntries[0].getObject2());

    ASSERT_EQ(nss2, oplogEntries[1].getNss());
    ASSERT_EQ(uuid2, *oplogEntries[1].getUuid());
    ASSERT_BSONOBJ_EQ(oplogEntries[1].getObject(), BSON("_id" << 1));
    ASSERT_FALSE(oplogEntries[1].getObject2());

    ASSERT_EQ(prepareOpTime.getTimestamp(), opCtx()->recoveryUnit()->getPrepareTimestamp());
    ASSERT_BSONOBJ_EQ(BSON("prepareTransaction" << 1), oplogEntries[2].getObject());
    ASSERT_FALSE(oplogEntries[2].getObject2());

    ASSERT_EQ(prepareOpTime, txnParticipant.getLastWriteOpTime());
    txnParticipant.stashTransactionResources(opCtx());
    assertTxnRecord(txnNum(), prepareOpTime, DurableTxnStateEnum::kPrepared);
    txnParticipant.unstashTransactionResources(opCtx(), "abortTransaction");
}
}  // namespace
}  // namespace mongo
