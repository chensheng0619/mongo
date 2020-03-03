/**
 *    Copyright (C) 2020-present MongoDB, Inc.
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

#include "mongo/db/exec/document_value/document_value_test_util.h"
#include "mongo/db/pipeline/document_source_group.h"
#include "mongo/db/pipeline/document_source_match.h"
#include "mongo/db/pipeline/document_source_queue.h"
#include "mongo/db/pipeline/document_source_union_with.h"
#include "mongo/db/pipeline/process_interface/shardsvr_process_interface.h"
#include "mongo/db/views/resolved_view.h"
#include "mongo/s/query/sharded_agg_test_fixture.h"
#include "mongo/s/stale_exception.h"
#include "mongo/unittest/unittest.h"

namespace mongo {
namespace {

// Use this new name to register these tests under their own unit test suite.
using ShardedUnionTest = ShardedAggTestFixture;

TEST_F(ShardedUnionTest, RetriesSubPipelineOnNetworkError) {
    // Sharded by {_id: 1}, [MinKey, 0) on shard "0", [0, MaxKey) on shard "1".
    setupNShards(2);
    loadRoutingTableWithTwoChunksAndTwoShards(kTestAggregateNss);

    auto pipeline = Pipeline::create(
        {DocumentSourceMatch::create(fromjson("{_id: 'unionResult'}"), expCtx())}, expCtx());
    auto unionWith = DocumentSourceUnionWith(expCtx(), std::move(pipeline));
    expCtx()->mongoProcessInterface = std::make_shared<ShardServerProcessInterface>(executor());
    auto queue = DocumentSourceQueue::create(expCtx());
    unionWith.setSource(queue.get());

    auto expectedResult = Document{{"_id"_sd, "unionResult"_sd}};

    auto future = launchAsync([&] {
        auto next = unionWith.getNext();
        ASSERT_TRUE(next.isAdvanced());
        auto result = next.releaseDocument();
        ASSERT_DOCUMENT_EQ(result, expectedResult);
        ASSERT(unionWith.getNext().isEOF());
        ASSERT(unionWith.getNext().isEOF());
        ASSERT(unionWith.getNext().isEOF());
    });

    onCommand([&](const executor::RemoteCommandRequest& request) {
        return Status{ErrorCodes::NetworkTimeout, "Mock error: network timed out"};
    });

    // Now schedule the response with the expected result.
    onCommand([&](const executor::RemoteCommandRequest& request) {
        return CursorResponse(kTestAggregateNss, CursorId{0}, {expectedResult.toBson()})
            .toBSON(CursorResponse::ResponseType::InitialResponse);
    });

    future.default_timed_get();
}

TEST_F(ShardedUnionTest, ForwardsMaxTimeMSToRemotes) {
    // Sharded by {_id: 1}, [MinKey, 0) on shard "0", [0, MaxKey) on shard "1".
    setupNShards(2);
    loadRoutingTableWithTwoChunksAndTwoShards(kTestAggregateNss);

    auto pipeline = Pipeline::create({}, expCtx());
    auto unionWith = DocumentSourceUnionWith(expCtx(), std::move(pipeline));
    expCtx()->mongoProcessInterface = std::make_shared<ShardServerProcessInterface>(executor());
    auto queue = DocumentSourceQueue::create(expCtx());
    unionWith.setSource(queue.get());

    auto expectedResult = Document{{"_id"_sd, BSONNULL}, {"count"_sd, 1}};

    expCtx()->opCtx->setDeadlineAfterNowBy(Milliseconds(15), ErrorCodes::MaxTimeMSExpired);

    auto future = launchAsync([&] {
        auto next = unionWith.getNext();
        ASSERT_TRUE(next.isAdvanced());
        auto result = next.releaseDocument();
        ASSERT_DOCUMENT_EQ(result, expectedResult);
        ASSERT(unionWith.getNext().isEOF());
        ASSERT(unionWith.getNext().isEOF());
        ASSERT(unionWith.getNext().isEOF());
    });

    const auto assertHasExpectedMaxTimeMSAndReturnResult =
        [&](const executor::RemoteCommandRequest& request) {
            ASSERT(request.cmdObj.hasField("maxTimeMS")) << request;
            ASSERT(request.cmdObj["maxTimeMS"].isNumber());
            return CursorResponse(kTestAggregateNss, CursorId{0}, {expectedResult.toBson()})
                .toBSON(CursorResponse::ResponseType::InitialResponse);
        };

    onCommand(assertHasExpectedMaxTimeMSAndReturnResult);
    onCommand(assertHasExpectedMaxTimeMSAndReturnResult);
}

TEST_F(ShardedUnionTest, RetriesSubPipelineOnStaleConfigError) {
    // Sharded by {_id: 1}, [MinKey, 0) on shard "0", [0, MaxKey) on shard "1".
    setupNShards(2);
    loadRoutingTableWithTwoChunksAndTwoShards(kTestAggregateNss);

    auto pipeline = Pipeline::create(
        {DocumentSourceMatch::create(fromjson("{_id: 'unionResult'}"), expCtx())}, expCtx());
    auto unionWith = DocumentSourceUnionWith(expCtx(), std::move(pipeline));
    expCtx()->mongoProcessInterface = std::make_shared<ShardServerProcessInterface>(executor());
    auto queue = DocumentSourceQueue::create(expCtx());
    unionWith.setSource(queue.get());

    auto expectedResult = Document{{"_id"_sd, "unionResult"_sd}};

    auto future = launchAsync([&] {
        auto next = unionWith.getNext();
        ASSERT_TRUE(next.isAdvanced());
        auto result = next.releaseDocument();
        ASSERT_DOCUMENT_EQ(result, expectedResult);
        ASSERT(unionWith.getNext().isEOF());
        ASSERT(unionWith.getNext().isEOF());
        ASSERT(unionWith.getNext().isEOF());
    });

    // Mock out one error response, then expect a refresh of the sharding catalog for that
    // namespace, then mock out a successful response.
    onCommand([&](const executor::RemoteCommandRequest& request) {
        return Status{ErrorCodes::StaleShardVersion, "Mock error: shard version mismatch"};
    });

    // Mock the expected config server queries.
    const OID epoch = OID::gen();
    const ShardKeyPattern shardKeyPattern(BSON("_id" << 1));
    expectGetCollection(kTestAggregateNss, epoch, shardKeyPattern);
    expectFindSendBSONObjVector(kConfigHostAndPort, [&]() {
        ChunkVersion version(1, 0, epoch);

        ChunkType chunk1(kTestAggregateNss,
                         {shardKeyPattern.getKeyPattern().globalMin(), BSON("_id" << 0)},
                         version,
                         {"0"});
        chunk1.setName(OID::gen());
        version.incMinor();

        ChunkType chunk2(kTestAggregateNss,
                         {BSON("_id" << 0), shardKeyPattern.getKeyPattern().globalMax()},
                         version,
                         {"1"});
        chunk2.setName(OID::gen());
        version.incMinor();

        return std::vector<BSONObj>{chunk1.toConfigBSON(), chunk2.toConfigBSON()};
    }());

    // That error should be retried, but only the one on that shard.
    onCommand([&](const executor::RemoteCommandRequest& request) {
        return CursorResponse(kTestAggregateNss, CursorId{0}, {expectedResult.toBson()})
            .toBSON(CursorResponse::ResponseType::InitialResponse);
    });

    future.default_timed_get();
}

TEST_F(ShardedUnionTest, CorrectlySplitsSubPipelineIfRefreshedDistributionRequiresIt) {
    // Sharded by {_id: 1}, [MinKey, 0) on shard "0", [0, MaxKey) on shard "1".
    auto shards = setupNShards(2);
    loadRoutingTableWithTwoChunksAndTwoShards(kTestAggregateNss);

    auto&& parser = AccumulationStatement::getParser("$sum");
    auto accumulatorArg = BSON("" << 1);
    auto sumStatement =
        parser(expCtx(), accumulatorArg.firstElement(), expCtx()->variablesParseState);
    AccumulationStatement countStatement{"count", sumStatement};
    auto pipeline = Pipeline::create(
        {DocumentSourceMatch::create(fromjson("{_id: {$gte: 0}}"), expCtx()),
         DocumentSourceGroup::create(
             expCtx(), ExpressionConstant::create(expCtx(), Value(BSONNULL)), {countStatement})},
        expCtx());
    auto unionWith = DocumentSourceUnionWith(expCtx(), std::move(pipeline));
    expCtx()->mongoProcessInterface = std::make_shared<ShardServerProcessInterface>(executor());
    auto queue = DocumentSourceQueue::create(expCtx());
    unionWith.setSource(queue.get());

    auto expectedResult = Document{{"_id"_sd, BSONNULL}, {"count"_sd, 1}};

    auto future = launchAsync([&] {
        auto next = unionWith.getNext();
        ASSERT_TRUE(next.isAdvanced());
        auto result = next.releaseDocument();
        ASSERT_DOCUMENT_EQ(result, expectedResult);
        ASSERT(unionWith.getNext().isEOF());
        ASSERT(unionWith.getNext().isEOF());
        ASSERT(unionWith.getNext().isEOF());
    });

    // With the $match at the front of the sub-pipeline, we should be able to target the request to
    // just shard 1.  Mock out an error response from that shard, then expect a refresh of the
    // sharding catalog for that namespace.
    onCommand([&](const executor::RemoteCommandRequest& request) {
        ASSERT_EQ(request.target, HostAndPort(shards[1].getHost()));
        return Status{ErrorCodes::StaleShardVersion, "Mock error: shard version mismatch"};
    });

    // Mock the expected config server queries. Update the distribution as if a chunk [0, 10] was
    // created and moved to the first shard.
    const OID epoch = OID::gen();
    const ShardKeyPattern shardKeyPattern(BSON("_id" << 1));
    expectGetCollection(kTestAggregateNss, epoch, shardKeyPattern);
    expectFindSendBSONObjVector(kConfigHostAndPort, [&]() {
        ChunkVersion version(1, 0, epoch);

        ChunkType chunk1(kTestAggregateNss,
                         {shardKeyPattern.getKeyPattern().globalMin(), BSON("_id" << 0)},
                         version,
                         {shards[0].getName()});
        chunk1.setName(OID::gen());
        version.incMinor();

        ChunkType chunk2(kTestAggregateNss,
                         {BSON("_id" << 0), BSON("_id" << 10)},
                         version,
                         {shards[1].getName()});
        chunk2.setName(OID::gen());
        version.incMinor();

        ChunkType chunk3(kTestAggregateNss,
                         {BSON("_id" << 10), shardKeyPattern.getKeyPattern().globalMax()},
                         version,
                         {shards[0].getName()});
        chunk3.setName(OID::gen());

        return std::vector<BSONObj>{
            chunk1.toConfigBSON(), chunk2.toConfigBSON(), chunk3.toConfigBSON()};
    }());

    // That error should be retried, this time two shards.
    onCommand([&](const executor::RemoteCommandRequest& request) {
        return CursorResponse(
                   kTestAggregateNss, CursorId{0}, {BSON("_id" << BSONNULL << "count" << 1)})
            .toBSON(CursorResponse::ResponseType::InitialResponse);
    });
    onCommand([&](const executor::RemoteCommandRequest& request) {
        return CursorResponse(
                   kTestAggregateNss, CursorId{0}, {BSON("_id" << BSONNULL << "count" << 0)})
            .toBSON(CursorResponse::ResponseType::InitialResponse);
    });

    future.default_timed_get();
}

TEST_F(ShardedUnionTest, AvoidsSplittingSubPipelineIfRefreshedDistributionDoesNotRequire) {
    // Sharded by {_id: 1}, [MinKey, 0) on shard "0", [0, MaxKey) on shard "1".
    auto shards = setupNShards(2);
    loadRoutingTableWithTwoChunksAndTwoShards(kTestAggregateNss);

    auto&& parser = AccumulationStatement::getParser("$sum");
    auto accumulatorArg = BSON("" << 1);
    auto sumStatement =
        parser(expCtx(), accumulatorArg.firstElement(), expCtx()->variablesParseState);
    AccumulationStatement countStatement{"count", sumStatement};
    auto pipeline = Pipeline::create(
        {DocumentSourceGroup::create(
            expCtx(), ExpressionConstant::create(expCtx(), Value(BSONNULL)), {countStatement})},
        expCtx());
    auto unionWith = DocumentSourceUnionWith(expCtx(), std::move(pipeline));
    expCtx()->mongoProcessInterface = std::make_shared<ShardServerProcessInterface>(executor());
    auto queue = DocumentSourceQueue::create(expCtx());
    unionWith.setSource(queue.get());

    auto expectedResult = Document{{"_id"_sd, BSONNULL}, {"count"_sd, 1}};

    auto future = launchAsync([&] {
        auto next = unionWith.getNext();
        ASSERT_TRUE(next.isAdvanced());
        auto result = next.releaseDocument();
        ASSERT_DOCUMENT_EQ(result, expectedResult);
        ASSERT(unionWith.getNext().isEOF());
        ASSERT(unionWith.getNext().isEOF());
        ASSERT(unionWith.getNext().isEOF());
    });

    // Mock out an error response from both shards, then expect a refresh of the sharding catalog
    // for that namespace, then mock out a successful response.
    onCommand([&](const executor::RemoteCommandRequest& request) {
        return Status{ErrorCodes::StaleShardVersion, "Mock error: shard version mismatch"};
    });
    onCommand([&](const executor::RemoteCommandRequest& request) {
        return Status{ErrorCodes::StaleShardVersion, "Mock error: shard version mismatch"};
    });


    // Mock the expected config server queries. Update the distribution so that all chunks are on
    // the same shard.
    const OID epoch = OID::gen();
    const ShardKeyPattern shardKeyPattern(BSON("_id" << 1));
    expectGetCollection(kTestAggregateNss, epoch, shardKeyPattern);
    expectFindSendBSONObjVector(kConfigHostAndPort, [&]() {
        ChunkVersion version(1, 0, epoch);

        ChunkType chunk1(kTestAggregateNss,
                         {shardKeyPattern.getKeyPattern().globalMin(),
                          shardKeyPattern.getKeyPattern().globalMax()},
                         version,
                         {shards[0].getName()});
        chunk1.setName(OID::gen());

        return std::vector<BSONObj>{chunk1.toConfigBSON()};
    }());

    // That error should be retried, this time targetting only one shard.
    onCommand([&](const executor::RemoteCommandRequest& request) {
        ASSERT_EQ(request.target, HostAndPort(shards[0].getHost()));
        return CursorResponse(kTestAggregateNss, CursorId{0}, {expectedResult.toBson()})
            .toBSON(CursorResponse::ResponseType::InitialResponse);
    });

    future.default_timed_get();
}

TEST_F(ShardedUnionTest, IncorporatesViewDefinitionAndRetriesWhenViewErrorReceived) {
    // Sharded by {_id: 1}, [MinKey, 0) on shard "0", [0, MaxKey) on shard "1".
    auto shards = setupNShards(2);
    loadRoutingTableWithTwoChunksAndTwoShards(kTestAggregateNss);

    NamespaceString nsToUnionWith(expCtx()->ns.db(), "view");
    // Mock out the view namespace as emtpy for now - this is what it would be when parsing in a
    // sharded cluster - only later would we learn the actual view definition.
    expCtx()->setResolvedNamespaces(StringMap<ExpressionContext::ResolvedNamespace>{
        {nsToUnionWith.coll().toString(), {nsToUnionWith, std::vector<BSONObj>{}}}});
    auto bson = BSON("$unionWith" << nsToUnionWith.coll());
    auto unionWith = DocumentSourceUnionWith::createFromBson(bson.firstElement(), expCtx());
    expCtx()->mongoProcessInterface = std::make_shared<ShardServerProcessInterface>(executor());
    auto queue = DocumentSourceQueue::create(expCtx());
    unionWith->setSource(queue.get());

    NamespaceString expectedBackingNs(kTestAggregateNss);
    auto expectedResult = Document{{"_id"_sd, "unionResult"_sd}};
    auto expectToBeFiltered = Document{{"_id"_sd, "notTheUnionResult"_sd}};

    auto future = launchAsync([&] {
        auto next = unionWith->getNext();
        ASSERT_TRUE(next.isAdvanced());
        auto result = next.releaseDocument();
        ASSERT_DOCUMENT_EQ(result, expectedResult);
        ASSERT(unionWith->getNext().isEOF());
        ASSERT(unionWith->getNext().isEOF());
        ASSERT(unionWith->getNext().isEOF());
    });

    // Mock out one error response, then expect a refresh of the sharding catalog for that
    // namespace, then mock out a successful response.
    onCommand([&](const executor::RemoteCommandRequest& request) {
        return Status{ResolvedView{expectedBackingNs,
                                   {fromjson("{$group: {_id: '$groupKey'}}"),
                                    // Prevent the $match from being pushed into the shards where it
                                    // would not execute in this mocked environment.
                                    fromjson("{$_internalInhibitOptimization: {}}"),
                                    fromjson("{$match: {_id: 'unionResult'}}")},
                                   BSONObj()},
                      "It was a view!"_sd};
    });

    // That error should be incorporated, then we should target both shards. The results should be
    // de-duplicated in the merging part of the pipeline which performs the second half of the
    // $group, then the document which doesn't match the final $match should be filtered out.
    onCommand([&](const executor::RemoteCommandRequest& request) {
        return CursorResponse(expectedBackingNs,
                              CursorId{0},
                              {expectedResult.toBson(), expectToBeFiltered.toBson()})
            .toBSON(CursorResponse::ResponseType::InitialResponse);
    });
    onCommand([&](const executor::RemoteCommandRequest& request) {
        return CursorResponse(expectedBackingNs,
                              CursorId{0},
                              {expectedResult.toBson(), expectToBeFiltered.toBson()})
            .toBSON(CursorResponse::ResponseType::InitialResponse);
    });

    future.default_timed_get();
}

}  // namespace
}  // namespace mongo