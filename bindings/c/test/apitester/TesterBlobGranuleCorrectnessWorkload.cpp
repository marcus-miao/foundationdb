/*
 * TesterBlobGranuleCorrectnessWorkload.cpp
 *
 * This source file is part of the FoundationDB open source project
 *
 * Copyright 2013-2022 Apple Inc. and the FoundationDB project authors
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */
#include "TesterApiWorkload.h"
#include "TesterUtil.h"
#include <memory>
#include <fmt/format.h>

namespace FdbApiTester {

class ApiBlobGranuleCorrectnessWorkload : public ApiWorkload {
public:
	ApiBlobGranuleCorrectnessWorkload(const WorkloadConfig& config) : ApiWorkload(config) {
		// sometimes don't do range clears
		if (Random::get().randomInt(0, 1) == 0) {
			excludedOpTypes.push_back(OP_CLEAR_RANGE);
		}
	}

private:
	enum OpType { OP_INSERT, OP_CLEAR, OP_CLEAR_RANGE, OP_READ, OP_GET_RANGES, OP_LAST = OP_GET_RANGES };
	std::vector<OpType> excludedOpTypes;

	void randomReadOp(TTaskFct cont) {
		std::string begin = randomKeyName();
		std::string end = randomKeyName();
		auto results = std::make_shared<std::vector<KeyValue>>();
		if (begin > end) {
			std::swap(begin, end);
		}
		execTransaction(
		    [begin, end, results](auto ctx) {
			    ctx->tx()->setOption(FDB_TR_OPTION_READ_YOUR_WRITES_DISABLE);
			    KeyValuesResult res = ctx->tx()->readBlobGranules(begin, end, ctx->getBGBasePath());
			    bool more;
			    (*results) = res.getKeyValues(&more);
			    ASSERT(!more);
			    if (res.getError() != error_code_success) {
				    ctx->onError(res.getError());
			    } else {
				    ctx->done();
			    }
		    },
		    [this, begin, end, results, cont]() {
			    std::vector<KeyValue> expected = store.getRange(begin, end, store.size(), false);
			    if (results->size() != expected.size()) {
				    error(fmt::format("randomReadOp result size mismatch. expected: {} actual: {}",
				                      expected.size(),
				                      results->size()));
			    }
			    ASSERT(results->size() == expected.size());

			    for (int i = 0; i < results->size(); i++) {
				    if ((*results)[i].key != expected[i].key) {
					    error(fmt::format("randomReadOp key mismatch at {}/{}. expected: {} actual: {}",
					                      i,
					                      results->size(),
					                      expected[i].key,
					                      (*results)[i].key));
				    }
				    ASSERT((*results)[i].key == expected[i].key);

				    if ((*results)[i].value != expected[i].value) {
					    error(
					        fmt::format("randomReadOp value mismatch at {}/{}. key: {} expected: {:.80} actual: {:.80}",
					                    i,
					                    results->size(),
					                    expected[i].key,
					                    expected[i].value,
					                    (*results)[i].value));
				    }
				    ASSERT((*results)[i].value == expected[i].value);
			    }
			    schedule(cont);
		    });
	}

	void randomGetRangesOp(TTaskFct cont) {
		std::string begin = randomKeyName();
		std::string end = randomKeyName();
		auto results = std::make_shared<std::vector<KeyValue>>();
		if (begin > end) {
			std::swap(begin, end);
		}
		execTransaction(
		    [begin, end, results](auto ctx) {
			    KeyRangesFuture f = ctx->tx()->getBlobGranuleRanges(begin, end);
			    ctx->continueAfter(
			        f,
			        [ctx, f, results]() {
				        (*results) = f.getKeyRanges();
				        ctx->done();
			        },
			        true);
		    },
		    [this, begin, end, results, cont]() {
			    ASSERT(results->size() > 0);
			    ASSERT(results->front().key <= begin);
			    ASSERT(results->back().value >= end);

			    for (int i = 0; i < results->size(); i++) {
				    // no empty or inverted ranges
				    ASSERT((*results)[i].key < (*results)[i].value);
			    }

			    for (int i = 1; i < results->size(); i++) {
				    // ranges contain entire requested key range
				    ASSERT((*results)[i].key == (*results)[i - 1].value);
			    }

			    schedule(cont);
		    });
	}

	void randomOperation(TTaskFct cont) {
		OpType txType = (store.size() == 0) ? OP_INSERT : (OpType)Random::get().randomInt(0, OP_LAST);
		while (std::count(excludedOpTypes.begin(), excludedOpTypes.end(), txType)) {
			txType = (OpType)Random::get().randomInt(0, OP_LAST);
		}
		switch (txType) {
		case OP_INSERT:
			randomInsertOp(cont);
			break;
		case OP_CLEAR:
			randomClearOp(cont);
			break;
		case OP_CLEAR_RANGE:
			randomClearRangeOp(cont);
			break;
		case OP_READ:
			randomReadOp(cont);
			break;
		case OP_GET_RANGES:
			randomGetRangesOp(cont);
			break;
		}
	}
};

WorkloadFactory<ApiBlobGranuleCorrectnessWorkload> ApiBlobGranuleCorrectnessWorkloadFactory(
    "ApiBlobGranuleCorrectness");

} // namespace FdbApiTester