#include "fdbserver/TagQueue.h"
#include "flow/UnitTest.h"
#include "flow/actorcompiler.h" // must be last include

void TagQueue::updateRates(std::map<TransactionTag, double> const& newRates) {
	for (const auto& [tag, rate] : newRates) {
		auto it = rateInfos.find(tag);
		if (it == rateInfos.end()) {
			rateInfos[tag] = GrvTransactionRateInfo(rate);
		} else {
			it->second.setRate(rate);
		}
	}

	for (const auto& [tag, _] : rateInfos) {
		if (newRates.find(tag) == newRates.end()) {
			rateInfos.erase(tag);
		}
	}
}

bool TagQueue::canStart(TransactionTag tag, int64_t count) const {
	auto it = rateInfos.find(tag);
	if (it == rateInfos.end()) {
		return true;
	}
	auto it2 = releasedInEpoch.find(tag);
	auto alreadyReleased = (it2 == releasedInEpoch.end() ? 0 : it2->second);
	return it->second.canStart(alreadyReleased, count);
}

bool TagQueue::canStart(GetReadVersionRequest req) const {
	if (req.priority == TransactionPriority::IMMEDIATE) {
		return true;
	}
	for (const auto& [tag, count] : req.tags) {
		if (!canStart(tag, count)) {
			return false;
		}
	}
	return true;
}

void TagQueue::addRequest(GetReadVersionRequest req) {
	newRequests.push_back(req);
}

void TagQueue::startEpoch() {
	for (auto& [_, rateInfo] : rateInfos) {
		rateInfo.startEpoch();
	}
	releasedInEpoch.clear();
}

void TagQueue::endEpoch(double elapsed) {
	for (auto& [tag, rateInfo] : rateInfos) {
		rateInfo.endEpoch(releasedInEpoch[tag], false, elapsed);
	}
}

void TagQueue::runEpoch(double elapsed,
                        SpannedDeque<GetReadVersionRequest>& outBatchPriority,
                        SpannedDeque<GetReadVersionRequest>& outDefaultPriority,
                        SpannedDeque<GetReadVersionRequest>& outImmediatePriority) {
	startEpoch();
	Deque<DelayedRequest> newDelayedRequests;
	while (!newRequests.empty()) {
		auto const& req = newRequests.front();
		if (canStart(req)) {
			for (const auto& [tag, count] : req.tags) {
				releasedInEpoch[tag] += count;
			}
			if (req.priority == TransactionPriority::BATCH) {
				outBatchPriority.push_back(req);
			} else if (req.priority == TransactionPriority::DEFAULT) {
				outDefaultPriority.push_back(req);
			} else if (req.priority == TransactionPriority::IMMEDIATE) {
				outImmediatePriority.push_back(req);
			} else {
				ASSERT(false);
			}
		} else {
			newDelayedRequests.emplace_back(req);
		}
		newRequests.pop_front();
	}

	while (!delayedRequests.empty()) {
		auto const& delayedReq = delayedRequests.front();
		auto const& req = delayedReq.req;
		if (canStart(req)) {
			for (const auto& [tag, count] : req.tags) {
				releasedInEpoch[tag] += count;
			}
			if (req.priority == TransactionPriority::BATCH) {
				outBatchPriority.push_back(req);
			} else if (req.priority == TransactionPriority::DEFAULT) {
				outDefaultPriority.push_back(req);
			} else if (req.priority == TransactionPriority::IMMEDIATE) {
				outImmediatePriority.push_back(req);
			} else {
				ASSERT(false);
			}
		} else {
			newDelayedRequests.push_back(delayedReq);
		}
		delayedRequests.pop_front();
	}

	delayedRequests = std::move(newDelayedRequests);
	endEpoch(elapsed);
}

ACTOR static Future<Void> mockClient(TagQueue* tagQueue,
                                     TransactionPriority priority,
                                     TransactionTag tag,
                                     double desiredRate,
                                     int64_t* count) {
	state Future<Void> timer;
	loop {
		timer = delayJittered(1.0 / desiredRate);
		GetReadVersionRequest req;
		req.tags[tag] = 1;
		req.priority = priority;
		tagQueue->addRequest(req);
		wait(success(req.reply.getFuture()) && timer);
		++(*count);
	}
}

ACTOR static Future<Void> mockServer(TagQueue* tagQueue) {
	state SpannedDeque<GetReadVersionRequest> outBatchPriority("TestTagQueue_Batch"_loc);
	state SpannedDeque<GetReadVersionRequest> outDefaultPriority("TestTagQueue_Default"_loc);
	state SpannedDeque<GetReadVersionRequest> outImmediatePriority("TestTagQueue_Immediate"_loc);
	loop {
		state double elapsed = (0.09 + 0.02 * deterministicRandom()->random01());
		wait(delay(elapsed));
		tagQueue->runEpoch(elapsed, outBatchPriority, outDefaultPriority, outImmediatePriority);
		while (!outBatchPriority.empty()) {
			outBatchPriority.front().reply.send(GetReadVersionReply{});
			outBatchPriority.pop_front();
		}
		while (!outDefaultPriority.empty()) {
			outDefaultPriority.front().reply.send(GetReadVersionReply{});
			outDefaultPriority.pop_front();
		}
		while (!outImmediatePriority.empty()) {
			outImmediatePriority.front().reply.send(GetReadVersionReply{});
			outImmediatePriority.pop_front();
		}
	}
}

TEST_CASE("/TagQueue/Simple") {
	state TagQueue tagQueue;
	state int64_t counter = 0;
	{
		std::map<TransactionTag, double> rates;
		rates["sampleTag"_sr] = 10.0;
		tagQueue.updateRates(rates);
	}

	state Future<Void> client = mockClient(&tagQueue, TransactionPriority::DEFAULT, "sampleTag"_sr, 20.0, &counter);
	state Future<Void> server = mockServer(&tagQueue);
	wait(timeout(client && server, 60.0, Void()));
	TraceEvent("TagQuotaTest").detail("Counter", counter);
	return Void();
}
