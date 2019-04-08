/* Copyright (c) 2016 PaddlePaddle Authors. All Rights Reserved.

Licensed under the Apache License, Version 2.0 (the "License");
you may not use this file except in compliance with the License.
You may obtain a copy of the License at

   http://www.apache.org/licenses/LICENSE-2.0

Unless required by applicable law or agreed to in writing, software
distributed under the License is distributed on an "AS IS" BASIS,
WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
See the License for the specific language governing permissions and
limitations under the License. */

#pragma once
#include <algorithm>
#include <vector>
#include "framework/lod_tensor.h"
#include "framework/tensor.h"

namespace paddle_mobile {
namespace operators {
namespace math {
template <typename T>
class CopyMatrixRowsFunctor {
public:
  void operator()(const framework::Tensor& src, std::vector<size_t> index_lod,
                  framework::Tensor* dst, bool is_src_index) {
    size_t* index = index_lod.data();
    auto src_dims = src.dims();
    auto dst_dims = dst->dims();
    PADDLE_MOBILE_ENFORCE((src_dims.size() == 2UL),
                          "The src must be matrix with rank 2.");
    PADDLE_MOBILE_ENFORCE((dst_dims.size() == 2UL),
                          "The dst must be matrix with rank 2.");
    PADDLE_MOBILE_ENFORCE((src_dims[1] == dst_dims[1]),
                          "The width of src and dst must be same.");
    auto height = dst_dims[0];
    auto width = dst_dims[1];
    auto* src_data = src.data<T>();
    auto* dst_data = dst->data<T>();
    for (int i = 0; i < height; ++i) {
      if (is_src_index) {
        memcpy(dst_data + i * width, src_data + index[i] * width,
               width * sizeof(T));
      } else {
        memcpy(dst_data + index[i] * width, src_data + i * width,
               width * sizeof(T));
      }
    }
  }
};


template <typename T>
class LoDTensor2BatchFunctor {
  // Calculate the length of each sequence and
  // sort sequence index by the length.
  // example:  sequences = {s0, s1, s2}
  //           s0: 0 0 0 0, s1: 1 1 1 1 1, s2: 2 2 2
  //           seq_info[3] = {(4, 5, 1), (0, 4, 0), (9, 3, 2)}
  //
  struct SeqInfo {
    SeqInfo(int start, int length, int seq_idx)
        : start(start), length(length), seq_idx(seq_idx) {}
    int start;
    int length;
    int seq_idx;
  };

 public:
  void operator()(const framework::LoDTensor& lod_tensor,
                  framework::LoDTensor* batch, bool is_cal_batch_lod,
                  bool is_reverse = false) {
    if (!is_cal_batch_lod) {
      auto lods = batch->lod();
      PADDLE_MOBILE_ENFORCE(
          (lods.size() > 2UL),
          "The LoD of LoDTensor should inlcude at least 2-level "
          "sequence information.");
      PADDLE_MOBILE_ENFORCE(
          (lods[1].size() == static_cast<size_t>(lod_tensor.dims()[0])),
          "The LoD information should be consistent with the dims.");
      CopyMatrixRowsFunctor<T> to_batch;
      to_batch(lod_tensor, lods[1], batch, true);
      return;
    }

    auto lods = lod_tensor.lod();
    PADDLE_MOBILE_ENFORCE((lods.size() == 1UL),
                          "Only support 1 level sequence, but %d is given",
                          lods.size());

    const auto& lod = lods[0];
    std::vector<SeqInfo> seq_info;
    for (size_t seq_id = 0; seq_id < lod.size() - 1; ++seq_id) {
      int length = lod[seq_id + 1] - lod[seq_id];
      seq_info.emplace_back(lod[seq_id], length, seq_id);
    }

    std::sort(seq_info.begin(), seq_info.end(),
              [](SeqInfo a, SeqInfo b) { return a.length > b.length; });

    // Calculate the start position of each batch.
    // example:  sequences = {s0, s1, s2}
    //           s0: 0 0 0 0, s1: 1 1 1 1 1, s2: 2 2 2
    //           num_batch = 5,
    //           batchIndex = {b0, b1, b2, b3, b4}
    //           b0: 1 0 2, b1: 1 0 2, b2: 1 0 2, b3: 1 0, b4: 1
    //           batch_start_positions[6] = {0, 3, 6, 9, 11, 12}
    //              batch_start_positions[0] = len(b0)
    //              batch_start_positions[1] = len(b0) + len(b1)
    //              batch_start_positions[2] = len(b0) + len(b1) + len(b2)
    //              ...
    //           seq2batch_idx[12] = {4, 0, 9,
    //                                5, 1, 10,
    //                                6, 2, 11,
    //                                7, 3,
    //                                8}
    //           seq_order = {1, 0, 2}, the sort order.
    //               where 1 is the second sequence,
    //                     0 is the first sequence,
    //                     2 is the third sequence.
    // The num_batch represents batch size after rearranging the
    // input LodTensor. It is also the maximum length of input sequence.

    framework::LoD batch_lods;
    batch_lods.emplace_back(std::vector<size_t>{0});
    batch_lods.emplace_back(std::vector<size_t>{0});
    batch_lods.emplace_back(std::vector<size_t>{0});

    // batch_lods[0] is the start positions for batch LoDTensor
    int num_batch = seq_info[0].length;
    batch_lods[0].resize(static_cast<size_t>(num_batch + 1));
    // batch_lods[1] is the raw index in the input LoDTensor
    batch_lods[1].resize(static_cast<size_t>(lod_tensor.dims()[0]));
    // batch_lods[2] is the sort order for the input LoDTensor.
    batch_lods[2].resize(seq_info.size());

    size_t* batch_starts = batch_lods[0].data();
    size_t* seq2batch_idx = batch_lods[1].data();
    batch_starts[0] = 0;
    for (int n = 0; n < num_batch; n++) {
      auto batch_id = static_cast<int>(batch_starts[n]);
      for (size_t i = 0; i < seq_info.size(); ++i) {
        int seq_len = seq_info[i].length;
        int start = seq_info[i].start;
        if (n < seq_len) {
          seq2batch_idx[batch_id] =
              is_reverse ? start + seq_len - 1 - n : start + n;
          batch_id++;
        } else {
          break;
        }
      }
      batch_starts[n + 1] = static_cast<size_t>(batch_id);
    }
    size_t* seq_order = batch_lods[2].data();
    for (size_t i = 0; i < seq_info.size(); ++i) {
      seq_order[i] = seq_info[i].seq_idx;
    }
    batch->set_lod(batch_lods);

    CopyMatrixRowsFunctor<T> to_batch;
    to_batch(lod_tensor, batch_lods[1], batch, true);
  }
};

template <typename T>
class Batch2LoDTensorFunctor {
 public:
  void operator()(const framework::LoDTensor& batch,
                  framework::LoDTensor* lod_tensor) {
    auto in_lod = batch.lod();
    PADDLE_MOBILE_ENFORCE(
        (in_lod.size() > 2UL),
        "The LoD of LoDTensor should inlcude at least 2-level "
        "sequence information.");
    PADDLE_MOBILE_ENFORCE(
        (in_lod[1].size() == static_cast<size_t>(lod_tensor->dims()[0])),
        "The LoD information should be consistent with the dims.");
    CopyMatrixRowsFunctor<T> to_seq;
    to_seq(batch, in_lod[1], lod_tensor, false);
  }
};
}  // namespace math
}  // namespace operators
}  // namespace paddle_mobile
