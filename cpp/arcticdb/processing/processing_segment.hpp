/* Copyright 2023 Man Group Operations Limited
 *
 * Use of this software is governed by the Business Source License 1.1 included in the file licenses/BSL.txt.
 *
 * As of the Change Date specified in that file, in accordance with the Business Source License, use of this software will be governed by the Apache License, version 2.0.
 */

#pragma once

#include <unordered_map>
#include <vector>

#include <fmt/core.h>

#include <arcticdb/async/task_scheduler.hpp>
#include <arcticdb/column_store/memory_segment.hpp>
#include <arcticdb/processing/expression_context.hpp>
#include <arcticdb/processing/expression_node.hpp>
#include <arcticdb/pipeline/frame_slice.hpp>
#include <arcticdb/pipeline/filter_segment.hpp>
#include <arcticdb/util/composite.hpp>
#include <arcticdb/util/string_utils.hpp>
#include <arcticdb/util/variant.hpp>

namespace arcticdb {
    enum class PipelineOptimisation : uint8_t {
        SPEED,
        MEMORY
    };

    /*
     * A processing segment is designed to be used in conjunction with the clause processing framework.
     * Each clause will execute in turn and will be passed the same mutable ProcessingSegment which it will have free
     * rein to modify prior to passing off to the next clause. In contract, clauses and the expression trees contained
     * within are immutable as they represent predefined behaviour.
     *
     * ProcessingSegment contains two main member attributes:
     *
     * ExpressionContext: This contains the expression tree for the currently running clause - each clause contains an
     * expression tree containing nodes that map to columns, constant values or other nodes in the tree. This allows
     * for the processing of expressions such as `(df['a'] + 4) < df['b']`. One instance of an expression context can be
     * shared across many ProcessingSegment's. See ExpressionContext class for more documentation.
     *
     * computed_data_: This contains a map from node name to previously computed result. Should an expression such as
     * df['col1'] + 1 appear multiple times in the tree this allows us to only perform the computation once and spill
     * the result to disk.
     */
    struct ProcessingSegment {
        // We want a collection of SliceAndKeys here so that we have all of the columns for a given row present in a single
        // ProcessingSegment, for 2 reasons:
        // 1: For binary operations in ExpressionNode involving 2 columns, we need both available in a single processing
        //    segment in order for "get" calls to make sense
        // 2: As "get" uses cached results where possible, it is not obvious which processing segment should hold the
        //    cached result for operations involving 2 columns (probably both). By moving these cached results up a level
        //    to contain all the information for a given row, this becomes unambiguous.
        std::vector<pipelines::SliceAndKey> data_;
        std::shared_ptr<ExpressionContext> expression_context_;
        std::unordered_map<std::string, VariantData> computed_data_;

        // Set by PartitioningClause
        std::optional<size_t> bucket_;

        ProcessingSegment() = default;

        ProcessingSegment(SegmentInMemory &&seg,
                          pipelines::FrameSlice&& slice,
                          std::optional<size_t> bucket = std::nullopt) :
                bucket_(bucket) {
            data_.emplace_back(pipelines::SliceAndKey{std::move(seg), std::move(slice)});
        }

        explicit ProcessingSegment(pipelines::SliceAndKey &&sk,
                                   std::optional<size_t> bucket = std::nullopt) :
                bucket_(bucket) {
            data_.emplace_back(pipelines::SliceAndKey{std::move(sk)});
        }

        explicit ProcessingSegment(std::vector<pipelines::SliceAndKey> &&sk,
                                   std::optional<size_t> bucket = std::nullopt) :
                bucket_(bucket) {
            data_ = std::move(sk);
        }

        explicit ProcessingSegment(SegmentInMemory &&seg,
                                   std::optional<size_t> bucket = std::nullopt) :
                bucket_(bucket) {
            data_.emplace_back(pipelines::SliceAndKey{std::move(seg)});
        }

        std::vector<pipelines::SliceAndKey>&& release_data() {
            return std::move(data_);
        }

        std::vector<pipelines::SliceAndKey>& data() {
            return data_;
        }

        const std::vector<pipelines::SliceAndKey>& data() const {
            return data_;
        }

        ProcessingSegment &self() {
            return *this;
        }

        size_t get_bucket(){
            return bucket_.value();
        }

        void set_bucket(size_t bucket) {
            bucket_ = bucket;
        }

        void apply_filter(const util::BitSet& bitset, const std::shared_ptr<Store>& store, PipelineOptimisation optimisation);

        void set_expression_context(const std::shared_ptr<ExpressionContext>& expression_context) {
            expression_context_ = expression_context;
        }

