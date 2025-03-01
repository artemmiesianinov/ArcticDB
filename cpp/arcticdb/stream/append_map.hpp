/* Copyright 2023 Man Group Operations Limited
 *
 * Use of this software is governed by the Business Source License 1.1 included in the file licenses/BSL.txt.
 *
 * As of the Change Date specified in that file, in accordance with the Business Source License, use of this software will be governed by the Apache License, version 2.0.
 */

#pragma once

#include <arcticdb/entity/types.hpp>
#include <arcticdb/entity/atom_key.hpp>
#include <arcticdb/pipeline/frame_slice.hpp>
#include <arcticdb/stream/stream_source.hpp>
#include <arcticdb/storage/store.hpp>
#include <arcticdb/pipeline/input_tensor_frame.hpp>

namespace arcticdb::pipelines {

struct PythonOutputFrame;
struct InputTensorFrame;
using FilterRange = std::variant<std::monostate, IndexRange, RowRange>;

}

namespace arcticdb::stream {
StreamDescriptor merge_descriptors(
    const StreamDescriptor &original,
    const std::vector<StreamDescriptor::FieldsCollection> &entries,
    const std::unordered_set<std::string_view> &filtered_set,
    const std::optional<IndexDescriptor>& default_index);

entity::StreamDescriptor merge_descriptors(
    const entity::StreamDescriptor &original,
    const std::vector<StreamDescriptor::FieldsCollection> &entries,
    const std::vector<std::string> &filtered_columns,
    const std::optional<entity::IndexDescriptor>& default_index = std::nullopt);

entity::StreamDescriptor merge_descriptors(
    const entity::StreamDescriptor &original,
    const std::vector<pipelines::SliceAndKey> &entries,
    const std::vector<std::string> &filtered_columns,
    const std::optional<entity::IndexDescriptor>& default_index = std::nullopt);

entity::StreamDescriptor merge_descriptors(
    const std::shared_ptr<Store>& store,
    const entity::StreamDescriptor &original,
    const std::vector<pipelines::SliceAndKey> &entries,
    const std::unordered_set<std::string_view> &filtered_set,
    const std::optional<entity::IndexDescriptor>& default_index = std::nullopt);

std::pair<std::optional<entity::AtomKey>, size_t> read_head(
    const std::shared_ptr<stream::StreamSource>& store,
    StreamId stream_id);

std::set<StreamId> get_incomplete_refs(const std::shared_ptr<Store>& store);

std::set<StreamId> get_incomplete_symbols(const std::shared_ptr<Store>& store);

std::set<StreamId> get_active_incomplete_refs(const std::shared_ptr<Store>& store);

std::vector<pipelines::SliceAndKey> get_incomplete(
    const std::shared_ptr<Store> &store,
    const StreamId &stream_id,
    const pipelines::FilterRange &range,
    uint64_t last_row,
    bool via_iteration,
    bool load_data);

void remove_incomplete_segments(
    const std::shared_ptr<Store>& store,
    const StreamId& stream_id);

folly::Future<entity::VariantKey> write_incomplete_frame(
    const std::shared_ptr<Store>& store,
    const StreamId& stream_id,
    pipelines::InputTensorFrame&& frame,
    std::optional<AtomKey>&& next_key);

void write_parallel(
    const std::shared_ptr<Store>& store,
    const StreamId& stream_id,
    pipelines::InputTensorFrame&& frame);

void write_head(
    const std::shared_ptr<Store>& store,
    const AtomKey& next_key,
    size_t total_rows);

void append_incomplete_segment(
    const std::shared_ptr<Store>& store,
    const StreamId& stream_id,
    SegmentInMemory &&seg);

void append_incomplete(
    const std::shared_ptr<Store>& store,
    const StreamId& stream_id,
    pipelines::InputTensorFrame&& frame);

std::optional<int64_t> latest_incomplete_timestamp(
    const std::shared_ptr<Store>& store,
    const StreamId& stream_id
    );

} //namespace arcticdb::stream
