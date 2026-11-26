/*
 * src/cuda_loader.hpp
 * SPDX-License-Identifier: MPL-2.0
 */
#pragma once

#include "_base.hpp"
#include "post_processors/processor.hpp"
#include "post_processors/transfer.hpp"

#include <memory>

std::unique_ptr<IFrameProcessor> create_cuda_processor(int width, int height, OutputFormat format);

std::unique_ptr<IOutputTransfer> create_cuda_output_transfer();