        // The name argument to this function is either a column/value name, or uniquely identifies an ExpressionNode object.
        // If this function has been called before with the same ExpressionNode name, then we cache the result in the
        // computed_data_ map to avoid duplicating work.
        VariantData get(const VariantNode &name, const std::shared_ptr<Store> &store);
    };

    inline std::vector<pipelines::SliceAndKey> collect_segments(Composite<ProcessingSegment>&& p) {
        auto procs = std::move(p);
        std::vector<pipelines::SliceAndKey> output;

        procs.broadcast([&output] (auto&& p) {
            auto proc = std::forward<ProcessingSegment>(p);
            auto slice_and_keys = proc.release_data();
            std::move(std::begin(slice_and_keys), std::end(slice_and_keys), std::back_inserter(output));
        });

        return output;
    }

    template<typename Grouper, typename Bucketizer>
    std::vector<std::optional<size_t>> get_buckets(const ColumnWithStrings& col, std::shared_ptr<Grouper> grouper, std::shared_ptr<Bucketizer> bucketizer) {
        auto input_data = col.column_->data();

        std::vector<std::optional<size_t>> output;
        output.reserve(col.column_->row_count());
        using TypeDescriptorTag = typename Grouper::GrouperDescriptor;
        using RawType = typename TypeDescriptorTag::DataTypeTag::raw_type;

        while (auto block = input_data.next<TypeDescriptorTag>()) {
            const auto row_count = block->row_count();
            auto ptr = reinterpret_cast<const RawType*>(block->data());
            for(auto i = 0u; i < row_count; ++i, ++ptr){
                if constexpr(std::is_same_v<typename Grouper::GrouperDescriptor, TypeDescriptorTag>) {
                    auto opt_group = grouper->group(*ptr, col.string_pool_);
                    if (opt_group.has_value()) {
                        output.emplace_back(bucketizer->bucket(*opt_group));
                    } else {
                        output.emplace_back(std::nullopt);
                    }
                }
            }
        }
        return output;
    }

    template<typename GrouperType, typename BucketizerType>
    Composite<ProcessingSegment> partition_processing_segment(
            const std::shared_ptr<Store>& store,
            ProcessingSegment& input,
            const ColumnName& grouping_column_name,
            bool dynamic_schema) {
        Composite<ProcessingSegment> output;
        auto get_result = input.get(ColumnName(grouping_column_name), store);
        if (std::holds_alternative<ColumnWithStrings>(get_result)) {
            auto partitioning_column = std::get<ColumnWithStrings>(get_result);
            partitioning_column.column_->type().visit_tag([&output, &input, &partitioning_column, &store](auto type_desc_tag) {
                using TypeDescriptorTag = decltype(type_desc_tag);
                using ResolvedGrouperType = typename GrouperType::template Grouper<TypeDescriptorTag>;

                auto grouper = std::make_shared<ResolvedGrouperType>(ResolvedGrouperType());
                auto num_buckets = ConfigsMap::instance()->get_int("Partition.NumBuckets", async::TaskScheduler::instance()->cpu_thread_count());
                auto bucketizer = std::make_shared<BucketizerType>(num_buckets);
                auto bucket_vec = get_buckets(partitioning_column, grouper, bucketizer);
                std::vector<util::BitSet> bitsets;
                bitsets.resize(bucketizer->num_buckets());
                std::vector<util::BitSet::bulk_insert_iterator> iterators;
                for(auto& bitset : bitsets)
                    iterators.emplace_back(util::BitSet::bulk_insert_iterator(bitset));

                for(auto val : folly::enumerate(bucket_vec))
                    if (val->has_value())
                        iterators[val->value()] = val.index;

                for(auto& iterator : iterators)
                    iterator.flush();

                for(auto bitset : folly::enumerate(bitsets)) {
                    if(bitset->count() != 0) {
                        ProcessingSegment proc;
                        proc.set_bucket(bitset.index);
                        for (auto& seg_slice_and_key : input.data()) {
                            const SegmentInMemory& seg = seg_slice_and_key.segment(store);
                            bitset->resize(seg.row_count());
                            auto new_seg = filter_segment(seg, *bitset);
                            pipelines::FrameSlice new_slice{seg_slice_and_key.slice()};
                            new_slice.adjust_rows(new_seg.row_count());

                            proc.data().emplace_back(pipelines::SliceAndKey{std::move(new_seg), std::move(new_slice)});
                        }
                        output.push_back(std::move(proc));
                    }
                }
            });
        } else {
            internal::check<ErrorCode::E_ASSERTION_FAILURE>(
                    dynamic_schema,
                    "Grouping column missing from row-slice in static schema symbol"
            );
        }
        return output;
    }

} //namespace arcticdb